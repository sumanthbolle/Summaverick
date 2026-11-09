#include "check.h"
#include <cmath>
#include "sve_sections.h"
#include "sve_store.h"
#include "sve_context.h"

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
    CHECK_EQ(s["points"].size(), (size_t)6);  // N7 rev 1: back to 6 (10 points turned garbled audio into more points)
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

// --- memories from the section call ----------------------------------------------------------

// Today's section_prompt output for these pieces (N7 rev 1: 3 to 6 points spread over the part, [unclear] lines,
// no invented expansions, no ads).
static const Pieces kGoldPieces = {{0, "రవికి చెప్పాను"}, {42, "Meet Ravi on Friday at 5."}};
static const char *kGoldTe =
    "Summarize this part of a recording. Write the summary in Telugu. Keep names, places and technical terms in the "
    "script they were spoken in. Reply with one JSON object and nothing else, in this shape:\n"
    "{\"title\":\"short title, at most 8 words\",\"topic\":\"one line on what this part is about\",\"points\":"
    "[{\"text\":\"one key point\",\"start_sec\":0}],\"terms\":[\"important names or technical terms\"],\"hints\":"
    "[\"things worth following up, if any\"]}\n"
    "Rules:\n"
    "- 3 to 6 points, each one sentence. Draw points from the beginning, middle and end of this part, not only its "
    "start.\n"
    "- Lines marked [unclear] were hard to hear: do not state anything from them as fact; skip them or say the passage "
    "was unclear.\n"
    "- Each point's \"start_sec\" is copied from the [t=SEC] marker of the transcript line it came from.\n"
    "- Use only what was said. Use an empty array when nothing applies.\n"
    "- Never expand an abbreviation the speaker did not expand. Leave out advertisements and sponsor messages.\n"
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
    "- 3 to 6 points, each one sentence. Draw points from the beginning, middle and end of this part, not only its "
    "start.\n"
    "- Lines marked [unclear] were hard to hear: do not state anything from them as fact; skip them or say the passage "
    "was unclear.\n"
    "- Each point's \"start_sec\" is copied from the [t=SEC] marker of the transcript line it came from.\n"
    "- Use only what was said. Use an empty array when nothing applies.\n"
    "- Never expand an abbreviation the speaker did not expand. Leave out advertisements and sponsor messages.\n"
    "\n"
    "Transcript (each line starts with its [t=SEC] marker):\n"
    "[t=0] రవికి చెప్పాను\n"
    "[t=42] Meet Ravi on Friday at 5.\n"
    "\n"
    "JSON:\n";

TEST(section_prompt_summary_part_unchanged) {
    // The section summary prompt byte for byte; memories have their own prompt.
    CHECK_EQ(section_prompt(kGoldPieces, "te", "Words: Metformin, రవి"), std::string(kGoldTe));
    CHECK_EQ(section_prompt(kGoldPieces, "en", ""), std::string(kGoldEn));
}

TEST(memory_prompt_shape) {
    const std::string m = memory_prompt(kGoldPieces, "te", "Words: Metformin, రవి");
    CHECK(m.find(std::string(kMemoryShape) + "\n") != std::string::npos);
    CHECK(std::string(kMemoryShape).rfind("{\"memories\":[", 0) == 0);
    CHECK(std::string(kMemoryShape).find("\"as_said\"") != std::string::npos);
    CHECK(m.find(memory_rules("te")) != std::string::npos);
    CHECK(memory_rules("te").find("Telugu") != std::string::npos);
    CHECK(memory_rules("en").find("English") != std::string::npos);
    CHECK(memory_rules("en").find("At most 5 \"memories\"") != std::string::npos);
    CHECK(m.find("[t=42] Meet Ravi on Friday at 5.\n") != std::string::npos);
    CHECK(m.find("Words: Metformin, రవి\n") != std::string::npos);
    CHECK(m.find("\"points\"") == std::string::npos);  // memories only
    CHECK(m.size() > 6 && m.compare(m.size() - 6, 6, "JSON:\n") == 0);
}

static const char *kMemoriesPart =
    ",\"memories\":[{\"type\":\"promise\",\"text\":\"Ravi will send the report.\",\"who\":\"Ravi\",\"to_whom\":\"\","
    "\"due_text\":\"\",\"quote\":\"Ravi will send the report\",\"start_sec\":28,\"names\":[{\"as_said\":\"Ravi\","
    "\"as_written\":\"Ravi\"}],\"numbers\":[],\"dates\":[],\"times\":[]}]";

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

// --- N7: vocabulary is spelling help only and filtered in code; unclear pieces marked ------------

TEST(section_prompt_vocab_is_spelling_only) {
    Store st(":memory:");
    st.add_memory("vocab", "IPO", std::nullopt);
    st.add_memory("vocab", "S&P 500 index", std::nullopt);
    const std::string line = spelling_vocab_line(st);
    const std::string p = section_prompt(kGoldPieces, "en", line);
    // The instruction sits right next to the terms, in the same line.
    const std::string expect = std::string("Spell these words this way (") + kVocabOnlyIfSaid + "): S&P 500 index, IPO\n";
    CHECK(p.find(expect) != std::string::npos);
    CHECK(std::string(kVocabOnlyIfSaid).find("only if") != std::string::npos);
    CHECK(std::string(kVocabOnlyIfSaid).find("never add") != std::string::npos);
    CHECK(std::string(kVocabOnlyIfSaid).find("topic") != std::string::npos);
    const std::string m = memory_prompt(kGoldPieces, "en", line);
    CHECK(m.find(expect) != std::string::npos);
}

TEST(section_prompt_points_spread_and_capped) {
    const std::string p = section_prompt(kGoldPieces, "en", "");
    CHECK(p.find("3 to 6 points") != std::string::npos);
    CHECK(p.find("beginning, middle and end") != std::string::npos);
    CHECK(p.find("Cover every part") == std::string::npos);
    CHECK(p.find("up to 10 points") == std::string::npos);
    CHECK(p.find("[unclear]") != std::string::npos);
    CHECK_EQ(kSectionMaxPoints, (size_t)6);
}

TEST(term_said_word_boundaries) {
    CHECK(term_said("IPO", "the IPO was priced"));
    CHECK(term_said("IPO", "ipo, then"));                 // case-insensitive
    CHECK(term_said("ipo", "The IPO."));
    CHECK(!term_said("IPO", "two IPOs this year"));        // a word boundary: IPOs is another word
    CHECK(!term_said("IPOs", "one IPO"));
    CHECK(!term_said("AI", "he SAID so"));
    CHECK(!term_said("AI bubble", "AI bubbles everywhere"));
    CHECK(term_said("AI bubble", "is there an ai bubble?"));
    CHECK(term_said("S&P 500 index", "the S&P 500 index rose"));
    CHECK(!term_said("S&P 500 index", "the S&P 500 rose"));
    CHECK(term_said("IPO", "IPOలో పెట్టుబడి"));           // a Telugu case ending is not a Latin word letter
    CHECK(term_said("UPSC", "[unclear] UPSC exam"));
    CHECK(!term_said("café", "cafés"));                    // Latin letters past ASCII are word letters too
    CHECK(term_said("రవి", "రవికి చెప్పాను"));             // Telugu: a substring match
    CHECK(!term_said("రవి", "Ravi"));
    CHECK(!term_said("", "anything"));
    CHECK(!term_said("  ", "anything"));
    CHECK(term_said(" IPO ", "an IPO"));                   // the saved term is trimmed
    CHECK(term_said("IPO", "IPO"));
}

TEST(terms_said_keeps_only_present_terms) {
    const std::vector<std::string> vocab = {"S&P 500 index", "IPO", "AI bubble", "రవి"};
    const auto kept = terms_said(vocab, "Ravi spoke about the IPO. రవికి చెప్పాను");
    CHECK_EQ(kept, (std::vector<std::string>{"IPO", "రవి"}));
    CHECK(terms_said(vocab, "").empty());
}

TEST(drop_unsaid_vocab_from_a_section_summary) {
    json s = json::parse(R"({"title":"Exam talk","topic":"UPSC","points":[
        {"text":"UPSC prelims cut-off is 33%.","start_sec":0},
        {"text":"The AI bubble may burst.","start_sec":28},
        {"text":"An ipo is coming.","start_sec":28}],
        "terms":["UPSC","AI bubble","IPO","Kannan Gopinathan"],"hints":["Read about the AI bubble","Check CSAT"]})");
    const std::vector<std::string> vocab = {"AI bubble", "IPO", "UPSC"};
    const int n = drop_unsaid_vocab(s, vocab, "UPSC prelims cut-off is thirty three percent. CSAT. Kannan Gopinathan");
    CHECK_EQ(n, 5);
    CHECK_EQ(s["terms"], json::parse(R"(["UPSC","Kannan Gopinathan"])"));
    CHECK_EQ(s["hints"], json::parse(R"(["Check CSAT"])"));
    CHECK_EQ(s["points"].size(), (size_t)1);
    CHECK_EQ(s["points"][0]["text"].get<std::string>(), std::string("UPSC prelims cut-off is 33%."));
    // A vocabulary term that was said stays everywhere.
    json t = json::parse(R"({"title":"T","points":[{"text":"The IPO opens.","start_sec":0}],"terms":["IPO"],"hints":[]})");
    CHECK_EQ(drop_unsaid_vocab(t, vocab, "the IPO opens on Monday"), 0);
    CHECK_EQ(t["terms"].size(), (size_t)1);
    CHECK_EQ(t["points"].size(), (size_t)1);
    // No vocabulary: nothing to drop.
    CHECK_EQ(drop_unsaid_vocab(t, {}, ""), 0);
}

TEST(unclear_pieces_are_marked) {
    CHECK(piece_unclear(0.79));
    CHECK(piece_unclear(0.0));
    CHECK(!piece_unclear(0.80));
    CHECK(!piece_unclear(0.95));
    CHECK(!piece_unclear(std::nan("")));                   // no conf (older recordings, no token): not marked
    CHECK_EQ(unclear_marked("hello", 0.75), std::string("[unclear] hello"));
    CHECK_EQ(unclear_marked("hello", 0.80), std::string("hello"));
    CHECK_EQ(unclear_marked("hello", std::nan("")), std::string("hello"));
    CHECK_EQ(unclear_marked("  ", 0.5), std::string("  "));  // nothing to mark
    CHECK_EQ(std::string(kUnclearTag), std::string("[unclear] "));
    CHECK(std::isnan(piece_conf(json::parse(R"({"text":"a"})"))));
    CHECK(std::isnan(piece_conf(json::parse(R"({"text":"a","conf":null})"))));
    CHECK(std::isnan(piece_conf(json::parse(R"({"text":"a","conf":"0.5"})"))));
    CHECK_EQ(piece_conf(json::parse(R"({"text":"a","conf":0.5})")), 0.5);
}

TEST(notes_material_marks_low_conf_pieces) {
    const json pieces = json::parse(R"([
        {"start_sec":0,"text":"Clear start.","conf":0.93},
        {"start_sec":28,"text":"garbled bit","conf":0.73},
        {"start_sec":56,"text":"old piece without conf"},
        {"start_sec":84,"text":"exactly at the line","conf":0.80},
        {"start_sec":112,"text":"no token","conf":null},
        {"start_sec":140,"text":"  ","conf":0.2},
        {"start_sec":168,"text":"last low","conf":0.5}])");
    CHECK_EQ(notes_material(pieces), std::string("Clear start.\n[unclear] garbled bit\nold piece without conf\n"
                                                 "exactly at the line\nno token\n  \n[unclear] last low"));
    // Nothing unclear: the same text as the transcript (pieces joined by newlines, trimmed).
    CHECK_EQ(notes_material(json::parse(R"([{"text":" a","conf":0.9},{"text":"b "}])")), std::string("a\nb"));
    CHECK_EQ(notes_material(json::parse("{}")), std::string());
}

TEST(section_pieces_marked_for_the_prompt_only) {
    const json arr = json::parse(R"([{"start_sec":0,"text":"Clear.","conf":0.9},{"start_sec":28,"text":"Mumble.","conf":0.6},
                                     {"start_sec":56,"text":"   "},{"text":5}])");
    SectionInput in = section_input(arr);
    CHECK_EQ(in.plain.size(), (size_t)2);
    CHECK_EQ(in.marked.size(), (size_t)2);
    CHECK_EQ(in.plain[1].second, std::string("Mumble."));
    CHECK_EQ(in.marked[1].second, std::string("[unclear] Mumble."));
    CHECK_EQ(in.marked[0].second, std::string("Clear."));
    CHECK_EQ(in.starts, (std::vector<int64_t>{0, 28}));
    CHECK_EQ(in.said, std::string("Clear.\nMumble."));
    const std::string p = section_prompt(in.marked, "en", "");
    CHECK(p.find("[t=28] [unclear] Mumble.\n") != std::string::npos);
    CHECK(p.find("[t=0] Clear.\n") != std::string::npos);
    // The fallback copies the plain text: the tag is never shown as a point.
    const json fb = fallback_section_summary(in.plain);
    CHECK(fb.dump().find("[unclear]") == std::string::npos);
}

TEST(sections_text_is_titles_and_points) {
    const json secs = json::parse(R"([{"start_sec":0,"summary":{"title":"Intro","points":[{"text":"UPSC talk"}]}},
                                      {"start_sec":300,"summary":{"title":"Later","points":[{"text":"IPO news"}]}}])");
    const std::string t = sections_text(secs);
    CHECK(t.find("Intro") != std::string::npos && t.find("UPSC talk") != std::string::npos);
    CHECK(t.find("IPO news") != std::string::npos);
    CHECK_EQ(sections_text(json::array()), std::string());
}
