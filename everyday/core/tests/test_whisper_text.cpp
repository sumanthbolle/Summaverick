#include "check.h"
#include "sve_whisper_text.h"
#include <cmath>

using namespace sve;

static const int64_t R = 16000;

TEST(whisper_zeros_group) {
    std::vector<float> pcm(60 * R, 0.0f);
    Group g = plan_group(pcm.data(), pcm.size(), 0, true);
    CHECK(g.status == Group::Piece);
    CHECK(!g.spans.empty());
    int64_t pos = 0;
    for (auto &s : g.spans) {
        CHECK_EQ(s.start, pos);
        CHECK(s.end - s.start >= 4 * R && s.end - s.start <= 10 * R);
        pos = s.end;
    }
    CHECK_EQ(g.end_sample, pos);
    CHECK(g.end_sample <= 28 * R);
}

TEST(whisper_quietest_point) {
    std::vector<float> pcm(30 * R);
    for (size_t i = 0; i < pcm.size(); i++) pcm[i] = 0.5f * std::sin(2.0 * M_PI * 200.0 * i / R);
    for (int64_t i = 6 * R; i < 6 * R + R * 4 / 10; i++) pcm[i] = 0.0f;
    Group g = plan_group(pcm.data(), pcm.size(), 0, true);
    CHECK(g.status == Group::Piece);
    CHECK(g.spans[0].end >= 6 * R - 320 && g.spans[0].end <= 6 * R + R * 4 / 10 + 320);
}

TEST(whisper_wait_and_end) {
    std::vector<float> pcm(20 * R, 0.0f);
    CHECK(plan_group(pcm.data(), pcm.size(), 0, false).status == Group::Wait);
    std::vector<float> p2(10 * R + 4800, 0.0f);
    CHECK(plan_group(p2.data(), p2.size(), 10 * R, true).status == Group::End);
}

TEST(whisper_short_recording) {
    std::vector<float> pcm(R * 5 / 2, 0.0f);
    Group g = plan_group(pcm.data(), pcm.size(), 0, true);
    CHECK(g.status == Group::Piece);
    CHECK_EQ(g.spans.size(), (size_t)1);
    CHECK_EQ(g.spans[0].start, 0);
    CHECK_EQ(g.spans[0].end, 40000);
}

TEST(whisper_short_tail) {
    std::vector<float> pcm(31 * R, 0.0f);
    Group a = plan_group(pcm.data(), pcm.size(), 0, true);
    Group b = plan_group(pcm.data(), pcm.size(), a.end_sample, true);
    CHECK(b.status == Group::Piece);
    CHECK_EQ(b.spans.size(), (size_t)1);
    CHECK(b.spans[0].end - b.spans[0].start >= 2 * R && b.spans[0].end - b.spans[0].start <= 4 * R);
    for (auto &s : a.spans) CHECK(s.end > s.start);
    CHECK_EQ(b.end_sample, 31 * R);
}

TEST(whisper_collapse_repeats) {
    CHECK_EQ(collapse_repeats("టిక్ టిక్ టిక్ టిక్ టిక్ ఇది"), std::string("టిక్ ఇది"));
    CHECK_EQ(collapse_repeats("a b a b a b a b c"), std::string("a b c"));
    CHECK_EQ(collapse_repeats("one one one two"), std::string("one one one two"));
    CHECK_EQ(collapse_repeats("టిక్ టిక్ టిక్ టిక్ టిక్"), std::string(""));
    CHECK_EQ(collapse_repeats("ఇది టిక్ టిక్ టిక్ టిక్ టిక్"), std::string("ఇది టిక్"));
    CHECK_EQ(collapse_repeats("a a a a b b b b"), std::string("a b"));
}

TEST(whisper_lang_choice) {
    CHECK(lang_choice(0.9f, 0.05f) == LangChoice::Telugu);
    CHECK(lang_choice(0.55f, 0.4f) == LangChoice::Gemma);
    CHECK(lang_choice(0.7f, 0.45f) == LangChoice::Gemma);
}

TEST(whisper_non_final_piece_and_wait_boundary) {
    std::vector<float> pcm(40 * R, 0.0f);
    Group g = plan_group(pcm.data(), pcm.size(), 0, false);
    CHECK(g.status == Group::Piece);
    CHECK(g.end_sample <= 28 * R);
    std::vector<float> a(10 * R + 31 * R, 0.0f), b(10 * R + 31 * R - 1, 0.0f);
    CHECK(plan_group(a.data(), a.size(), 10 * R, false).status == Group::Piece);
    CHECK(plan_group(b.data(), b.size(), 10 * R, false).status == Group::Wait);
}

TEST(whisper_group_from_mid_recording_matches_truncated_buffer) {
    std::vector<float> pcm(120 * R);
    unsigned x = 12345;
    for (size_t i = 0; i < pcm.size(); i++) {
        x = x * 1664525u + 1013904223u;
        float amp = (i / (3 * R)) % 2 ? 0.05f : 0.5f;   // loud and quiet stretches
        pcm[i] = amp * ((x >> 8) / 8388608.0f - 1.0f);
    }
    int64_t start = 47 * R + 123;
    Group full = plan_group(pcm.data(), pcm.size(), start, true);
    Group cut = plan_group(pcm.data(), start + 29 * R, start, true);
    CHECK(full.status == Group::Piece && cut.status == Group::Piece);
    CHECK_EQ(full.spans.size(), cut.spans.size());
    for (size_t i = 0; i < full.spans.size() && i < cut.spans.size(); i++) {
        CHECK_EQ(full.spans[i].start, cut.spans[i].start);
        CHECK_EQ(full.spans[i].end, cut.spans[i].end);
    }
}
