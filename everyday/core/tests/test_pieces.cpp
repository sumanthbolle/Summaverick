#include "check.h"
#include "sve_core.h"
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

static const int64_t R = 16000;

static std::string write_wav(const std::vector<int16_t> &s, const char *tag) {
    std::string path = std::string("/tmp/sve_test_pieces_") + tag + ".wav";
    FILE *f = std::fopen(path.c_str(), "wb");
    uint32_t data = (uint32_t)(s.size() * 2), riff = 36 + data, fmt = 16, rate = 16000, brate = 32000;
    uint16_t pcm = 1, ch = 1, align = 2, bits = 16;
    std::fwrite("RIFF", 1, 4, f); std::fwrite(&riff, 4, 1, f); std::fwrite("WAVEfmt ", 1, 8, f);
    std::fwrite(&fmt, 4, 1, f); std::fwrite(&pcm, 2, 1, f); std::fwrite(&ch, 2, 1, f);
    std::fwrite(&rate, 4, 1, f); std::fwrite(&brate, 4, 1, f); std::fwrite(&align, 2, 1, f);
    std::fwrite(&bits, 2, 1, f); std::fwrite("data", 1, 4, f); std::fwrite(&data, 4, 1, f);
    std::fwrite(s.data(), 2, s.size(), f);
    std::fclose(f);
    return path;
}

static std::vector<int16_t> tone(int64_t secs, bool gaps) {
    std::vector<int16_t> s(secs * R);
    for (int64_t i = 0; i < (int64_t)s.size(); i++) {
        bool quiet = gaps && (i / R) % 8 == 7;
        s[i] = quiet ? 0 : (int16_t)(10000 * std::sin(2.0 * M_PI * 440.0 * i / R));
    }
    return s;
}

// Walks the file piece by piece; returns the number of pieces, -1 on a gap, a bad status or a runaway loop.
static int walk(const std::string &path, int64_t total) {
    int64_t pos = 0; int n = 0;
    for (int guard = 0; guard < 100; guard++) {
        int64_t end = -1;
        int st = sve_plan_next_piece(path.c_str(), pos, 1, &end);
        if (st == 2) return (total - pos <= R / 2 && n > 0) || total == 0 ? n : -1;
        if (st != 0 || end <= pos) return -1;
        pos = end; n++;
    }
    return -1;
}

TEST(pieces_tone_with_silences) {
    std::string p = write_wav(tone(60, true), "tone");
    int64_t end = -1;
    CHECK_EQ(sve_plan_next_piece(p.c_str(), 0, 1, &end), 0);
    CHECK(end > 0 && end <= 28 * R);
    int n = walk(p, 60 * R);
    CHECK(n >= 2);
    std::remove(p.c_str());
}

TEST(pieces_not_final_waits) {
    std::string p = write_wav(tone(20, false), "wait");
    int64_t end = -1;
    CHECK_EQ(sve_plan_next_piece(p.c_str(), 0, 0, &end), 1);
    std::remove(p.c_str());
}

TEST(pieces_short_recording) {
    std::string p = write_wav(tone(3, false), "short");
    int64_t end = -1;
    CHECK_EQ(sve_plan_next_piece(p.c_str(), 0, 1, &end), 0);
    CHECK_EQ(end, 3 * R);
    CHECK_EQ(sve_plan_next_piece(p.c_str(), end, 1, &end), 2);
    std::remove(p.c_str());
}

TEST(pieces_all_zero_ends) {
    std::string p = write_wav(std::vector<int16_t>(30 * R, 0), "zero");
    CHECK(walk(p, 30 * R) >= 1);
    std::remove(p.c_str());
}

TEST(pieces_unreadable) {
    int64_t end = -1;
    CHECK_EQ(sve_plan_next_piece("/nonexistent.wav", 0, 1, &end), -1);
}

TEST(pieces_read_samples) {
    std::string p = write_wav(tone(3, false), "read");
    float *out = nullptr;
    CHECK_EQ(sve_read_samples(p.c_str(), R, 2 * R, &out), R);
    CHECK(out != nullptr);
    if (out) { for (int64_t i = 0; i < R; i++) CHECK(out[i] >= -1.0f && out[i] < 1.0f); sve_free_samples(out); }
    std::remove(p.c_str());
    FILE *f = std::fopen("/tmp/sve_test_pieces.txt", "w");
    std::fputs("this is plain text, not a wav file at all, padding padding padding padding\n", f);
    std::fclose(f);
    out = nullptr;
    CHECK_EQ(sve_read_samples("/tmp/sve_test_pieces.txt", 0, 100, &out), -1);
    std::remove("/tmp/sve_test_pieces.txt");
}
