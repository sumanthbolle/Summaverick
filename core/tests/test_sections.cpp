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

TEST(plan_sections_json_api) {
    // 14 pieces of 28 s: the first 11 (308 s) close a section; the last 3 wait while not final.
    json pieces = json::array();
    for (int i = 0; i < 14; ++i) pieces.push_back({{"start_sec", i * 28}, {"text", "p"}});
    CHECK_EQ(plan_sections_json(pieces.dump(), 14 * 28, false), std::string("[[0,11]]"));
    CHECK_EQ(plan_sections_json(pieces.dump(), 14 * 28, true), std::string("[[0,11],[11,14]]"));
    // The last piece's end comes from the caller: a short tail waits, a long one closes.
    json ten = json::array();
    for (int i = 0; i < 10; ++i) ten.push_back({{"start_sec", i * 28}, {"text", "p"}});
    CHECK_EQ(plan_sections_json(ten.dump(), 280, false), std::string("[]"));
    CHECK_EQ(plan_sections_json(ten.dump(), 300, false), std::string("[[0,10]]"));
    // Not an array, empty, missing input: no ranges.
    CHECK_EQ(plan_sections_json("{}", 0, true), std::string("[]"));
    CHECK_EQ(plan_sections_json("[]", 0, true), std::string("[]"));
    CHECK_EQ(plan_sections_json("not json", 0, true), std::string("[]"));
    // Not sane: a start going backwards, negative, a string, an end before the last start. Still covers all pieces in order.
    json bad = json::parse(R"([{"start_sec":0},{"start_sec":200},{"start_sec":150},{"start_sec":-5},{"start_sec":"x"},{"start_sec":320.7}])");
    json r = json::parse(plan_sections_json(bad.dump(), 10, true));
    CHECK(r.is_array() && !r.empty());
    size_t next = 0;
    for (const auto &g : r) {
        CHECK_EQ(g[0].get<size_t>(), next);
        CHECK(g[1].get<size_t>() > g[0].get<size_t>());
        next = g[1].get<size_t>();
    }
    CHECK_EQ(next, (size_t)6);
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

using Pieces = std::vector<std::pair<int64_t, std::string>>;
static const std::vector<int64_t> kStarts = {0, 28, 56, 84, 112};

TEST(parse_section_summary_accepts_fenced_json) {
    std::string out = "<think>hmm</think>Here:\n```json\n{\"title\":\"Budget\",\"topic\":\"money\",\"points\":"
                      "[{\"text\":\"Spend less\",\"start_sec\":28},{\"text\":\"Save\",\"start_sec\":56}],"
                      "\"terms\":[\"GST\"],\"hints\":[\"x\"]}\n```";
    json s;
    CHECK(parse_section_summary(out, kStarts, s));
    CHECK_EQ(s["title"].get<std::string>(), std::string("Budget"));
    CHECK_EQ(s["points"].size(), (size_t)2);
    CHECK_EQ(s["points"][1]["start_sec"].get<int64_t>(), (int64_t)56);
    CHECK_EQ(s["terms"][0].get<std::string>(), std::string("GST"));
}

TEST(parse_section_summary_clamps_start_secs) {
    json s;
    CHECK(parse_section_summary("{\"title\":\"T\",\"points\":[{\"text\":\"a\",\"start_sec\":95},"
                                "{\"text\":\"b\",\"start_sec\":9999},{\"text\":\"c\"}]}", kStarts, s));
    CHECK_EQ(s["points"][0]["start_sec"].get<int64_t>(), (int64_t)84);
    CHECK_EQ(s["points"][1]["start_sec"].get<int64_t>(), (int64_t)112);
    CHECK_EQ(s["points"][2]["start_sec"].get<int64_t>(), (int64_t)0);
    CHECK(s["terms"].is_array() && s["hints"].is_array());
}

TEST(parse_section_summary_rejects_truncated) {
    json s;
    CHECK(!parse_section_summary("{\"title\":\"T\",\"points\":[{\"text\":\"abc", kStarts, s));
    CHECK(!parse_section_summary("{\"title\":\"\",\"points\":[{\"text\":\"a\",\"start_sec\":0}]}", kStarts, s));
    CHECK(!parse_section_summary("{\"title\":\"T\",\"points\":[]}", kStarts, s));
    CHECK(!parse_section_summary("", kStarts, s));
}

TEST(parse_section_summary_caps_points_at_6) {
    std::string o = "{\"title\":\"T\",\"points\":[";
    for (int i = 0; i < 9; ++i) o += std::string(i ? "," : "") + "{\"text\":\"p" + std::to_string(i) + "\",\"start_sec\":0}";
    o += "]}";
    json s;
    CHECK(parse_section_summary(o, kStarts, s));
    CHECK_EQ(s["points"].size(), (size_t)6);
}

TEST(fallback_summary_shape) {
    Pieces p = {{0, "One two three four five six seven eight nine ten. Second sentence."}, {28, "Alpha beta. More."},
                {56, "Gamma."}, {84, "Delta."}};
    json s = fallback_section_summary(p);
    CHECK_EQ(s["title"].get<std::string>(), std::string("One two three four five six seven eight"));
    CHECK_EQ(s["points"].size(), (size_t)3);
    CHECK_EQ(s["points"][0]["text"].get<std::string>(), std::string("One two three four five six seven eight nine ten."));
    CHECK_EQ(s["points"][1]["start_sec"].get<int64_t>(), (int64_t)28);
    CHECK(s["fallback"].get<bool>());
    CHECK(s["terms"].is_array() && s["hints"].is_array());
}

TEST(section_prompt_mentions_language_and_starts) {
    Pieces p = {{0, "hello"}, {28, "world"}, {56, "x"}};
    std::string te = section_prompt(p, "te", "Words: Metformin");
    CHECK(te.find("Telugu") != std::string::npos);
    CHECK(te.find("[t=0]") != std::string::npos && te.find("[t=28]") != std::string::npos && te.find("[t=56]") != std::string::npos);
    CHECK(te.find("Metformin") != std::string::npos);
    std::string en = section_prompt(p, "en", "");
    CHECK(en.find("English") != std::string::npos);
    CHECK(en.find("Telugu") == std::string::npos);
}

TEST(parse_section_summary_survives_huge_start_sec) {
    json s;
    CHECK(parse_section_summary("{\"title\":\"T\",\"points\":[{\"text\":\"a\",\"start_sec\":1e300},"
                                "{\"text\":\"b\",\"start_sec\":-1e300}]}", kStarts, s));
    CHECK_EQ(s["points"][0]["start_sec"].get<int64_t>(), (int64_t)0);
    CHECK_EQ(s["points"][1]["start_sec"].get<int64_t>(), (int64_t)0);
}

static json sec(int64_t start, const std::string &title, const std::string &topic) {
    return {{"start_sec", start}, {"summary", {{"title", title}, {"topic", topic}, {"points", json::array({{{"text", title + " point"}, {"start_sec", start}}})}}}};
}

TEST(merge_chapters_joins_same_topic) {
    json s = json::array({sec(0, "First A", "A"), sec(300, "Second", "a "), sec(600, "B1", "B"), sec(900, "B2", "B"), sec(1200, "Back", "A")});
    json c = merge_chapters(s);
    CHECK_EQ(c.size(), (size_t)3);
    CHECK_EQ(c[0]["title"].get<std::string>(), std::string("First A"));
    CHECK_EQ(c[0]["start_sec"].get<int64_t>(), (int64_t)0);
    CHECK_EQ(c[1]["title"].get<std::string>(), std::string("B1"));
    CHECK_EQ(c[1]["start_sec"].get<int64_t>(), (int64_t)600);
    CHECK_EQ(c[2]["start_sec"].get<int64_t>(), (int64_t)1200);
}

TEST(merge_chapters_telugu_topics) {
    json s = json::array({sec(0, "ఆరోగ్యం 1", "ఆరోగ్య సలహా"), sec(300, "ఆరోగ్యం 2", "  ఆరోగ్య సలహా\n"), sec(600, "ఆహారం", "ఆహారం")});
    json c = merge_chapters(s);
    CHECK_EQ(c.size(), (size_t)2);
    CHECK_EQ(c[0]["title"].get<std::string>(), std::string("ఆరోగ్యం 1"));
    CHECK_EQ(c[1]["start_sec"].get<int64_t>(), (int64_t)300 * 2);
    CHECK_EQ(merge_chapters(json::array()).size(), (size_t)0);
}

TEST(section_groups_respects_budget) {
    json s = json::array();
    for (int i = 0; i < 36; ++i) s.push_back(sec(i * 300, "Section number " + std::to_string(i), "t" + std::to_string(i)));
    auto count = [](const std::string &t) { return (int)(t.size() / 4); };
    const int budget = 120;
    auto g = section_groups(s, budget, count);
    CHECK(g.size() > 1);
    std::string all;
    for (const auto &x : g) { CHECK(count(x) <= budget); all += x; }
    size_t last = 0;
    for (int i = 0; i < 36; ++i) {
        size_t p = all.find("Section number " + std::to_string(i) + " ", last);
        CHECK(p != std::string::npos);
        if (p != std::string::npos) last = p;
    }
    CHECK_EQ(section_groups(json::array(), budget, count).size(), (size_t)0);
}
