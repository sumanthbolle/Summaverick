#include "check.h"
#include <algorithm>
#include "sve_store.h"
#include "sve_suggest.h"

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

// The person's own reminder from an Ask answer: checked by direct_reminder_row, then saved as a row.
TEST(reminder_create_direct) {
    using sve::direct_reminder_row;
    Store s(":memory:");
    int64_t note = mk(s, "Doctor visit");
    const std::string today = "2026-11-06";
    json row = direct_reminder_row(json::parse("{\"text\":\"  Walk 20 minutes \",\"times\":[\"07:30\",\"07:30\",\"18:00\"],\"note_id\":" +
                                               std::to_string(note) + ",\"start_sec\":56}"), today);
    CHECK(row.is_object());
    CHECK_EQ(row["text"].get<std::string>(), std::string("Walk 20 minutes"));
    CHECK_EQ(row["times"], json::array({"07:30", "18:00"}));
    CHECK_EQ(row["repeat"].get<std::string>(), std::string("once"));   // not set -> once, never guessed
    CHECK_EQ(row["start_date"].get<std::string>(), today);
    CHECK(row["until_date"].is_null());
    CHECK(row["medicine_id"].is_null());
    int64_t id = s.add_reminder(row);
    CHECK(id > 0);
    json rs = s.reminders(true);
    CHECK_EQ(rs.size(), (size_t)1);
    CHECK_EQ(rs[0]["id"].get<int64_t>(), id);
    CHECK_EQ(rs[0]["note_id"].get<int64_t>(), note);
    CHECK_EQ(rs[0]["start_sec"].get<int>(), 56);
    CHECK_EQ(rs[0]["times"], json::array({"07:30", "18:00"}));
    CHECK(rs[0]["active"].get<bool>());

    json full = direct_reminder_row(json::parse(R"({"text":"x","times":["21:00"],"repeat":"daily","start_date":"2026-11-10","until_date":"2026-11-20","note_id":null})"), today);
    CHECK(full.is_object());
    CHECK_EQ(full["repeat"].get<std::string>(), std::string("daily"));
    CHECK_EQ(full["start_date"].get<std::string>(), std::string("2026-11-10"));
    CHECK_EQ(full["until_date"].get<std::string>(), std::string("2026-11-20"));
    CHECK(full["note_id"].is_null() && full["start_sec"].is_null());
    // start_sec without a note is dropped
    CHECK(direct_reminder_row(json::parse(R"({"text":"x","times":["21:00"],"start_sec":12})"), today)["start_sec"].is_null());

    // refused: no text, no times, any bad time, bad repeat/dates, bad source, not an object
    for (const char *bad : {R"({"times":["09:00"]})", R"({"text":"   ","times":["09:00"]})", R"({"text":"x"})",
                            R"({"text":"x","times":[]})", R"({"text":"x","times":"09:00"})", R"({"text":"x","times":["9am"]})",
                            R"({"text":"x","times":["09:00","25:00"]})", R"({"text":"x","times":[900]})",
                            R"({"text":"x","times":["09:00"],"repeat":"hourly"})", R"({"text":"x","times":["09:00"],"start_date":"tomorrow"})",
                            R"({"text":"x","times":["09:00"],"until_date":"2026-13-01"})",
                            R"({"text":"x","times":["09:00"],"start_date":"2026-11-10","until_date":"2026-11-09"})",
                            R"({"text":"x","times":["09:00"],"note_id":0})", R"({"text":"x","times":["09:00"],"note_id":"3"})",
                            R"({"text":"x","times":["09:00"],"note_id":3,"start_sec":-1})", R"({"text":7,"times":["09:00"]})",
                            R"(["x"])", "null"})
        CHECK(direct_reminder_row(json::parse(bad), today).is_null());
}

// An Ask-made reminder keeps an explicit medicine_id, so the medicine's cascade reaches it.
TEST(reminder_create_direct_medicine_link) {
    using sve::direct_reminder_row;
    Store s(":memory:");
    int64_t a = s.add_medicine(json::parse(R"({"name":"Amlodipine","dose":"5 mg","times":["20:00"],"repeat":"daily"})"));
    int64_t m = s.add_medicine(json::parse(R"({"name":"Metformin","dose":"500 mg","times":["08:00"],"repeat":"daily"})"));
    json row = direct_reminder_row(json::parse("{\"text\":\"my evening tablet\",\"times\":[\"20:00\"],\"repeat\":\"daily\",\"medicine_id\":" +
                                               std::to_string(a) + "}"), "2026-11-06");
    CHECK(row.is_object());
    CHECK_EQ(row["medicine_id"].get<int64_t>(), a);
    CHECK(s.add_reminder(row) > 0);
    CHECK(s.delete_medicine(a));
    CHECK_EQ(s.reminders(false).size(), (size_t)0);   // FK cascade, whatever the wording
    for (const char *bad : {R"({"text":"x","times":["09:00"],"medicine_id":0})", R"({"text":"x","times":["09:00"],"medicine_id":"2"})"})
        CHECK(direct_reminder_row(json::parse(bad), "2026-11-06").is_null());
    json meds = s.medicines();
    CHECK_EQ(sve::medicines_named("Take Metformin after food", meds), std::vector<int64_t>({m}));
    s.add_medicine(json::parse(R"({"name":"Telma","dose":"40 mg"})"));
    CHECK_EQ(sve::medicines_named("Metformin and Telma at night", s.medicines()).size(), (size_t)2);
    CHECK(sve::medicines_named("walk every evening", s.medicines()).empty());
    int64_t t40 = s.add_medicine(json::parse(R"({"name":"Telma 40","dose":"1 tablet"})"));
    std::vector<int64_t> hit = sve::medicines_named("Take Telma40 every morning", s.medicines());
    CHECK(std::find(hit.begin(), hit.end(), t40) != hit.end());
}

TEST(task_create_direct) {
    using sve::direct_task_row;
    Store s(":memory:");
    int64_t note = mk(s, "Class");
    json row = direct_task_row(json::parse("{\"text\":\" Submit project \",\"who\":\"  \",\"note_id\":" + std::to_string(note) + ",\"start_sec\":28}"));
    CHECK(row.is_object());
    CHECK_EQ(row["text"].get<std::string>(), std::string("Submit project"));
    CHECK(row["who"].is_null());
    CHECK(row["due_date"].is_null());   // never invented
    int64_t id = s.add_task(row);
    CHECK(id > 0);
    json ts = s.tasks(true);
    CHECK_EQ(ts.size(), (size_t)1);
    CHECK_EQ(ts[0]["id"].get<int64_t>(), id);
    CHECK_EQ(ts[0]["note_id"].get<int64_t>(), note);
    CHECK_EQ(ts[0]["start_sec"].get<int>(), 28);
    json full = direct_task_row(json::parse(R"({"text":"pay fees","who":"Ravi","due_date":"2026-12-01"})"));
    CHECK_EQ(full["who"].get<std::string>(), std::string("Ravi"));
    CHECK_EQ(full["due_date"].get<std::string>(), std::string("2026-12-01"));
    for (const char *bad : {R"({})", R"({"text":""})", R"({"text":"x","due_date":"next week"})", R"({"text":"x","who":5})",
                            R"({"text":"x","note_id":-4})", R"([1])"})
        CHECK(direct_task_row(json::parse(bad)).is_null());
}

TEST(wipe_clears_everything) {
    Store s(":memory:");
    int64_t note = mk(s, "Doctor visit");
    CHECK(s.set_pieces(note, json::parse(R"([{"start_sec":0,"text":"Take Amlodipine after dinner"}])")));
    CHECK(s.setting_set("profile", R"({"name":"Lakshmi","lang":"te"})"));
    CHECK(s.setting_set("insight.last_date", "2026-11-06"));
    CHECK(s.setting_set("notes.lang", "en"));   // not the assistant's: kept
    int64_t med = s.add_medicine(json::parse(R"({"name":"Amlodipine","dose":"5 mg","times":["20:00"]})"));
    CHECK(med > 0);
    CHECK(s.add_reminder(json::parse("{\"text\":\"Amlodipine\",\"times\":[\"20:00\"],\"medicine_id\":" + std::to_string(med) + "}")) > 0);
    CHECK(s.add_reminder(json::parse("{\"text\":\"call\",\"times\":[\"09:00\"],\"note_id\":" + std::to_string(note) + "}")) > 0);
    CHECK(s.add_task(json::parse(R"({"text":"submit"})")) > 0);
    int64_t topic = s.upsert_topic("topic", "Health");
    CHECK(topic > 0 && s.link_topic(note, topic));
    CHECK(s.add_suggestion(json::parse(R"({"kind":"task","payload":{"text":"x"}})")) > 0);
    CHECK(s.add_memory("vocab", "Amlodipine", std::nullopt) > 0);
    CHECK(s.add_memory("fact", "Lives in Guntur", note) > 0);
    int64_t mem = s.add_memory_item(json::parse(R"({"type":"promise","text":"Call Amma on Sunday"})"));
    CHECK(mem > 0 && s.add_memory_source(mem, note, 0, "Call Amma on Sunday", std::nullopt));
    CHECK(s.link_memory_entity(mem, topic, "topic") && s.mark_memory_section(note, 0));
    CHECK(s.log_decision("reminder", "decision", "", "yes"));
    CHECK(s.add_insight("Walk more", note) > 0);

    CHECK(s.wipe_assistant());

    CHECK(!s.setting_get("profile"));
    CHECK(!s.setting_get("insight.last_date"));
    CHECK_EQ(s.setting_get("notes.lang").value_or(""), std::string("en"));
    CHECK(s.medicines().empty());
    CHECK(s.reminders(false).empty());
    CHECK(s.tasks(false).empty());
    CHECK(s.topics("").empty());
    CHECK(s.topic_notes(topic).empty());
    CHECK(s.suggestions("").empty());
    CHECK(s.memory("").empty());
    CHECK(s.search_memory("Amlodipine", 5).empty());
    CHECK(s.memory_items(json::parse(R"({"include_wrong":true,"include_superseded":true})")).empty());
    CHECK(s.memory_sections_done(note).empty());
    CHECK(s.decisions("", "", 10).empty());
    CHECK(s.latest_insight().is_null());
    CHECK(s.pieces(note).empty());
    CHECK(s.search_pieces("Amlodipine", 5).empty());
    CHECK(s.get(note).has_value());   // notes themselves are the host's to delete
    // The store still works afterwards.
    CHECK(s.add_medicine(json::parse(R"({"name":"Telma"})")) > 0);
    CHECK(s.wipe_assistant());
    CHECK(s.medicines().empty());
}

TEST(extracted_flag_per_note) {
    Store s(":memory:");
    int64_t a = mk(s, "Old note"), b = mk(s, "Other"), c = mk(s, "With cards");
    CHECK(!s.was_extracted(a) && !s.was_extracted(b));
    CHECK(s.mark_extracted(a));
    CHECK(s.was_extracted(a) && !s.was_extracted(b));
    // A note extracted before the flag existed: its suggestions count.
    s.add_suggestion(json::parse(R"({"kind":"task","payload":{"text":"Buy milk"},"note_id":)" + std::to_string(c) + "}"));
    CHECK(s.was_extracted(c));
}

TEST(extracted_flag_goes_with_note_and_wipe) {
    Store s(":memory:");
    int64_t a = mk(s, "A"), b = mk(s, "B");
    s.mark_extracted(a);
    s.mark_extracted(b);
    CHECK(s.remove(a));
    CHECK(!s.setting_get("extracted." + std::to_string(a)).has_value());
    CHECK(s.was_extracted(b));
    s.setting_set("lang", "te");
    CHECK(s.wipe_assistant());
    CHECK(!s.was_extracted(b));
    CHECK(s.setting_get("lang").has_value());  // host settings stay
}

TEST(audio_path_can_be_replaced) {
    Store s(":memory:");
    int64_t id = s.create("n", "en", "/r/a.wav", "tx", 10);
    CHECK(s.set_text_column(id, "audio_path", "/r/a.m4a"));
    CHECK_EQ(*s.get(id)->audio_path, std::string("/r/a.m4a"));
    CHECK(!s.set_text_column(id + 99, "audio_path", "/r/b.m4a"));  // no such note
}

// Review 2026-11-25: a recording waiting for the AI download, with no transcript, was removed by age at launch with
// its transcription checkpoint. The retention clock now starts when a usable transcript is saved.
#include <sqlite3.h>
#include <cstdio>
#include <unistd.h>

static void age(const std::string &path, const char *sql) {
    sqlite3 *db = nullptr;
    sqlite3_open(path.c_str(), &db);
    sqlite3_exec(db, sql, nullptr, nullptr, nullptr);
    sqlite3_close(db);
}

TEST(retention_waits_for_a_usable_transcript) {
    const std::string path = "/tmp/sve_retention_" + std::to_string(getpid()) + ".sqlite";
    std::remove(path.c_str());
    {
        Store s(path);
        const int64_t waiting = s.create("waiting", "en", "/r/waiting.m4a", nullptr, 600);
        const int64_t done = s.create("done", "en", "/r/done.m4a", nullptr, 600);
        CHECK(s.set_text_column(done, "transcript", "We will meet on Friday."));
        // Both recorded 8 days ago.
        age(path, "UPDATE notes SET created_at=created_at-8*86400");
        // The transcript was just saved: its clock starts now, not when it was recorded.
        CHECK(s.audio_older_than(7).empty());
        age(path, "UPDATE notes SET transcribed_at=transcribed_at-8*86400 WHERE title='done'");
        auto rows = s.audio_older_than(7);
        CHECK_EQ(rows.size(), (size_t)1);
        if (!rows.empty()) CHECK_EQ(rows[0].first, done);
        // The waiting recording is never chosen, however old.
        age(path, "UPDATE notes SET created_at=created_at-365*86400 WHERE title='waiting'");
        for (const auto &r : s.audio_older_than(1)) CHECK(r.first != waiting);
        // An empty transcript is not usable; clearing the transcript stops the clock again.
        CHECK(s.set_text_column(waiting, "transcript", "   "));
        for (const auto &r : s.audio_older_than(0)) CHECK(r.first != waiting);
        CHECK(s.clear_transcript(done));
        CHECK(s.audio_older_than(0).empty());
    }
    {
        // Reopening keeps the triggers (created IF NOT EXISTS on every open).
        Store s(path);
        const int64_t again = s.create("again", "en", "/r/again.m4a", "Said in full.", 60);
        age(path, "UPDATE notes SET transcribed_at=transcribed_at-2*86400 WHERE title='again'");
        auto rows = s.audio_older_than(1);
        CHECK_EQ(rows.size(), (size_t)1);
        if (!rows.empty()) CHECK_EQ(rows[0].first, again);
    }
    std::remove(path.c_str());
}

TEST(retention_backfills_notes_from_before_the_column) {
    const std::string path = "/tmp/sve_retention_old_" + std::to_string(getpid()) + ".sqlite";
    std::remove(path.c_str());
    // A database from before transcribed_at: today's schema with the column dropped, one transcribed and one
    // waiting recording, both recorded long ago.
    {
        Store s(path);
        s.create("old", "en", "/r/old.m4a", "Said.", 60);
        s.create("wait", "en", "/r/wait.m4a", nullptr, 60);
    }
    age(path, "ALTER TABLE notes DROP COLUMN transcribed_at; UPDATE notes SET created_at=1000;");
    {
        Store s(path);
        auto rows = s.audio_older_than(7);
        CHECK_EQ(rows.size(), (size_t)1);
        if (!rows.empty()) CHECK_EQ(rows[0].second, std::string("/r/old.m4a"));
    }
    std::remove(path.c_str());
}
