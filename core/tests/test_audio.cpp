// Growing-WAV reader, piece planner and transcription prompt (no model).
#include "check.h"
#include "sve_audio.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <unistd.h>
#include <vector>

using namespace sve;

static std::string tmp_path(const char *name) {
    const char *d = getenv("TMPDIR");
    std::string p = std::string(d && *d ? d : "/tmp/") + "/sve_test_audio_" + std::to_string(getpid()) + "_" + name;
    return p;
}

static void put32(std::string &s, uint32_t v) { for (int i = 0; i < 4; ++i) s += (char)((v >> (8 * i)) & 0xFF); }
static void put16(std::string &s, uint16_t v) { s += (char)(v & 0xFF); s += (char)(v >> 8); }

// A WAV with `frames` frames of a ramp; data size field = data_size_field (or the real size when ~0ull).
// extra: a chunk before "data" (recorders add FLLR padding). trailing_bytes: an odd half frame at the end.
static std::string make_wav(uint32_t frames, int channels, uint64_t data_size_field, bool extra, int trailing_bytes = 0,
                            uint32_t rate = 16000) {
    std::string s = "RIFF";
    put32(s, 0);
    s += "WAVE";
    s += "fmt ";
    put32(s, 16);
    put16(s, 1);
    put16(s, (uint16_t)channels);
    put32(s, rate);
    put32(s, rate * 2 * channels);
    put16(s, (uint16_t)(2 * channels));
    put16(s, 16);
    if (extra) { s += "FLLR"; put32(s, 6); s += std::string(6, '\0'); }
    s += "data";
    const uint32_t real = frames * 2 * channels;
    put32(s, data_size_field == ~0ull ? real : (uint32_t)data_size_field);
    for (uint32_t i = 0; i < frames; ++i)
        for (int c = 0; c < channels; ++c) put16(s, (uint16_t)(int16_t)((i % 100) * 100));
    s += std::string(trailing_bytes, '\x7f');
    return s;
}

static std::string write_file(const char *name, const std::string &bytes) {
    std::string p = tmp_path(name);
    std::ofstream(p, std::ios::binary) << bytes;
    return p;
}

TEST(wav_complete_header) {
    std::string p = write_file("a.wav", make_wav(1000, 1, ~0ull, false));
    WavInfo info;
    CHECK(wav_probe(p, info) == WavProbe::ok);
    CHECK_EQ(info.frames, 1000u);
    CHECK_EQ(info.data_offset, 44u);
    std::vector<float> pcm;
    CHECK(read_wav16k(p, pcm));
    CHECK_EQ(pcm.size(), 1000u);
    CHECK(std::fabs(pcm[5] - 500 / 32768.0f) < 1e-6f);
    remove(p.c_str());
}

TEST(wav_growing_size_zero_uses_disk) {
    // Header written at start, data size still 0, odd trailing byte (half a frame) not counted.
    std::string p = write_file("b.wav", make_wav(1234, 1, 0, true, 1));
    WavInfo info;
    CHECK(wav_probe(p, info) == WavProbe::ok);
    CHECK_EQ(info.frames, 1234u);
    CHECK_EQ(info.data_offset, 44u + 14u);
    remove(p.c_str());
}

TEST(wav_growing_size_ffffffff_and_too_large) {
    std::string p = write_file("c.wav", make_wav(500, 1, 0xFFFFFFFFu, false));
    WavInfo info;
    CHECK(wav_probe(p, info) == WavProbe::ok);
    CHECK_EQ(info.frames, 500u);
    remove(p.c_str());
    p = write_file("d.wav", make_wav(500, 1, 10'000'000, false));
    CHECK(wav_probe(p, info) == WavProbe::ok);
    CHECK_EQ(info.frames, 500u);
    remove(p.c_str());
}

TEST(wav_stereo_whole_frames_and_range) {
    // 3 trailing bytes: one sample of a frame plus half a sample -> not a whole frame.
    std::string p = write_file("e.wav", make_wav(300, 2, 0, false, 3));
    WavInfo info;
    CHECK(wav_probe(p, info) == WavProbe::ok);
    CHECK_EQ(info.channels, 2);
    CHECK_EQ(info.frames, 300u);
    std::vector<float> out;
    CHECK(wav_read(p, info, 250, 100, out));
    CHECK_EQ(out.size(), 50u);   // clamped to what is there
    CHECK(std::fabs(out[0] - 5000 / 32768.0f) < 1e-6f);   // frame 250: 50 * 100
    CHECK(wav_read(p, info, 400, 10, out));
    CHECK(out.empty());
    remove(p.c_str());
}

TEST(wav_incomplete_and_bad) {
    WavInfo info;
    std::string full = make_wav(10, 1, 0, false);
    std::string p = write_file("f.wav", full.substr(0, 8));
    CHECK(wav_probe(p, info) == WavProbe::incomplete);   // shorter than the RIFF header
    remove(p.c_str());
    p = write_file("g.wav", full.substr(0, 30));
    CHECK(wav_probe(p, info) == WavProbe::incomplete);   // fmt there, data chunk not yet
    remove(p.c_str());
    p = write_file("h.wav", make_wav(10, 1, 0, false, 0, 44100));
    CHECK(wav_probe(p, info) == WavProbe::bad);          // wrong rate
    remove(p.c_str());
    p = write_file("i.wav", "not a wav file at all, really");
    CHECK(wav_probe(p, info) == WavProbe::bad);
    remove(p.c_str());
    CHECK(wav_probe(tmp_path("missing.wav"), info) == WavProbe::bad);
}

TEST(wav_header_only_has_zero_frames) {
    std::string p = write_file("j.wav", make_wav(0, 1, 0, false));
    WavInfo info;
    CHECK(wav_probe(p, info) == WavProbe::ok);
    CHECK_EQ(info.frames, 0u);
    remove(p.c_str());
}

// Loud audio with one quiet 0.1 s window centred at `quiet_at` samples.
static std::vector<float> loud_with_gap(size_t n, size_t quiet_at) {
    std::vector<float> v(n, 0.5f);
    for (size_t i = quiet_at - kQuietWindow / 2; i < quiet_at + kQuietWindow / 2 && i < n; ++i) v[i] = 0;
    return v;
}

TEST(plan_wait_while_recording) {
    std::vector<float> pcm(30 * kAudioRate, 0.5f);
    CHECK(plan_next_piece(pcm, 0, 0, pcm.size(), false).kind == PiecePlan::wait);           // 30 s < 31 s
    CHECK(plan_next_piece(pcm, 0, 0, 0, false).kind == PiecePlan::wait);                    // nothing yet
    std::vector<float> more(31 * kAudioRate - 1, 0.5f);
    CHECK(plan_next_piece(more, 0, 0, more.size(), false).kind == PiecePlan::wait);         // one sample short
}

TEST(plan_cut_at_quietest_point) {
    const size_t quiet = 26 * kAudioRate + 800;   // inside the 25..28 s window, on the 800-sample grid
    std::vector<float> pcm = loud_with_gap(31 * kAudioRate, quiet);
    PiecePlan p = plan_next_piece(pcm, 0, 0, pcm.size(), false);
    CHECK(p.kind == PiecePlan::piece);
    CHECK_EQ(p.start, 0u);
    CHECK_EQ(p.stop, quiet);
    // Same cut when final (more than 28 s left).
    PiecePlan f = plan_next_piece(pcm, 0, 0, pcm.size(), true);
    CHECK(f.kind == PiecePlan::piece);
    CHECK_EQ(f.stop, quiet);
}

TEST(plan_offset_window_matches_whole_buffer) {
    // The incremental path reads only [start, start + 28 s); the cut must be the same as on the whole file.
    const size_t start = 40 * kAudioRate, quiet = start + 25 * kAudioRate + 2400;
    std::vector<float> whole = loud_with_gap(start + 60 * kAudioRate, quiet);
    std::vector<float> window(whole.begin() + start, whole.begin() + start + kPieceTarget);
    PiecePlan a = plan_next_piece(whole, 0, start, whole.size(), false);
    PiecePlan b = plan_next_piece(window, start, start, whole.size(), false);
    CHECK(a.kind == PiecePlan::piece && b.kind == PiecePlan::piece);
    CHECK_EQ(a.stop, quiet);
    CHECK_EQ(b.stop, a.stop);
    CHECK_EQ(b.start, start);
}

TEST(plan_final_takes_the_rest) {
    std::vector<float> pcm(20 * kAudioRate, 0.5f);
    PiecePlan p = plan_next_piece(pcm, 0, 0, pcm.size(), true);
    CHECK(p.kind == PiecePlan::piece);
    CHECK_EQ(p.stop, pcm.size());
    std::vector<float> exact(kPieceTarget, 0.5f);   // exactly 28 s: one piece, no cut
    CHECK_EQ(plan_next_piece(exact, 0, 0, exact.size(), true).stop, exact.size());
    std::vector<float> after(40 * kAudioRate, 0.5f);   // the tail after an earlier cut
    PiecePlan t = plan_next_piece(after, 0, 27 * kAudioRate, after.size(), true);
    CHECK(t.kind == PiecePlan::piece);
    CHECK_EQ(t.start, 27 * kAudioRate);
    CHECK_EQ(t.stop, after.size());
}

TEST(plan_end_cases) {
    std::vector<float> pcm(10 * kAudioRate, 0.5f);
    CHECK(plan_next_piece(pcm, 0, pcm.size(), pcm.size(), true).kind == PiecePlan::end);            // nothing left
    CHECK(plan_next_piece(pcm, 0, pcm.size() - kPieceMin + 1, pcm.size(), true).kind == PiecePlan::end);   // under 0.5 s
    CHECK(plan_next_piece(pcm, 0, pcm.size() - kPieceMin, pcm.size(), true).kind == PiecePlan::piece);     // exactly 0.5 s
    CHECK(plan_next_piece(pcm, 0, 0, 0, true).kind == PiecePlan::end);                                       // empty file
    CHECK(plan_next_piece(pcm, 0, pcm.size() + 5, pcm.size(), true).kind == PiecePlan::end);                 // past the end
    // Not final: nothing left is a wait, never an end.
    CHECK(plan_next_piece(pcm, 0, pcm.size(), pcm.size(), false).kind == PiecePlan::wait);
}

TEST(plan_walk_covers_everything_once) {
    // Final walk over 100 s: pieces are contiguous, each <= 28 s, the last ends at the end.
    std::vector<float> pcm(100 * kAudioRate);
    for (size_t i = 0; i < pcm.size(); ++i) pcm[i] = (float)std::sin(i * 0.01) * (0.2f + (i % 7919) / 7919.0f);
    size_t pos = 0;
    int n = 0;
    for (;; ++n) {
        PiecePlan p = plan_next_piece(pcm, 0, pos, pcm.size(), true);
        if (p.kind == PiecePlan::end) break;
        CHECK(p.kind == PiecePlan::piece);
        CHECK_EQ(p.start, pos);
        CHECK(p.stop > p.start && p.stop - p.start <= kPieceTarget);
        pos = p.stop;
        if (n > 10) break;
    }
    CHECK_EQ(pos, pcm.size());
    CHECK_EQ(n, 4);
}

TEST(instruction_has_vocab_and_tail) {
    std::string base = transcription_instruction("<__media__>", "", "");
    CHECK(base.rfind("<__media__>\nTranscribe this audio exactly as spoken.", 0) == 0);
    CHECK(base.find("previous part") == std::string::npos);
    std::string v = transcription_instruction("<m>", "Words to spell this way: Telma", "");
    CHECK(v.find("\nWords to spell this way: Telma.") != std::string::npos);
    std::string prev = std::string(500, 'a') + "తెలుగు";   // 18 bytes of Telugu at the end
    std::string t = transcription_instruction("<m>", "", prev);
    const std::string lead = "\nFor context only, the previous part ended with: \"";
    size_t at = t.find(lead);
    CHECK(at != std::string::npos);
    std::string tail = t.substr(at + lead.size());
    tail.pop_back();   // closing quote
    CHECK_EQ(tail.size(), 400u);
    CHECK(tail.find("తెలుగు") != std::string::npos);
    // A cut inside a UTF-8 sequence drops the continuation bytes.
    std::string mid = std::string(399, 'b');
    std::string p2 = "x" + std::string("త") + mid;   // 1 + 3 + 399 bytes; the last 400 start inside "త"
    std::string t2 = transcription_instruction("<m>", "", p2);
    std::string tail2 = t2.substr(t2.find(lead) + lead.size());
    tail2.pop_back();
    CHECK_EQ(tail2, mid);
}
