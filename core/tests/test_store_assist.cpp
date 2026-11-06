#include "check.h"
#include "sve_store.h"

using sve::Store;
using json = nlohmann::json;

static int64_t mk(Store &s, const char *title) { return s.create(title, "en", "a.m4a", "tx", 10); }

TEST(pieces_roundtrip_and_search) {
    Store s(":memory:");
    int64_t id = mk(s, "Finance class");
    CHECK(s.set_pieces(id, json::parse(R"([{"start_sec":0,"text":"ETF index mutual funds"},{"start_sec":28,"text":"debt funds 5%"}])")));
    CHECK_EQ(s.pieces(id).size(), (size_t)2);
    json r = s.search_pieces("debt", 10);
    CHECK_EQ(r.size(), (size_t)1);
    CHECK_EQ(r[0]["start_sec"].get<int>(), 28);
    CHECK_EQ(r[0]["title"].get<std::string>(), std::string("Finance class"));
    CHECK_EQ(r[0]["note_id"].get<int64_t>(), id);
    // replacing pieces updates the index
    CHECK(s.set_pieces(id, json::parse(R"([{"start_sec":5,"text":"gold bonds"}])")));
    CHECK_EQ(s.search_pieces("debt", 10).size(), (size_t)0);
    CHECK_EQ(s.search_pieces("gold", 10).size(), (size_t)1);
}

TEST(delete_note_keeps_confirmed_reminder) {
    Store s(":memory:");
    int64_t id = mk(s, "n");
    int64_t other = mk(s, "other");
    int64_t rid = s.add_reminder(json::parse("{\"text\":\"call\",\"times\":[\"09:00\"],\"repeat\":\"once\",\"start_date\":\"2026-11-07\",\"note_id\":" + std::to_string(id) + ",\"start_sec\":12}"));
    CHECK(rid > 0);
    int64_t tid = s.add_task(json::parse("{\"text\":\"submit\",\"note_id\":" + std::to_string(id) + "}"));
    CHECK(tid > 0);
    int64_t sid = s.add_suggestion(json::parse("{\"kind\":\"task\",\"payload\":{\"text\":\"x\"},\"quote\":\"q\",\"note_id\":" + std::to_string(id) + "}"));
    CHECK(sid > 0);
    s.add_suggestion(json::parse("{\"kind\":\"task\",\"payload\":{},\"note_id\":" + std::to_string(other) + "}"));
    CHECK(s.set_pieces(id, json::parse(R"([{"start_sec":0,"text":"hello"}])")));
    CHECK(s.add_memory("fact", "keep me not", id) > 0);
    CHECK(s.add_memory("fact", "global fact", std::nullopt) > 0);
    CHECK(s.remove(id));
    json rs = s.reminders(false);
    CHECK_EQ(rs.size(), (size_t)1);
    CHECK(rs[0]["from_deleted"].get<bool>());
    CHECK(rs[0]["note_id"].is_null());
    CHECK(rs[0]["times"].is_array());
    json ts = s.tasks(false);
    CHECK_EQ(ts.size(), (size_t)1);
    CHECK(ts[0]["from_deleted"].get<bool>());
    CHECK_EQ(s.suggestions("pending").size(), (size_t)1);
    CHECK_EQ(s.pieces(id).size(), (size_t)0);
    CHECK_EQ(s.search_pieces("hello", 5).size(), (size_t)0);
    CHECK_EQ(s.memory("").size(), (size_t)1);
    CHECK_EQ(s.search_memory("keep", 5).size(), (size_t)0);
}

TEST(pieces_survive_audio_clear) {
    Store s(":memory:");
    int64_t id = mk(s, "n");
    s.set_pieces(id, json::parse(R"([{"start_sec":0,"text":"a"},{"start_sec":4,"text":"b"}])"));
    CHECK(s.set_text_column(id, "audio_path", nullptr));
    CHECK_EQ(s.pieces(id).size(), (size_t)2);
}

TEST(topic_link_marks_stale_and_counts) {
    Store s(":memory:");
    int64_t a = mk(s, "A"), b = mk(s, "B");
    int64_t t = s.upsert_topic("topic", "Finance class");
    CHECK(t > 0);
    CHECK_EQ(s.upsert_topic("topic", "Finance class"), t);
    CHECK(s.link_topic(a, t));
    CHECK(s.set_topic_summary(t, "sum"));
    CHECK_EQ(s.topics("topic")[0]["summary_stale"].get<int>(), 0);
    CHECK(s.link_topic(b, t));
    json ts = s.topics("topic");
    CHECK_EQ(ts[0]["note_count"].get<int>(), 2);
    CHECK_EQ(ts[0]["summary_stale"].get<int>(), 1);
    CHECK_EQ(s.topic_notes(t).size(), (size_t)2);
    CHECK(s.rename_topic(t, "Finance"));
    CHECK_EQ(s.topics("")[0]["name"].get<std::string>(), std::string("Finance"));
    CHECK_EQ(s.topics("person").size(), (size_t)0);
}

TEST(merge_topics_moves_links) {
    Store s(":memory:");
    int64_t a = mk(s, "A"), b = mk(s, "B");
    int64_t t1 = s.upsert_topic("person", "Rao");
    int64_t t2 = s.upsert_topic("person", "Dr Rao");
    s.link_topic(a, t1);
    s.link_topic(a, t2);
    s.link_topic(b, t2);
    CHECK(s.merge_topics(t2, t1));
    json ts = s.topics("person");
    CHECK_EQ(ts.size(), (size_t)1);
    CHECK_EQ(ts[0]["id"].get<int64_t>(), t1);
    CHECK_EQ(ts[0]["note_count"].get<int>(), 2);
}

TEST(suggestion_status_transitions) {
    Store s(":memory:");
    int64_t sid = s.add_suggestion(json::parse(R"({"kind":"reminder","payload":{"text":"x"},"quote":"q","check_status":"check","check_fields":["times"]})"));
    CHECK(sid > 0);
    CHECK_EQ(s.suggestions("pending").size(), (size_t)1);
    json one = s.suggestion(sid);
    CHECK(one["payload"].is_object());
    CHECK(one["check_fields"].is_array());
    CHECK(one["note_id"].is_null());
    CHECK(s.set_suggestion_status(sid, "accepted"));
    CHECK_EQ(s.suggestions("pending").size(), (size_t)0);
    CHECK_EQ(s.suggestions("accepted").size(), (size_t)1);
    CHECK(s.suggestion(9999).is_null());
    CHECK(s.add_suggestion(json::parse(R"({"kind":"bogus","payload":{}})")) < 0);
}

TEST(memory_search_finds_fact) {
    Store s(":memory:");
    int64_t m = s.add_memory("fact", "Dr. Rao: under one teaspoon salt a day", std::nullopt);
    CHECK(m > 0);
    CHECK_EQ(s.search_memory("salt", 5).size(), (size_t)1);
    CHECK(s.update_memory(m, "no sugar"));
    CHECK_EQ(s.search_memory("salt", 5).size(), (size_t)0);
    CHECK_EQ(s.search_memory("sugar", 5).size(), (size_t)1);
    CHECK_EQ(s.memory("vocab").size(), (size_t)0);
    CHECK(s.delete_memory(m));
    CHECK_EQ(s.search_memory("sugar", 5).size(), (size_t)0);
}

TEST(medicine_reminder_cascade_and_misc) {
    Store s(":memory:");
    int64_t m = s.add_medicine(json::parse(R"({"name":"Metformin","dose":"500mg","when_text":"after dinner","times":["20:00"],"repeat":"daily"})"));
    CHECK(m > 0);
    CHECK(s.medicines()[0]["times"].is_array());
    CHECK(s.update_medicine(m, json::parse(R"({"dose":"250mg"})")));
    CHECK_EQ(s.medicines()[0]["dose"].get<std::string>(), std::string("250mg"));
    int64_t r = s.add_reminder(json::parse("{\"text\":\"take\",\"times\":[\"20:00\"],\"repeat\":\"daily\",\"medicine_id\":" + std::to_string(m) + "}"));
    CHECK(s.set_reminder_active(r, false));
    CHECK_EQ(s.reminders(true).size(), (size_t)0);
    CHECK_EQ(s.reminders(false).size(), (size_t)1);
    int64_t t = s.add_task(json::parse(R"({"text":"pay","who":"me","due_date":"2026-12-01"})"));
    CHECK(s.set_task_done(t, true));
    CHECK_EQ(s.tasks(true).size(), (size_t)0);
    CHECK(s.tasks(false)[0]["done"].get<bool>());
    CHECK(s.delete_medicine(m));
    CHECK_EQ(s.reminders(false).size(), (size_t)0);
    CHECK(s.log_decision("reminder", "time", "20:00", "21:00"));
    CHECK(s.log_decision("task", "who", "a", "b"));
    CHECK_EQ(s.decisions("reminder", "time", 10).size(), (size_t)1);
    CHECK_EQ(s.decisions("", "", 10).size(), (size_t)2);
    CHECK(s.latest_insight().is_null());
    int64_t i = s.add_insight("you study at night", std::nullopt);
    CHECK(i > 0);
    CHECK_EQ(s.latest_insight()["text"].get<std::string>(), std::string("you study at night"));
    CHECK(s.dismiss_insight(i));
    CHECK(s.latest_insight().is_null());
}

TEST(search_pieces_matches_any_word_ranked) {
    Store s(":memory:");
    int64_t id = mk(s, "Doctor visit");
    s.set_pieces(id, json::parse(R"([{"start_sec":0,"text":"the doctor checked BP"},{"start_sec":28,"text":"salt less than one teaspoon"},{"start_sec":56,"text":"doctor said salt is bad"}])"));
    json r = s.search_pieces("doctor salt", 10);
    CHECK_EQ(r.size(), (size_t)3);
    CHECK_EQ(r[0]["start_sec"].get<int>(), 56);  // matches both words
    CHECK_EQ(s.search_pieces("pickles", 10).size(), (size_t)0);
    s.add_memory("fact", "no pickles", std::nullopt);
    CHECK_EQ(s.search_memory("pickles sugar", 5).size(), (size_t)1);
    // note search keeps AND semantics
    CHECK_EQ(s.list("doctor zebra", 10).size(), (size_t)0);
}

TEST(suggestion_check_can_be_reflagged) {
    Store s(":memory:");
    int64_t id = s.add_suggestion(json::parse(R"({"kind":"reminder","payload":{},"check_status":"ok","check_fields":[]})"));
    CHECK(s.set_suggestion_check(id, "check", json::array({"dose"})));
    json r = s.suggestion(id);
    CHECK_EQ(r["check_status"].get<std::string>(), std::string("check"));
    CHECK_EQ(r["check_fields"], json::array({"dose"}));
    CHECK(!s.set_suggestion_check(9999, "check", json::array()));
}
