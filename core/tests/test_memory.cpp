#include <cmath>
#include "check.h"
#include "sve_health.h"
#include "sve_memory.h"

using namespace sve;
using V = std::vector<std::string>;
using Conf = std::vector<std::optional<double>>;

namespace {

json cand(const std::string &type, const std::string &text, const std::string &quote) {
    json c = {{"type", type}, {"text", text}, {"quote", quote}};
    return parse_memory_candidates(json::array({c}))[0];
}

Grounded ground(const json &c, const SectionPieces &p, const V &meds = {}) {
    return ground_memory(c, p, Conf(p.size()), "2026-11-06", meds, kLowConfidence);
}

const SectionPieces kTwo = {{30, "we talked about the budget. Ravi will send the report"},
                            {58, "by Friday evening. then we had lunch"}};

}  // namespace

TEST(health_keywords_english) {
    CHECK(health_keywords("can I take paracetamol with ibuprofen?", {}) == HealthFlag::health);
    CHECK(health_keywords("what is inflation?", {}) == HealthFlag::none);
    CHECK(health_keywords("Any side effects if I skip it?", {}) == HealthFlag::health);
    CHECK(health_keywords("how many times a day?", {}) == HealthFlag::health);
    CHECK(health_keywords("I have a headache since morning", {}) == HealthFlag::health);
}

TEST(health_keywords_telugu) {
    CHECK(health_keywords("ఈ tablet ఎన్ని సార్లు వేసుకోవాలి", {}) == HealthFlag::health);
    CHECK(health_keywords("జ్వరం వస్తే ఏం చేయాలి", {}) == HealthFlag::health);
    CHECK(health_keywords("ఈ lecture లో ముఖ్యమైన పాయింట్లు ఏమిటి", {}) == HealthFlag::none);
    CHECK(health_keywords("మోకాలి నొప్పిగా ఉంది", {}) == HealthFlag::health);
    CHECK(health_keywords("ఈ మాత్ర ఎప్పుడు", {}) == HealthFlag::health);
    CHECK(health_keywords("dolo eppudu veskovali", {}) == HealthFlag::health);
    CHECK(health_keywords("jwaram vachindi", {}) == HealthFlag::health);
    CHECK(health_keywords("ఇల్లు దగ్గర ఉంది", {}) == HealthFlag::none);  // దగ్గర (near), not దగ్గు (cough)
}

TEST(health_keywords_stored_name) {
    CHECK(health_keywords("metformin timing?", {"Metformin"}) == HealthFlag::health);
    CHECK(health_keywords("Glycomet eppudu?", {"Glycomet"}) == HealthFlag::health);
    CHECK(health_keywords("Glycomet eppudu?", {}) == HealthFlag::none);
}

TEST(is_health_memory_words) {
    CHECK(is_health_memory("Dr. Rao said to come back next week", {}));
    CHECK(is_health_memory("డాక్టర్ గారు వచ్చే వారం రమ్మన్నారు", {}));
    CHECK(is_health_memory("Sugar was high in the test result", {}));
    CHECK(is_health_memory("ఆసుపత్రి కి వెళ్ళాలి", {}));
    CHECK(is_health_memory("BP check on Monday", {}));
    CHECK(!is_health_memory("Ravi will bring the chairs on Friday", {}));
    CHECK(!is_health_memory("రవి శుక్రవారం కుర్చీలు తెస్తాడు", {}));
}

TEST(parse_candidates_shape) {
    json in = json::array();
    in.push_back({{"type", "open question"}, {"text", "Who books the hall?"}, {"quote", "who will book the hall"}});
    in.push_back({{"type", "medicine"}, {"text", "x"}, {"quote", "x"}});              // not a memory type
    in.push_back({{"type", "fact"}, {"text", "  "}, {"quote", "x"}});                  // empty text
    in.push_back({{"type", "fact"}, {"text", std::string(301, 'a')}, {"quote", "x"}}); // too long
    in.push_back({{"type", "fact"}, {"text", "ok"}});                                  // no quote
    in.push_back("not an object");
    json out = parse_memory_candidates(in);
    CHECK_EQ(out.size(), 1u);
    CHECK_EQ(out[0]["type"], "question");
    CHECK_EQ(out[0]["who"], "");
    CHECK_EQ(out[0]["names"], json::array());
    CHECK_EQ(out[0]["start_sec"], 0);
    json many = json::array();
    for (int i = 0; i < 12; ++i) many.push_back({{"type", "idea"}, {"text", "idea"}, {"quote", "q"}});
    CHECK_EQ(parse_memory_candidates(many).size(), 8u);
    CHECK_EQ(parse_memory_candidates(json::object()), json::array());
}

TEST(ground_quote_not_found_drops) {
    Grounded g = ground(cand("fact", "Ravi sends the report", "Suresh will send the invoice"), kTwo);
    CHECK(!g.ok);
    CHECK_EQ(g.reason, "quote_not_found");
    g = ground(cand("fact", "x", ""), kTwo);
    CHECK(!g.ok);
    CHECK_EQ(g.reason, "quote_not_found");
}

TEST(ground_quote_across_two_pieces) {
    Grounded g = ground(cand("promise", "Ravi sends the report by Friday", "Ravi will send the report by Friday"), kTwo);
    CHECK(g.ok);
    CHECK_EQ(g.row["sources"][0]["start_sec"], 30);
    CHECK_EQ(g.row["sources"][0]["quote"], "Ravi will send the report by Friday");
    CHECK(g.needs_double_check);  // a promise
    g = ground(cand("fact", "They had lunch", "then we had lunch"), kTwo);
    CHECK(g.ok);
    CHECK_EQ(g.row["sources"][0]["start_sec"], 58);
    CHECK(!g.needs_double_check);
}

TEST(ground_number_must_be_in_quote) {
    SectionPieces p = {{0, "సరే ఐదు వందలు శుక్రవారం లోపు ఇస్తాను"}};
    json c = cand("promise", "Pay 500 by Friday", "ఐదు వందలు శుక్రవారం లోపు");
    c["numbers"] = V{"ఐదు వందలు"};
    c["dates"] = V{"శుక్రవారం"};
    Grounded g = ground(c, p);
    CHECK(g.ok);
    CHECK_EQ(g.row["checked"].size(), 2u);
    CHECK_EQ(g.row["checked"][0]["kind"], "number");
    CHECK_EQ(g.row["checked"][0]["text"], "ఐదు వందలు");
    c["text"] = "Pay 600 by Friday";
    g = ground(c, p);
    CHECK(!g.ok);
    CHECK_EQ(g.reason, "text_not_in_quote");
    c["text"] = "Pay 500 by Friday";
    c["numbers"] = V{"ఆరు వందలు"};
    g = ground(c, p);
    CHECK(!g.ok);
    CHECK_EQ(g.reason, "field_not_in_quote:number");
}

TEST(ground_name_with_case_ending) {
    SectionPieces p = {{12, "సాయంత్రం రవికి రిపోర్ట్ పంపుతాను"}};
    json c = cand("promise", "Send the report to Ravi", "రవికి రిపోర్ట్ పంపుతాను");
    c["names"] = V{"రవి"};
    Grounded g = ground(c, p);
    CHECK(g.ok);
    CHECK_EQ(g.row["checked"][0]["kind"], "name");
    CHECK_EQ(g.row["checked"][0]["text"], "రవి");
    c["names"] = V{"Ravi"};
    g = ground(c, p);
    CHECK(!g.ok);
    CHECK_EQ(g.reason, "field_not_in_quote:name");
}

TEST(ground_who_never_guessed) {
    json c = cand("promise", "Send the report by Friday", "will send the report by Friday");
    c["who"] = "Suresh";
    c["to_whom"] = "Ravi";
    Grounded g = ground(c, kTwo);
    CHECK(g.ok);
    CHECK_EQ(g.row["who"], "");
    CHECK_EQ(g.row["to_whom"], "Ravi");  // said just before the quote
    c["who"] = "I";                       // a pronoun must be in the quote itself
    g = ground(c, kTwo);
    CHECK_EQ(g.row["who"], "");
    c["quote"] = "we had lunch";
    c["text"] = "They had lunch";
    c["who"] = "we";
    g = ground(c, kTwo);
    CHECK(g.ok);
    CHECK_EQ(g.row["who"], "we");
}

TEST(ground_due_date_from_said_date) {
    SectionPieces p = {{0, "బిల్లు వచ్చే శుక్రవారం కడతాను"}};
    json c = cand("promise", "Pay the bill next Friday", "బిల్లు వచ్చే శుక్రవారం కడతాను");
    c["due_text"] = "వచ్చే శుక్రవారం";
    Grounded g = ground(c, p);
    CHECK(g.ok);
    CHECK_EQ(g.row["due_text"], "వచ్చే శుక్రవారం");
    CHECK_EQ(g.row["due_date"], "2026-11-13");
    CHECK_EQ(g.row["said_date"], "2026-11-06");
    c["due_text"] = "రేపు";  // not in the quote
    g = ground(c, p);
    CHECK(g.ok);
    CHECK_EQ(g.row["due_text"], "");
    CHECK_EQ(g.row["due_date"], "");
}

TEST(ground_medicine_candidate_dropped) {
    SectionPieces p = {{0, "take Dolo 650 twice a day"}};
    Grounded g = ground(cand("fact", "take Dolo 650 twice", "take Dolo 650 twice"), p, {"Dolo 650"});
    CHECK(!g.ok);
    CHECK_EQ(g.reason, "medicine");
    g = ground(cand("fact", "one tablet at night", "one tablet at night"), {{0, "one tablet at night"}});
    CHECK(!g.ok);
    CHECK_EQ(g.reason, "medicine");
}

TEST(ground_low_confidence_marks_low) {
    json c = cand("promise", "Ravi sends the report by Friday", "Ravi will send the report by Friday");
    Grounded g = ground_memory(c, kTwo, Conf{0.9, 0.3}, "2026-11-06", {}, 0.5);
    CHECK(g.ok);
    CHECK_EQ(g.row["confidence"], "low");
    g = ground_memory(c, kTwo, Conf{std::nullopt, std::nullopt}, "2026-11-06", {}, 0.5);
    CHECK_EQ(g.row["confidence"], "high");
    g = ground_memory(c, kTwo, Conf{}, "2026-11-06", {}, 0.5);  // no conf at all
    CHECK_EQ(g.row["confidence"], "high");
    json d = cand("fact", "They had lunch", "then we had lunch");
    g = ground_memory(d, kTwo, Conf{0.1, 0.9}, "2026-11-06", {}, 0.5);  // the quote covers piece 1 only
    CHECK_EQ(g.row["confidence"], "high");
}

TEST(ground_health_flag) {
    SectionPieces p = {{0, "doctor said come back on Monday"}};
    Grounded g = ground(cand("fact", "Come back to the doctor on Monday", "doctor said come back on Monday"), p);
    CHECK(g.ok);
    CHECK_EQ(g.row["health"], 1);
    CHECK(goes_to_queue(g.row, 1, true));
}

TEST(queue_rule) {
    json clean = {{"health", 0}, {"confidence", "high"}, {"conflict_with", nullptr}};
    CHECK(!goes_to_queue(clean, 1, true));
    CHECK(goes_to_queue(clean, 1, false));
    CHECK(goes_to_queue(clean, -1, true));
    json h = clean; h["health"] = 1;
    CHECK(goes_to_queue(h, 1, true));
    json low = clean; low["confidence"] = "low";
    CHECK(goes_to_queue(low, 1, true));
    json conflict = clean; conflict["conflict_with"] = 7;
    CHECK(goes_to_queue(conflict, 1, true));
    CHECK(!kDirectToMapDefault);
    CHECK_EQ(kLowConfidence, 0.0);
}

TEST(importance_orders_promise_over_idea_and_decays) {
    const int64_t now = 1'800'000'000;
    json promise = {{"type", "promise"}, {"checked", json::array()}, {"review", "new"},
                    {"last_seen", now}, {"last_used", nullptr}};
    json idea = promise; idea["type"] = "idea";
    double p = memory_importance(promise, 1, 0, now);
    CHECK(p > memory_importance(idea, 1, 0, now));
    json old = promise; old["last_seen"] = now - 120 * 86400;
    CHECK(std::fabs(memory_importance(old, 1, 0, now) - p / 4) < 1e-9);
    old["last_used"] = now;  // used today: no decay
    CHECK(std::fabs(memory_importance(old, 1, 0, now) - p) < 1e-9);
    json rich = promise;
    rich["checked"] = json::array({{{"kind", "date"}, {"text", "Friday"}}});
    rich["review"] = "kept";
    // 3 + 1 (date) + min(2, 0.5*4) + 2 (kept) + 0.5*min(4, 9)
    CHECK(std::fabs(memory_importance(rich, 5, 9, now) - 10.0) < 1e-9);
}

TEST(double_check_prompt_has_both) {
    std::string s = double_check_prompt("Ravi sends the report", "Ravi will send the report");
    CHECK(s.find("Does the quote say this? Answer yes or no.") != std::string::npos);
    CHECK(s.find("Ravi sends the report") != std::string::npos);
    CHECK(s.find("Ravi will send the report") != std::string::npos);
}

TEST(parse_yes_no_variants) {
    CHECK_EQ(parse_yes_no("Yes."), 1);
    CHECK_EQ(parse_yes_no("  yes"), 1);
    CHECK_EQ(parse_yes_no("అవును"), 1);
    CHECK_EQ(parse_yes_no("No"), 0);
    CHECK_EQ(parse_yes_no("కాదు"), 0);
    CHECK_EQ(parse_yes_no("లేదు."), 0);
    CHECK_EQ(parse_yes_no("maybe"), -1);
    CHECK_EQ(parse_yes_no("yesterday"), -1);
    CHECK_EQ(parse_yes_no("nothing"), -1);
    CHECK_EQ(parse_yes_no(""), -1);
}
