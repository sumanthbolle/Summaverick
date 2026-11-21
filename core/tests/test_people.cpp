// The people of a recording are people. On the owner's phone (2026-11-21) 4 of 7 were not: Google, Perplexity,
// "l three harris technologies" and "one of the managers from contentment depo".
#include "check.h"
#include "sve_extract.h"
#include "sve_store.h"
#include "sve_consolidate.h"

using namespace sve;

TEST(person_names_pass) {
    for (const char *n : {"Roman Goffman", "John Rankisney", "Srinivasa", "Priya", "Dr. Rao", "రవి", "Aravind Srinivas"})
        CHECK(looks_like_person_name(n));
}

TEST(companies_and_descriptions_are_not_people) {
    for (const char *n : {"google", "Perplexity", "l three harris technologies", "one of the managers from contentment depo",
                          "the doctor", "a friend", "Microsoft", "OpenAI", "Infosys Ltd", "State Bank of India",
                          "my manager", "HCSC", "Traffic Police Department", "", "   "})
        CHECK(!looks_like_person_name(n));
}

TEST(descriptions_are_told_from_names_of_things) {
    CHECK(looks_like_description("one of the managers from contentment depo"));
    CHECK(looks_like_description("my manager"));
    CHECK(!looks_like_description("google"));
    CHECK(!looks_like_description("l three harris technologies"));
}

TEST(store_turns_saved_non_people_into_topics_once) {
    Store st(":memory:");
    int64_t note = st.create("t", "en", nullptr, "tr", 60);
    int64_t g = st.upsert_topic("person", "google");
    int64_t r = st.upsert_topic("person", "Roman Goffman");
    int64_t d = st.upsert_topic("person", "one of the managers from contentment depo");
    st.link_topic(note, g);
    st.link_topic(note, r);
    st.link_topic(note, d);
    CHECK_EQ(st.reclassify_non_people(looks_like_person_name, looks_like_description), 2);
    bool google_topic = false, roman_person = false;
    for (const auto &t : st.topics("topic")) google_topic = google_topic || t.value("name", "") == "google";
    for (const auto &t : st.topics("person")) roman_person = roman_person || t.value("name", "") == "Roman Goffman";
    bool description_left = false;
    for (const auto &t : st.topics("")) description_left = description_left || t.value("name", "").rfind("one of", 0) == 0;
    CHECK(google_topic);
    CHECK(roman_person);
    CHECK(!description_left);  // a description is nobody in particular: gone, not a topic
    CHECK_EQ(st.reclassify_non_people(looks_like_person_name, looks_like_description), 0);  // nothing left to move
}

// Note 4 (2026-11-21): words the grounding check could not place ("yoi", "lanex", "hyperarity") were "mentioned"
// names, and each became a new person. A mentioned name only links someone already known.
TEST(mentioned_name_links_only_a_known_person) {
    Store st(":memory:");
    int64_t note = st.create("t", "en", nullptr, "tr", 60);
    int64_t ravi = st.upsert_topic("person", "Ravi Kumar");
    CHECK_EQ(known_person(st, note, "Ravi Kumar"), ravi);
    CHECK_EQ(known_person(st, note, "lanex"), (int64_t)0);
    CHECK_EQ((int)st.topics("person").size(), 1);
}

TEST(store_drops_people_known_only_as_mentioned_words) {
    Store st(":memory:");
    int64_t note = st.create("t", "en", nullptr, "tr", 60);
    int64_t yoi = st.upsert_topic("person", "yoi");
    int64_t roman = st.upsert_topic("person", "Roman Goffman");  // named by extraction: no memory links
    st.link_topic(note, yoi);
    st.link_topic(note, roman);
    int64_t m = st.add_memory_item({{"type", "fact"}, {"text", "old yoi"}, {"review", "kept"}});
    CHECK(m > 0);
    CHECK(st.link_memory_entity(m, yoi, "mentioned"));
    CHECK_EQ(st.drop_mentioned_only_people(), 1);
    CHECK_EQ((int)st.topics("person").size(), 1);
    CHECK_EQ(st.topics("person")[0].value("name", ""), std::string("Roman Goffman"));
}

TEST(notes_whose_memories_all_dropped_are_due_again) {
    Store st(":memory:");
    int64_t empty = st.create("a", "te", nullptr, "tr", 60), full = st.create("b", "te", nullptr, "tr", 60);
    st.set_sections(empty, nlohmann::json::array({{{"idx", 0}, {"first_piece", 0}, {"last_piece", 0}, {"title", "x"}, {"summary", "y"}}}));
    st.set_sections(full, nlohmann::json::array({{{"idx", 0}, {"first_piece", 0}, {"last_piece", 0}, {"title", "x"}, {"summary", "y"}}}));
    st.mark_memory_section(empty, 0);
    st.mark_memory_section(full, 0);
    int64_t m = st.add_memory_item({{"type", "fact"}, {"text", "kept"}, {"review", "kept"}});
    st.add_memory_source(m, full, 0, "kept", std::nullopt);
    CHECK_EQ(st.retry_notes_without_memories(), 1);
    auto due = st.notes_needing_memories(10);
    CHECK_EQ((int)due.size(), 1);
    CHECK_EQ(due[0].get<int64_t>(), empty);
}
