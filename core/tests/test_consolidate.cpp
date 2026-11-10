#include "check.h"
#include "sve_consolidate.h"
#include "sve_memory.h"
#include "sve_store.h"

using namespace sve;

namespace {

json row(int64_t id, const std::string &type, const std::string &text, const json &checked, const std::string &said,
         const json &people, const json &topics = json::array()) {
    return {{"id", id}, {"type", type}, {"text", text}, {"checked", checked}, {"said_date", said},
            {"people", people}, {"topics", topics}, {"superseded_by", nullptr}};
}

json person(int64_t id) { return json::array({{{"id", id}, {"name", "p"}, {"role", "who"}}}); }

}  // namespace

// ---------------------------------------------------------------- people

TEST(match_person_examples) {
    CHECK(match_person("Ravi", "రవి") == PersonMatch::same);
    CHECK(match_person("రవికి", "Ravi") == PersonMatch::same);
    CHECK(match_person("Lakshmi", "లక్ష్మి") == PersonMatch::same);
    CHECK(match_person("Rao", "Dr. Rao") == PersonMatch::maybe);
    CHECK(match_person("Ravi", "Raju") == PersonMatch::different);
}

TEST(match_person_more_spellings) {
    CHECK(match_person("Ravi", "ravi") == PersonMatch::same);
    CHECK(match_person("Dr. Rao", "dr rao") == PersonMatch::same);
    CHECK(match_person("Rao", "రావు") == PersonMatch::same);
    CHECK(match_person("Geetha", "గీత") == PersonMatch::same);
    CHECK(match_person("Srinivas", "శ్రీనివాస్") == PersonMatch::same);
    CHECK(match_person("Krishna", "కృష్ణ") == PersonMatch::same);
    CHECK(match_person("Ramesh", "రమేష్") == PersonMatch::same);
    CHECK(match_person("Swetha", "శ్వేత") == PersonMatch::same);
    CHECK(match_person("Anand", "ఆనంద్") == PersonMatch::same);
    CHECK(match_person("Sai", "సాయి") == PersonMatch::same);
    CHECK(match_person("Ravi garu", "రవి గారు") == PersonMatch::same);
    CHECK(match_person("రవి గారికి", "Ravi garu") == PersonMatch::same);
    // An honorific one side lacks, or one more word: only maybe.
    CHECK(match_person("Ravi", "Ravi garu") == PersonMatch::maybe);
    CHECK(match_person("Ravi", "Ravi Kumar") == PersonMatch::maybe);
    CHECK(match_person("రవి", "రవి కుమార్") == PersonMatch::maybe);
    // One letter apart: maybe, never same; too short for that: different.
    CHECK(match_person("Ravi", "Rani") == PersonMatch::maybe);
    CHECK(match_person("Ram", "Raj") == PersonMatch::different);
    CHECK(match_person("Ravi", "Sita") == PersonMatch::different);
    // A short name ending like a case ending keeps it (భాను is Bhanu, not భా).
    CHECK(match_person("Bhanu", "భాను") == PersonMatch::same);
    CHECK(match_person("", "Ravi") == PersonMatch::different);
}

TEST(match_person_case_endings_never_merge_other_names) {
    // A ని/ను/ది/దే/కే ending may be part of the name: removing it can only give "maybe".
    CHECK(match_person("Raja", "రజని") != PersonMatch::same);
    CHECK(match_person("రాజా", "రజని") != PersonMatch::same);
    CHECK(match_person("Rajani", "రజని") == PersonMatch::same);
    CHECK(match_person("Janaki", "జానకి") == PersonMatch::same);
    CHECK(match_person("Mohini", "మోహిని") == PersonMatch::same);
    CHECK(match_person("Padmini", "పద్మిని") == PersonMatch::same);
    CHECK(match_person("Padma", "పద్మిని") != PersonMatch::same);
    CHECK(match_person("Saroja", "సరోజిని") != PersonMatch::same);
    CHECK(match_person("Kamini", "కామిని") == PersonMatch::same);
    CHECK(match_person("రవికి", "Ravi") == PersonMatch::same);   // కి is a clear ending
    CHECK(match_person("రవితో", "రవి") == PersonMatch::same);
    CHECK(match_person("రవిని", "Ravi") == PersonMatch::maybe);  // ని may be the name's own: ask
}

TEST(match_person_honorific_only_asks_at_most) {
    CHECK(match_person("Sir", "Sir") == PersonMatch::maybe);
    CHECK(match_person("Sir", "sir") == PersonMatch::maybe);
    CHECK(match_person("Madam", "మేడం") == PersonMatch::maybe);
    CHECK(match_person("సార్", "Sir") == PersonMatch::maybe);
    CHECK(match_person("Sir", "Ravi") == PersonMatch::different);
}

TEST(person_norm_collapses_after_nasal_fold) {
    CHECK_EQ(person_key("Ramna"), person_key("Ranna"));
    CHECK_EQ(person_key("Ranna"), std::string("rana"));
}

TEST(transliterate_te_table) {
    CHECK_EQ(transliterate_te("రవి"), std::string("ravi"));
    CHECK_EQ(transliterate_te("లక్ష్మి"), std::string("laksmi"));
    CHECK_EQ(transliterate_te("రాము"), std::string("ramu"));
    CHECK_EQ(transliterate_te("రామ్"), std::string("ram"));
    CHECK_EQ(transliterate_te("సీత"), std::string("sita"));
    CHECK_EQ(transliterate_te("అమ్మ"), std::string("ama"));
    CHECK_EQ(transliterate_te("Ravi"), std::string("ravi"));
}

TEST(person_key_strips_honorifics_and_endings) {
    CHECK_EQ(person_key("Dr. Rao"), std::string("rao"));
    CHECK_EQ(person_key("రవికి"), std::string("ravi"));
    CHECK_EQ(person_key("రవి గారు"), std::string("ravi"));
    CHECK_EQ(person_key("Mrs Lakshmi"), std::string("laksmi"));
    CHECK_EQ(person_key("Sir"), std::string("sir"));  // all honorific: kept as the name
}

TEST(same_person_prompt_names_both) {
    std::string p = same_person_prompt("Rao", "fixes the tap", "Dr. Rao", "checked the knee");
    CHECK(p.find("Rao") != std::string::npos && p.find("Dr. Rao") != std::string::npos);
    CHECK(p.find("fixes the tap") != std::string::npos && p.find("checked the knee") != std::string::npos);
    CHECK(p.find("yes or no") != std::string::npos);
}

TEST(person_checks_lists_only_maybes_without_a_same) {
    json topics = json::array({{{"id", 1}, {"name", "Dr. Rao"}}, {{"id", 2}, {"name", "Ravi"}}});
    auto c = person_checks({"Rao", "రవి", "Sita"}, topics);
    CHECK_EQ(c.size(), (size_t)1);
    if (!c.empty()) { CHECK_EQ(c[0].first, std::string("Rao")); CHECK(c[0].second["id"] == 1); }
}

TEST(resolve_person_same_links_existing) {
    Store s(":memory:");
    int64_t n = s.create("n", "te", nullptr, "t", 1);
    int64_t ravi = s.upsert_topic("person", "Ravi");
    CHECK_EQ(resolve_person(s, n, "రవికి", "", "", {}), ravi);
    CHECK_EQ(s.topics("person").size(), (size_t)1);
    CHECK_EQ(s.note_topics(n, "person").size(), (size_t)1);
    CHECK(s.person_questions().empty());
    CHECK_EQ(resolve_person(s, n, "I", "", "", {}), 0);
    CHECK_EQ(resolve_person(s, n, "Sumanth", "sumanth", "", {}), 0);
}

TEST(resolve_person_maybe_without_model_asks) {
    Store s(":memory:");
    int64_t n = s.create("n", "en", nullptr, "t", 1);
    int64_t dr = s.upsert_topic("person", "Dr. Rao");
    int64_t rao = resolve_person(s, n, "Rao", "", "Rao fixes the tap", {});
    CHECK(rao > 0 && rao != dr);
    json q = s.person_questions();
    CHECK_EQ(q.size(), (size_t)1);
    if (!q.empty()) {
        CHECK(q[0]["a"]["id"] == dr);
        CHECK(q[0]["b"]["id"] == rao);
        CHECK(q[0]["b"]["context"] == "Rao fixes the tap");
        CHECK(q[0]["a"]["name"] == "Dr. Rao");
    }
    // The same name again links to the new person and asks nothing more.
    CHECK_EQ(resolve_person(s, n, "Rao", "", "again", {}), rao);
    CHECK_EQ(s.person_questions().size(), (size_t)1);
}

TEST(resolve_person_model_answers) {
    Store s(":memory:");
    int64_t n = s.create("n", "en", nullptr, "t", 1);
    int64_t dr = s.upsert_topic("person", "Dr. Rao");
    int64_t ravi = s.upsert_topic("person", "Ravi");
    PersonAnswers no = {{{"Rao", dr}, 0}}, yes = {{{"Rani", ravi}, 1}};
    int64_t rao = resolve_person(s, n, "Rao", "", "", no);
    CHECK(rao > 0 && rao != dr);
    CHECK(s.person_questions().empty());  // a clear no: a new person, no question
    int64_t rani = resolve_person(s, n, "Rani", "", "", yes);
    CHECK(rani > 0 && rani != ravi);       // a yes is still never merged silently
    CHECK_EQ(s.person_questions().size(), (size_t)1);
}

// ---------------------------------------------------------------- memories

TEST(classify_same_fact_again) {
    json checked = json::array({{{"kind", "number"}, {"text", "500"}}});
    json older = row(1, "fact", "Ravi paid 500 for the gate", checked, "2026-11-06", person(7));
    json newer = row(0, "fact", "Ravi paid 500 for the gate", checked, "2026-11-09", person(7));
    CHECK(classify_pair(newer, older, -1) == Pair::same);
}

TEST(classify_update_newer_date) {
    json older = row(1, "promise", "రవి శుక్రవారం డబ్బు ఇస్తాడు",
                     json::array({{{"kind", "name"}, {"text", "రవి"}}, {{"kind", "date"}, {"text", "శుక్రవారం"}}}),
                     "2026-11-06", person(7));
    json newer = row(0, "promise", "రవి సోమవారం డబ్బు ఇస్తాడు",
                     json::array({{{"kind", "name"}, {"text", "రవి"}}, {{"kind", "date"}, {"text", "సోమవారం"}}}),
                     "2026-11-09", person(7));
    CHECK(classify_pair(newer, older, -1) == Pair::update);
    json want = {{"was", "శుక్రవారం"}, {"now", "సోమవారం"}, {"said", "2026-11-09"}};
    CHECK_EQ(change_line(older, newer), want);
}

TEST(classify_contradiction_same_day) {
    json older = row(1, "fact", "The rent is 9000", json::array({{{"kind", "number"}, {"text", "9000"}}}), "2026-11-09",
                     json::array(), json::array({{{"id", 3}, {"name", "House"}}}));
    json newer = row(0, "fact", "The rent is 9500", json::array({{{"kind", "number"}, {"text", "9500"}}}), "2026-11-09",
                     json::array(), json::array({{{"id", 3}, {"name", "House"}}}));
    CHECK(classify_pair(newer, older, -1) == Pair::contradiction);
}

TEST(classify_unrelated_other_person) {
    json checked = json::array({{{"kind", "number"}, {"text", "500"}}});
    json older = row(1, "fact", "paid 500 for the gate", checked, "2026-11-06", person(7));
    json newer = row(0, "fact", "paid 500 for the gate", checked, "2026-11-09", person(8));
    CHECK(classify_pair(newer, older, -1) == Pair::unrelated);
}

TEST(classify_needs_same_type_and_words) {
    json checked = json::array({{{"kind", "number"}, {"text", "500"}}});
    json older = row(1, "fact", "Ravi paid 500 for the gate", checked, "2026-11-06", person(7));
    json idea = row(0, "idea", "Ravi paid 500 for the gate", checked, "2026-11-09", person(7));
    CHECK(classify_pair(idea, older, -1) == Pair::unrelated);
    json other = row(0, "fact", "Ravi bought 500 bricks yesterday morning", checked, "2026-11-09", person(7));
    CHECK(classify_pair(other, older, -1) == Pair::unrelated);
    // Same words, a number the older has no counterpart of: neither same nor an update.
    json more = row(0, "fact", "Ravi paid 500 for the gate", json::array({{{"kind", "date"}, {"text", "Friday"}}}),
                    "2026-11-09", person(7));
    CHECK(classify_pair(more, older, -1) == Pair::unrelated);
    // An embedding that agrees counts when the words alone do not.
    json reworded = row(0, "fact", "The gate cost Ravi 500 rupees in total", checked, "2026-11-09", person(7));
    CHECK(classify_pair(reworded, older, -1) == Pair::unrelated);
    CHECK(classify_pair(reworded, older, 0.95) == Pair::same);
    // Both with no person and no topic in common.
    json a = row(1, "fact", "The rent is 9000", checked, "2026-11-09", json::array());
    json b = row(0, "fact", "The rent is 9000", checked, "2026-11-09", json::array());
    CHECK(classify_pair(b, a, -1) == Pair::unrelated);
}

TEST(place_memory_finds_older_and_flips_for_an_older_recording) {
    json d = json::array({{{"kind", "date"}, {"text", "Friday"}}});
    json d2 = json::array({{{"kind", "date"}, {"text", "Monday"}}});
    json current = json::array({row(5, "promise", "Ravi pays on Monday", d2, "2026-11-09", person(7))});
    json late = row(0, "promise", "Ravi pays on Friday", d, "2026-11-06", person(7));
    Placement p = place_memory(late, current);
    CHECK(p.pair == Pair::update);
    CHECK_EQ(p.older, 5);
    CHECK(p.newer_is_older);
    json fresh = row(0, "promise", "Ravi pays on Friday", d, "2026-11-10", person(7));
    p = place_memory(fresh, current);
    CHECK(p.pair == Pair::update && p.older == 5 && !p.newer_is_older);
    CHECK(place_memory(row(0, "promise", "Ravi pays on Friday", d, "2026-11-10", person(8)), current).pair ==
          Pair::unrelated);
}

TEST(apply_update_supersedes_and_adds_change) {
    Store s(":memory:");
    int64_t n1 = s.create("a", "te", nullptr, "t", 1), n2 = s.create("b", "te", nullptr, "t", 1);
    json d1 = json::array({{{"kind", "date"}, {"text", "శుక్రవారం"}}}), d2 = json::array({{{"kind", "date"}, {"text", "సోమవారం"}}});
    int64_t old_id = s.add_memory_item({{"type", "promise"}, {"text", "రవి శుక్రవారం ఇస్తాడు"}, {"checked", d1},
                                        {"said_date", "2026-11-06"}, {"review", "kept"}});
    s.add_memory_source(old_id, n1, 0, "శుక్రవారం", std::nullopt);
    int64_t new_id = s.add_memory_item({{"type", "promise"}, {"text", "రవి సోమవారం ఇస్తాడు"}, {"checked", d2},
                                        {"said_date", "2026-11-09"}});
    s.add_memory_source(new_id, n2, 0, "సోమవారం", std::nullopt);
    Placement p;
    p.pair = Pair::update;
    p.older = old_id;
    CHECK(apply_placement(s, p, new_id));
    CHECK(s.memory_item(old_id)["superseded_by"] == new_id);
    CHECK(!memory_on_map(s.memory_item(old_id)));
    json got = with_change(s.memory_item(new_id));
    json want = {{"was", "శుక్రవారం"}, {"now", "సోమవారం"}, {"said", "2026-11-09"}};
    CHECK_EQ(got["change"], want);
    CHECK(!with_change(s.memory_item(old_id)).contains("change"));
}

TEST(apply_older_recording_is_stored_superseded) {
    Store s(":memory:");
    int64_t cur = s.add_memory_item({{"type", "promise"}, {"text", "x"}});
    int64_t late = s.add_memory_item({{"type", "promise"}, {"text", "y"}});
    Placement p;
    p.pair = Pair::update;
    p.older = cur;
    p.newer_is_older = true;
    CHECK(apply_placement(s, p, late));
    CHECK(s.memory_item(late)["superseded_by"] == cur);
    CHECK(s.memory_item(cur)["superseded_by"].is_null());
}

TEST(apply_contradiction_queues_both) {
    Store s(":memory:");
    int64_t a = s.add_memory_item({{"type", "fact"}, {"text", "rent 9000"}, {"review", "kept"}});
    int64_t b = s.add_memory_item({{"type", "fact"}, {"text", "rent 9500"}, {"conflict_with", a}, {"queue", 1}});
    Placement p;
    p.pair = Pair::contradiction;
    p.older = a;
    CHECK(apply_placement(s, p, b));
    CHECK(s.memory_item(a)["queue"] == 1);
    CHECK(s.memory_item(b)["queue"] == 1);
    CHECK(s.memory_item(b)["conflict_with"] == a);
}

TEST(merge_same_memory_adds_source_and_last_seen) {
    Store s(":memory:");
    int64_t n1 = s.create("a", "en", nullptr, "t", 1), n2 = s.create("b", "en", nullptr, "t", 1);
    int64_t m = s.add_memory_item({{"type", "fact"}, {"text", "Ravi paid 500"}, {"first_seen", 100}, {"last_seen", 100}});
    s.add_memory_source(m, n1, 0, "paid 500", std::nullopt);
    int64_t ravi = s.upsert_topic("person", "Ravi"), sita = s.upsert_topic("person", "Sita"), gate = s.upsert_topic("topic", "Gate");
    s.link_memory_entity(m, ravi, "who");
    // The new mention's people and topic join the older row; links it already has are not doubled.
    CHECK(merge_same_memory(s, m, n2, 12, "Ravi paid 500", json(nullptr), 5000,
                            {{ravi, "who"}, {sita, "mentioned"}, {gate, "topic"}}));
    CHECK_EQ(s.memory_item(m)["people"].size(), (size_t)2);
    CHECK_EQ(s.topic_memories(gate).size(), (size_t)0);  // not on the map (review new), but linked:
    CHECK_EQ(s.memory_items_with_topics(json::object())[0]["topics"].size(), (size_t)1);
    json r = s.memory_item(m);
    CHECK_EQ(r["sources"].size(), (size_t)2);
    CHECK(r["last_seen"] == 5000);
    CHECK_EQ(s.memory_items(json::object()).size(), (size_t)1);
}
