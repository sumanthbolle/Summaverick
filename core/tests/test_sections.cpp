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
    CHECK(s.value("memories_missing", false));  // its memories are made by the memory-only prompt
    CHECK(s["terms"].is_array() && s["hints"].is_array());
}

TEST(section_prompt_mentions_language_and_starts) {
    Pieces p = {{0, "hello"}, {28, "world"}, {56, "x"}};
    std::string te = section_prompt(p, "te", "Words: Metformin", false);
    CHECK(te.find("Telugu") != std::string::npos);
    CHECK(te.find("[t=0]") != std::string::npos && te.find("[t=28]") != std::string::npos && te.find("[t=56]") != std::string::npos);
    CHECK(te.find("Metformin") != std::string::npos);
    std::string en = section_prompt(p, "en", "", false);
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

// --- memories from the section call ----------------------------------------------------------

// Today's section_prompt output for these pieces (copied from the build before memories were added).
static const Pieces kGoldPieces = {{0, "రవికి చెప్పాను"}, {42, "Meet Ravi on Friday at 5."}};
static const char *kGoldTe =
    "Summarize this part of a recording. Write the summary in Telugu. Keep names, places and technical terms in the "
    "script they were spoken in. Reply with one JSON object and nothing else, in this shape:\n"
    "{\"title\":\"short title, at most 8 words\",\"topic\":\"one line on what this part is about\",\"points\":"
    "[{\"text\":\"one key point\",\"start_sec\":0}],\"terms\":[\"important names or technical terms\"],\"hints\":"
    "[\"things worth following up, if any\"]}\n"
    "Rules:\n"
    "- 3 to 6 points, each one sentence.\n"
    "- Each point's \"start_sec\" is copied from the [t=SEC] marker of the transcript line it came from.\n"
    "- Use only what was said. Use an empty array when nothing applies.\n"
    "Words: Metformin, రవి\n"
    "\n"
    "Transcript (each line starts with its [t=SEC] marker):\n"
    "[t=0] రవికి చెప్పాను\n"
    "[t=42] Meet Ravi on Friday at 5.\n"
    "\n"
    "JSON:\n";
static const char *kGoldEn =
    "Summarize this part of a recording. Write the summary in English. Keep names, places and technical terms in the "
    "script they were spoken in. Reply with one JSON object and nothing else, in this shape:\n"
    "{\"title\":\"short title, at most 8 words\",\"topic\":\"one line on what this part is about\",\"points\":"
    "[{\"text\":\"one key point\",\"start_sec\":0}],\"terms\":[\"important names or technical terms\"],\"hints\":"
    "[\"things worth following up, if any\"]}\n"
    "Rules:\n"
    "- 3 to 6 points, each one sentence.\n"
    "- Each point's \"start_sec\" is copied from the [t=SEC] marker of the transcript line it came from.\n"
    "- Use only what was said. Use an empty array when nothing applies.\n"
    "\n"
    "Transcript (each line starts with its [t=SEC] marker):\n"
    "[t=0] రవికి చెప్పాను\n"
    "[t=42] Meet Ravi on Friday at 5.\n"
    "\n"
    "JSON:\n";

static std::string erase_once(std::string s, const std::string &part) {
    size_t at = s.find(part);
    if (part.empty() || at == std::string::npos) return "<not found>";
    s.erase(at, part.size());
    return s;
}

TEST(section_prompt_summary_part_unchanged) {
    CHECK_EQ(section_prompt(kGoldPieces, "te", "Words: Metformin, రవి", false), std::string(kGoldTe));
    CHECK_EQ(section_prompt(kGoldPieces, "en", "", false), std::string(kGoldEn));
    for (const auto &lv : {std::make_pair(std::string("te"), std::string("Words: Metformin, రవి")),
                           std::make_pair(std::string("en"), std::string(""))}) {
        const std::string plain = section_prompt(kGoldPieces, lv.first, lv.second, false);
        const std::string both = section_prompt(kGoldPieces, lv.first, lv.second, true);
        CHECK(both != plain);
        CHECK_EQ(erase_once(erase_once(both, kMemoryShape), memory_rules(lv.first)), plain);
        // The shape goes before the shape line's closing brace, the rules after "Use only what was said".
        CHECK(both.find(std::string("[\"things worth following up, if any\"]") + kMemoryShape + "}\n") != std::string::npos);
        CHECK(both.find("- Use only what was said. Use an empty array when nothing applies.\n" + memory_rules(lv.first)) !=
              std::string::npos);
    }
    CHECK(memory_rules("te").find("Telugu") != std::string::npos);
    CHECK(memory_rules("en").find("English") != std::string::npos);
    CHECK(std::string(kMemoryShape).find("\"as_said\"") != std::string::npos);
    CHECK(std::string(kMemoryShape).rfind(",\"memories\":[", 0) == 0);
}

TEST(memory_prompt_shape) {
    const std::string m = memory_prompt(kGoldPieces, "te", "Words: Metformin, రవి");
    CHECK(m.find(std::string("{") + (kMemoryShape + 1) + "}\n") != std::string::npos);  // {"memories":[...]}
    CHECK(m.find("Telugu") != std::string::npos);
    CHECK(m.find("[t=42] Meet Ravi on Friday at 5.\n") != std::string::npos);
    CHECK(m.find("Words: Metformin, రవి\n") != std::string::npos);
    CHECK(m.find("\"points\"") == std::string::npos);  // memories only
    CHECK(m.size() > 6 && m.compare(m.size() - 6, 6, "JSON:\n") == 0);
}

static const char *kSummaryBody =
    "{\"title\":\"Budget\",\"topic\":\"money\",\"points\":[{\"text\":\"Spend less\",\"start_sec\":28,\"type\":\"decision\"},"
    "{\"text\":\"Save\",\"start_sec\":56,\"type\":\"nonsense\"}],\"terms\":[\"GST\"],\"hints\":[\"x\"]";
static const char *kMemoriesPart =
    ",\"memories\":[{\"type\":\"promise\",\"text\":\"Ravi will send the report.\",\"who\":\"Ravi\",\"to_whom\":\"\","
    "\"due_text\":\"\",\"quote\":\"Ravi will send the report\",\"start_sec\":28,\"names\":[{\"as_said\":\"Ravi\","
    "\"as_written\":\"Ravi\"}],\"numbers\":[],\"dates\":[],\"times\":[]}]";

static void check_same_summary(const json &a, const json &b) {
    for (const char *k : {"title", "topic", "terms", "hints"}) CHECK_EQ(a[k], b[k]);
    CHECK_EQ(a["points"].size(), b["points"].size());
    for (size_t i = 0; i < a["points"].size() && i < b["points"].size(); ++i) {
        CHECK_EQ(a["points"][i]["text"], b["points"][i]["text"]);
        CHECK_EQ(a["points"][i]["start_sec"], b["points"][i]["start_sec"]);
    }
}

TEST(parse_section_summary_ignores_memories) {
    json with, without;
    CHECK(parse_section_summary(std::string(kSummaryBody) + kMemoriesPart + "}", kStarts, with));
    CHECK(parse_section_summary(std::string(kSummaryBody) + "}", kStarts, without));
    check_same_summary(with, without);
    CHECK_EQ(with["points"][0]["type"].get<std::string>(), std::string("decision"));  // a valid point type is kept
    CHECK(!with["points"][1].contains("type"));                                       // an unknown one is not
    CHECK(with["memories"].is_array() && with["memories"].size() == 1);
    CHECK_EQ(with["memories"][0]["type"].get<std::string>(), std::string("promise"));
    CHECK(!with.contains("memories_missing"));
    CHECK(!without.contains("memories"));
    CHECK(without.value("memories_missing", false));
}

TEST(section_groups_ignore_types_and_memories) {
    json plain = json::array(), rich = json::array();
    for (int i = 0; i < 4; ++i) {
        json s = sec(i * 300, "Section " + std::to_string(i), i < 2 ? "A" : "B");
        plain.push_back(s);
        s["summary"]["points"][0]["type"] = "promise";
        s["summary"]["memories"] = json::parse("{" + std::string(kMemoriesPart + 1) + "}")["memories"];
        s["summary"]["memories_missing"] = i == 3;
        rich.push_back(s);
    }
    CHECK(rich[0]["summary"]["memories"].is_array() && !rich[0]["summary"]["memories"].empty());
    auto count = [](const std::string &t) { return (int)(t.size() / 4); };
    CHECK(section_groups(plain, 60, count) == section_groups(rich, 60, count));
    CHECK(section_groups(plain, 4000, count) == section_groups(rich, 4000, count));
    CHECK_EQ(merge_chapters(plain), merge_chapters(rich));
    for (const auto &g : section_groups(rich, 4000, count)) {
        CHECK(g.find("promise") == std::string::npos);
        CHECK(g.find("report") == std::string::npos);
    }
}

TEST(salvage_summary_from_truncated_memories) {
    const std::string cut = std::string(kSummaryBody) + ",\"memories\":[{\"type\":\"fact\",\"text\":\"cut";
    json s;
    CHECK(!parse_section_summary(cut, kStarts, s));
    CHECK(salvage_summary(cut, kStarts, s));
    CHECK_EQ(s["title"].get<std::string>(), std::string("Budget"));
    CHECK_EQ(s["points"].size(), (size_t)2);
    CHECK_EQ(s["points"][1]["start_sec"].get<int64_t>(), (int64_t)56);
    CHECK(s.value("memories_missing", false));
    CHECK(!s.contains("memories"));
    // Fenced, with thinking first, and a "memories" key inside a string value that is not the cut.
    const std::string tricky = "<think>x</think>```json\n{\"title\":\"A ,\\\"memories\\\" b\",\"points\":[{\"text\":\"p\","
                               "\"start_sec\":0}] , \"memories\" : [{\"type\":\"idea\",\"te";
    json t;
    CHECK(salvage_summary(tricky, kStarts, t));
    CHECK_EQ(t["title"].get<std::string>(), std::string("A ,\"memories\" b"));
    CHECK(t.value("memories_missing", false));
    // No summary before the memories, or no memories key at all: nothing to salvage.
    json u;
    CHECK(!salvage_summary("{\"memories\":[{\"type\":\"fact\"", kStarts, u));
    CHECK(!salvage_summary("{\"title\":\"T\",\"points\":[{\"text\":\"abc", kStarts, u));
    CHECK(!salvage_summary("", kStarts, u));
}

TEST(parse_memories_bad_array_marks_missing) {
    json s;
    CHECK(parse_section_summary(std::string(kSummaryBody) + ",\"memories\":\"none\"}", kStarts, s));
    CHECK(s.value("memories_missing", false));
    CHECK(!s.contains("memories"));
    json e;  // an empty array is valid: the section has nothing worth remembering
    CHECK(parse_section_summary(std::string(kSummaryBody) + ",\"memories\":[]}", kStarts, e));
    CHECK(e["memories"].is_array() && e["memories"].empty());
    CHECK(!e.contains("memories_missing"));
}

TEST(complete_memory_items_from_a_cut_reply) {
    const std::string two = "{\"type\":\"fact\",\"text\":\"A {b}\",\"quote\":\"q \\\"x\\\"\",\"names\":[{\"as_said\":\"R\"}]},"
                            "{\"type\":\"idea\",\"text\":\"C\",\"quote\":\"d\"}";
    json m = complete_memory_items("```json\n{\"memories\": [" + two + ",{\"type\":\"fact\",\"text\":\"cu");
    CHECK_EQ(m.size(), (size_t)2);
    if (m.size() == 2) {
        CHECK_EQ(m[0]["text"].get<std::string>(), std::string("A {b}"));
        CHECK_EQ(m[1]["type"].get<std::string>(), std::string("idea"));
    }
    CHECK_EQ(complete_memory_items("{\"memories\":[" + two + "]}").size(), (size_t)2);
    CHECK_EQ(complete_memory_items("{\"memories\":[]}").size(), (size_t)0);
    CHECK_EQ(complete_memory_items("{\"memories\":\"none\"}").size(), (size_t)0);
    CHECK_EQ(complete_memory_items("{\"title\":\"T\"}").size(), (size_t)0);
    CHECK_EQ(complete_memory_items("{\"memories\":[{\"type\":\"fa").size(), (size_t)0);
}
