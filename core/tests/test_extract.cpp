#include "check.h"
#include "sve_extract.h"

using namespace sve;
using nlohmann::json;

TEST(parses_plain_json) {
    auto e = parse_extraction(R"({"reminders":[{"text":"take pill","quote":"q","start_sec":3}],"topics":["a"]})");
    CHECK(e.ok);
    CHECK_EQ(e.reminders.size(), (size_t)1);
    CHECK_EQ(e.reminders[0]["text"].get<std::string>(), std::string("take pill"));
    CHECK_EQ(e.topics.size(), (size_t)1);
}

TEST(parses_fenced_json) {
    auto e = parse_extraction("Here you go:\n```json\n{\"tasks\":[{\"text\":\"call {x}\"}],\"people\":[\"Ravi\"]}\n```\n");
    CHECK(e.ok);
    CHECK_EQ(e.tasks.size(), (size_t)1);
    CHECK_EQ(e.tasks[0]["text"].get<std::string>(), std::string("call {x}"));
    CHECK_EQ(e.people.size(), (size_t)1);
}

TEST(braces_in_strings_and_escapes) {
    auto e = parse_extraction("note {\"facts\":[{\"text\":\"a \\\" } b\"}]} trailing {junk");
    CHECK(e.ok);
    CHECK_EQ(e.facts.size(), (size_t)1);
}

TEST(missing_keys_become_empty) {
    auto e = parse_extraction(R"({"vocab":["x"],"topics":"oops","people":{"a":1}})");
    CHECK(e.ok);
    CHECK(e.reminders.is_array() && e.reminders.empty());
    CHECK(e.profile.is_array() && e.profile.empty());
    CHECK_EQ(e.vocab.size(), (size_t)1);
    CHECK(e.topics.is_array() && e.topics.empty());
    CHECK(e.people.is_array() && e.people.empty());
}

TEST(truncated_json_yields_empty) {
    auto e = parse_extraction(R"({"reminders":[{"text":"a")");
    CHECK(!e.ok);
    CHECK(e.reminders.is_array() && e.reminders.empty());
    CHECK(e.facts.is_array() && e.facts.empty());
}

TEST(garbage_and_non_object) {
    CHECK(!parse_extraction("").ok);
    CHECK(!parse_extraction("no json here").ok);
    CHECK(!parse_extraction("{bad json}").ok);
}

TEST(markers_rendered) {
    json p = json::array({{{"start_sec", 0}, {"text", "a"}, {"note_id", 1}}, {{"start_sec", 28}, {"text", "b"}}});
    CHECK_EQ(pieces_with_markers(p), std::string("[t=0] a\n[t=28] b\n"));
    CHECK_EQ(pieces_with_markers(json::object()), std::string(""));
}

TEST(prompt_contains_schema_and_today) {
    std::string p = extraction_prompt("CTX-BLOCK", "[t=0] hi\n", "2026-11-06");
    CHECK(p.find("\"reminders\"") != std::string::npos);
    CHECK(p.find("2026-11-06") != std::string::npos);
    CHECK(p.find("CTX-BLOCK") != std::string::npos);
    CHECK(p.find("[t=0] hi") != std::string::npos);
}

TEST(leading_prose_with_braces) {
    auto a = parse_extraction("Use {x}: {\"reminders\":[{\"text\":\"r\"}]}");
    CHECK(a.ok);
    CHECK_EQ(a.reminders.size(), (size_t)1);
    auto b = parse_extraction("stray { brace then {\"topics\":[\"t\"]}");
    CHECK(b.ok);
    CHECK_EQ(b.topics.size(), (size_t)1);
}

TEST(wrong_typed_elements_dropped) {
    auto e = parse_extraction(R"({"reminders":["take pill", null, {"text":"x"}],"vocab":[1,"BP"],"people":[null,"Ravi",{"a":1}]})");
    CHECK(e.ok);
    CHECK_EQ(e.reminders.size(), (size_t)1);
    CHECK_EQ(e.vocab.size(), (size_t)1);
    CHECK_EQ(e.vocab[0].get<std::string>(), std::string("BP"));
    CHECK_EQ(e.people.size(), (size_t)1);
}

TEST(markers_tolerate_bad_pieces) {
    json p = json::array({{{"text", "a"}}, {{"start_sec", "x"}, {"text", "b"}}, {{"start_sec", 5}, {"text", 7}}, "str", {{"start_sec", 9}, {"text", "c"}}});
    CHECK_EQ(pieces_with_markers(p), std::string("[t=0] a\n[t=0] b\n[t=9] c\n"));
}

TEST(telugu_roundtrip) {
    auto e = parse_extraction("```json\n{\"facts\":[{\"text\":\"మీరు రోజూ మాత్ర వేసుకోండి\",\"quote\":\"మాత్ర\"}],\"topics\":[\"ఆరోగ్యం\"]}\n```");
    CHECK(e.ok);
    CHECK_EQ(e.facts[0]["text"].get<std::string>(), std::string("మీరు రోజూ మాత్ర వేసుకోండి"));
    CHECK_EQ(e.topics[0].get<std::string>(), std::string("ఆరోగ్యం"));
}
