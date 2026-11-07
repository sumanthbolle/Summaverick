#include "check.h"
#include "sve_sections.h"
#include "sve_store.h"

using namespace sve;
using nlohmann::json;

static std::vector<PieceRef> uniform(int n, int len) {
    std::vector<PieceRef> v;
    for (int i = 0; i < n; ++i) v.push_back({(int64_t)i * len, (int64_t)(i + 1) * len});
    return v;
}

TEST(plan_sections_cuts_near_300s) {
    auto p = uniform(30, 28);
    auto r = plan_sections(p, true);
    CHECK(r.size() > 1);
    size_t next = 0;
    for (size_t i = 0; i < r.size(); ++i) {
        CHECK_EQ(r[i].first, next);
        CHECK(r[i].second > r[i].first);
        next = r[i].second;
        int64_t d = p[r[i].second - 1].end_sec - p[r[i].first].start_sec;
        if (i + 1 < r.size()) CHECK(d >= 240 && d <= 360);
    }
    CHECK_EQ(next, (size_t)30);
}

TEST(plan_sections_holds_short_tail_when_not_final) {
    auto p = uniform(10, 28);
    CHECK_EQ(plan_sections(p, false).size(), (size_t)0);
    auto r = plan_sections(p, true);
    CHECK_EQ(r.size(), (size_t)1);
    CHECK(r[0].first == 0 && r[0].second == 10);
}

TEST(plan_sections_not_final_keeps_full_sections_and_holds_tail) {
    auto r = plan_sections(uniform(14, 28), false);  // 11 pieces = 308 s, then 3 pieces = 84 s
    CHECK_EQ(r.size(), (size_t)1);
    CHECK(r[0].second == 11);
}

TEST(plan_sections_single_long_piece) {
    auto r = plan_sections({{0, 400}}, true);
    CHECK_EQ(r.size(), (size_t)1);
    CHECK(r[0].first == 0 && r[0].second == 1);
    CHECK(plan_sections({}, true).empty());
}

TEST(plan_sections_avoids_passing_max_len) {
    // 250 s then a 200 s piece: closing at 250 (>= min_len) beats 450 (> max_len).
    auto r = plan_sections({{0, 125}, {125, 250}, {250, 450}}, true);
    CHECK_EQ(r.size(), (size_t)2);
    CHECK(r[0].second == 2 && r[1].first == 2);
}

static json sec(int idx, int first, int last, int s, int e, const char *title) {
    return {{"idx", idx}, {"first_piece", first}, {"last_piece", last}, {"start_sec", s}, {"end_sec", e},
            {"summary", {{"title", title}, {"points", json::array()}}}};
}

TEST(sections_roundtrip_and_cascade) {
    Store st(":memory:");
    int64_t id = st.create("t", "en", nullptr, "x", 600);
    CHECK(st.sections(id).is_array() && st.sections(id).empty());
    json in = json::array({sec(0, 0, 10, 0, 308, "A"), sec(1, 11, 20, 308, 600, "B")});
    CHECK(st.set_sections(id, in));
    CHECK(st.sections(id) == in);
    // replaces all
    json one = json::array({sec(0, 0, 20, 0, 600, "C")});
    CHECK(st.set_sections(id, one));
    CHECK(st.sections(id) == one);
    // clearing the transcript clears sections
    CHECK(st.clear_transcript(id));
    CHECK(st.sections(id).empty());
    // deleting the note clears sections
    CHECK(st.set_sections(id, in));
    CHECK(st.remove(id));
    CHECK(st.sections(id).empty());
    // no resurrecting a deleted note: writing sections for a missing note fails
    CHECK(!st.set_sections(id, in));
    CHECK(st.sections(id).empty());
}
