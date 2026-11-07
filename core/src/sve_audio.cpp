#include "sve_audio.h"

#include <algorithm>
#include <cstring>
#include <fstream>

namespace sve {

const char *const kTranscriberSystem =
    "You are a precise transcriber. Speakers often mix languages, for example Telugu with English words.";

static uint32_t le32(const unsigned char *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t le16(const unsigned char *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

WavProbe wav_probe(const std::string &path, WavInfo &info) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return WavProbe::bad;
    const uint64_t size = (uint64_t)f.tellg();
    if (size < 12) return WavProbe::incomplete;
    f.seekg(0);
    unsigned char h[12];
    f.read(reinterpret_cast<char *>(h), 12);
    if (memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) return WavProbe::bad;
    uint64_t p = 12;
    bool have_fmt = false;
    while (p + 8 <= size) {
        unsigned char ch[8];
        f.seekg((std::streamoff)p);
        if (!f.read(reinterpret_cast<char *>(ch), 8)) return WavProbe::incomplete;
        const uint32_t sz = le32(ch + 4);
        if (!memcmp(ch, "fmt ", 4)) {
            if (sz < 16 || p + 8 + 16 > size) return WavProbe::incomplete;
            unsigned char fm[16];
            if (!f.read(reinterpret_cast<char *>(fm), 16)) return WavProbe::incomplete;
            info.channels = le16(fm + 2);
            info.rate = (int)le32(fm + 4);
            info.bits = le16(fm + 14);
            have_fmt = true;
        } else if (!memcmp(ch, "data", 4)) {
            if (!have_fmt) return WavProbe::bad;
            if (info.bits != 16 || info.rate != (int)kAudioRate || info.channels < 1) return WavProbe::bad;
            const uint64_t on_disk = size - (p + 8);
            // A recorder writes the real size only when it stops: 0 or 0xFFFFFFFF until then.
            uint64_t bytes = (sz == 0 || sz == 0xFFFFFFFFu || sz > on_disk) ? on_disk : sz;
            info.data_offset = p + 8;
            info.frames = bytes / (2ull * (uint64_t)info.channels);
            return WavProbe::ok;
        }
        p += 8 + (uint64_t)sz + (sz & 1);
    }
    return WavProbe::incomplete;
}

bool wav_read(const std::string &path, const WavInfo &info, uint64_t start, uint64_t count, std::vector<float> &out) {
    out.clear();
    if (start >= info.frames) return true;
    count = std::min(count, info.frames - start);
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    const uint64_t frame = 2ull * (uint64_t)info.channels;
    f.seekg((std::streamoff)(info.data_offset + start * frame));
    std::vector<int16_t> raw((size_t)(count * info.channels));
    f.read(reinterpret_cast<char *>(raw.data()), (std::streamsize)(raw.size() * 2));
    const size_t got = (size_t)f.gcount() / (size_t)frame;   // whole frames only
    out.reserve(got);
    for (size_t i = 0; i < got; ++i) {
        float v = 0;
        for (int c = 0; c < info.channels; ++c) v += raw[i * info.channels + c];
        out.push_back(v / (info.channels * 32768.0f));
    }
    return true;
}

bool read_wav16k(const std::string &path, std::vector<float> &pcm) {
    WavInfo info;
    if (wav_probe(path, info) != WavProbe::ok) return false;
    return wav_read(path, info, 0, info.frames, pcm);
}

PiecePlan plan_next_piece(const std::vector<float> &pcm, size_t offset, size_t start, size_t total, bool is_final) {
    PiecePlan r;
    const size_t avail = total > start ? total - start : 0;
    if (!is_final && avail < kPieceTarget + kPieceSearch) { r.kind = PiecePlan::wait; return r; }
    if (is_final && avail <= kPieceTarget) {
        if (avail < kPieceMin) { r.kind = PiecePlan::end; return r; }
        r.kind = PiecePlan::piece; r.start = start; r.stop = total;
        return r;
    }
    // Cut at the quietest 0.1 s in the 3 s before 28 s, so words are not split.
    const size_t end = start + kPieceTarget;
    size_t best = end;
    float best_e = 1e30f;
    for (size_t c = end - kPieceSearch; c + kQuietWindow <= end; c += kQuietWindow / 2) {
        float e = 0;
        for (size_t k = c; k < c + kQuietWindow; ++k) {
            const size_t i = k - offset;
            const float v = i < pcm.size() ? pcm[i] : 0.0f;
            e += v * v;
        }
        if (e < best_e) { best_e = e; best = c + kQuietWindow / 2; }
    }
    r.kind = PiecePlan::piece; r.start = start; r.stop = best;
    return r;
}

std::string transcription_instruction(const std::string &marker, const std::string &vocab, const std::string &prev) {
    std::string instr = marker +
        "\nTranscribe this audio exactly as spoken. Write each word in the script of the language it was spoken in: "
        "Telugu words in Telugu script, Hindi in Devanagari, English words in English. Do not translate. "
        "Output only the transcript.";
    if (!vocab.empty()) instr += "\n" + vocab + ".";
    if (!prev.empty()) {
        // The tail of the previous piece keeps names and spellings consistent across the 28 s cuts.
        std::string tail = prev.size() > 400 ? prev.substr(prev.size() - 400) : prev;
        while (!tail.empty() && (static_cast<unsigned char>(tail.front()) & 0xC0) == 0x80) tail.erase(0, 1);
        instr += "\nFor context only, the previous part ended with: \"" + tail + "\"";
    }
    return instr;
}

} // namespace sve
