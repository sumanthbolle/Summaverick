#include "check.h"
#include <sqlite3.h>
#include <cstdlib>
#include <unistd.h>
#include "sve_store.h"
#include "sve_context.h"

using sve::Store;
using json = nlohmann::json;

namespace {

// The schema before the memory layer (user_version 0), verbatim from 28a5fab: pieces without `conf`,
// facts in `memory`.
const char *kV0Schema = R"SQL(
PRAGMA journal_mode=WAL;
PRAGMA foreign_keys=ON;
CREATE TABLE IF NOT EXISTS notes (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    title TEXT NOT NULL,
    lang TEXT NOT NULL DEFAULT 'en',
    created_at INTEGER NOT NULL,
    duration_seconds INTEGER NOT NULL DEFAULT 0,
    audio_path TEXT,
    transcript TEXT,
    short_summary TEXT,
    detailed_notes TEXT,
    revision_questions TEXT
);
CREATE VIRTUAL TABLE IF NOT EXISTS notes_fts USING fts5(
    title, transcript, short_summary, detailed_notes, revision_questions,
    content='notes', content_rowid='id', tokenize='unicode61 remove_diacritics 0'
);
CREATE TRIGGER IF NOT EXISTS notes_ai AFTER INSERT ON notes BEGIN
  INSERT INTO notes_fts(rowid,title,transcript,short_summary,detailed_notes,revision_questions)
  VALUES (new.id,new.title,new.transcript,new.short_summary,new.detailed_notes,new.revision_questions);
END;
CREATE TRIGGER IF NOT EXISTS notes_ad AFTER DELETE ON notes BEGIN
  INSERT INTO notes_fts(notes_fts,rowid,title,transcript,short_summary,detailed_notes,revision_questions)
  VALUES ('delete',old.id,old.title,old.transcript,old.short_summary,old.detailed_notes,old.revision_questions);
END;
CREATE TRIGGER IF NOT EXISTS notes_au AFTER UPDATE ON notes BEGIN
  INSERT INTO notes_fts(notes_fts,rowid,title,transcript,short_summary,detailed_notes,revision_questions)
  VALUES ('delete',old.id,old.title,old.transcript,old.short_summary,old.detailed_notes,old.revision_questions);
  INSERT INTO notes_fts(rowid,title,transcript,short_summary,detailed_notes,revision_questions)
  VALUES (new.id,new.title,new.transcript,new.short_summary,new.detailed_notes,new.revision_questions);
END;
CREATE TABLE IF NOT EXISTS settings (key TEXT PRIMARY KEY, value TEXT NOT NULL);
CREATE TABLE IF NOT EXISTS transcript_pieces (
    note_id INTEGER NOT NULL REFERENCES notes(id) ON DELETE CASCADE,
    idx INTEGER NOT NULL, start_sec INTEGER NOT NULL DEFAULT 0, text TEXT NOT NULL,
    PRIMARY KEY(note_id, idx)
);
CREATE TABLE IF NOT EXISTS sections (
    note_id INTEGER NOT NULL REFERENCES notes(id) ON DELETE CASCADE,
    idx INTEGER NOT NULL, first_piece INTEGER NOT NULL, last_piece INTEGER NOT NULL,
    start_sec INTEGER NOT NULL, end_sec INTEGER NOT NULL, summary_json TEXT,
    PRIMARY KEY(note_id, idx)
);
CREATE VIRTUAL TABLE IF NOT EXISTS pieces_fts USING fts5(
    text, content='transcript_pieces', content_rowid='rowid', tokenize='unicode61 remove_diacritics 0'
);
CREATE TRIGGER IF NOT EXISTS pieces_ai AFTER INSERT ON transcript_pieces BEGIN
  INSERT INTO pieces_fts(rowid,text) VALUES (new.rowid,new.text);
END;
CREATE TRIGGER IF NOT EXISTS pieces_ad AFTER DELETE ON transcript_pieces BEGIN
  INSERT INTO pieces_fts(pieces_fts,rowid,text) VALUES ('delete',old.rowid,old.text);
END;
CREATE TABLE IF NOT EXISTS medicines (
    id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT NOT NULL, dose TEXT, when_text TEXT,
    times TEXT NOT NULL DEFAULT '[]',
    repeat TEXT NOT NULL DEFAULT 'daily' CHECK(repeat IN ('daily','twice_daily','weekly','once')),
    until_date TEXT, created_at INTEGER NOT NULL
);
CREATE TABLE IF NOT EXISTS suggestions (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    kind TEXT NOT NULL CHECK(kind IN ('reminder','task','profile','vocab','fact')),
    status TEXT NOT NULL DEFAULT 'pending', payload TEXT NOT NULL DEFAULT '{}', quote TEXT,
    note_id INTEGER REFERENCES notes(id) ON DELETE CASCADE, start_sec INTEGER,
    medicine_id INTEGER REFERENCES medicines(id) ON DELETE CASCADE,
    check_status TEXT CHECK(check_status IN ('ok','check')), check_fields TEXT,
    created_at INTEGER NOT NULL
);
CREATE TABLE IF NOT EXISTS reminders (
    id INTEGER PRIMARY KEY AUTOINCREMENT, text TEXT NOT NULL, times TEXT NOT NULL DEFAULT '[]',
    repeat TEXT, start_date TEXT, until_date TEXT,
    note_id INTEGER REFERENCES notes(id) ON DELETE SET NULL, start_sec INTEGER,
    medicine_id INTEGER REFERENCES medicines(id) ON DELETE CASCADE,
    from_deleted INTEGER NOT NULL DEFAULT 0, active INTEGER NOT NULL DEFAULT 1, created_at INTEGER NOT NULL
);
CREATE TABLE IF NOT EXISTS tasks (
    id INTEGER PRIMARY KEY AUTOINCREMENT, text TEXT NOT NULL, who TEXT, due_date TEXT,
    done INTEGER NOT NULL DEFAULT 0, note_id INTEGER REFERENCES notes(id) ON DELETE SET NULL,
    start_sec INTEGER, from_deleted INTEGER NOT NULL DEFAULT 0, created_at INTEGER NOT NULL
);
CREATE TRIGGER IF NOT EXISTS notes_bd_assist BEFORE DELETE ON notes BEGIN
  UPDATE reminders SET from_deleted=1 WHERE note_id=old.id;
  UPDATE tasks SET from_deleted=1 WHERE note_id=old.id;
END;
CREATE TRIGGER IF NOT EXISTS notes_ad_extracted AFTER DELETE ON notes BEGIN
  DELETE FROM settings WHERE key='extracted.' || old.id;
END;
CREATE TABLE IF NOT EXISTS topics (
    id INTEGER PRIMARY KEY AUTOINCREMENT, kind TEXT NOT NULL CHECK(kind IN ('topic','person')),
    name TEXT NOT NULL, summary TEXT, summary_stale INTEGER NOT NULL DEFAULT 1, UNIQUE(kind, name)
);
CREATE TABLE IF NOT EXISTS note_topics (
    note_id INTEGER NOT NULL REFERENCES notes(id) ON DELETE CASCADE,
    topic_id INTEGER NOT NULL REFERENCES topics(id) ON DELETE CASCADE,
    PRIMARY KEY(note_id, topic_id)
);
CREATE TABLE IF NOT EXISTS memory (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    kind TEXT NOT NULL CHECK(kind IN ('fact','preference','vocab')), text TEXT NOT NULL,
    note_id INTEGER REFERENCES notes(id) ON DELETE CASCADE, created_at INTEGER NOT NULL
);
CREATE VIRTUAL TABLE IF NOT EXISTS memory_fts USING fts5(
    text, content='memory', content_rowid='id', tokenize='unicode61 remove_diacritics 0'
);
CREATE TRIGGER IF NOT EXISTS memory_ai AFTER INSERT ON memory BEGIN
  INSERT INTO memory_fts(rowid,text) VALUES (new.id,new.text);
END;
CREATE TRIGGER IF NOT EXISTS memory_ad AFTER DELETE ON memory BEGIN
  INSERT INTO memory_fts(memory_fts,rowid,text) VALUES ('delete',old.id,old.text);
END;
CREATE TRIGGER IF NOT EXISTS memory_au AFTER UPDATE ON memory BEGIN
  INSERT INTO memory_fts(memory_fts,rowid,text) VALUES ('delete',old.id,old.text);
  INSERT INTO memory_fts(rowid,text) VALUES (new.id,new.text);
END;
CREATE TABLE IF NOT EXISTS decisions (
    id INTEGER PRIMARY KEY AUTOINCREMENT, kind TEXT NOT NULL, field TEXT NOT NULL,
    from_value TEXT, to_value TEXT, created_at INTEGER NOT NULL
);
CREATE TABLE IF NOT EXISTS insights (
    id INTEGER PRIMARY KEY AUTOINCREMENT, text TEXT NOT NULL,
    note_id INTEGER REFERENCES notes(id) ON DELETE CASCADE,
    dismissed INTEGER NOT NULL DEFAULT 0, created_at INTEGER NOT NULL
);
INSERT INTO notes(id,title,created_at,transcript) VALUES (1,'Doctor visit',1762400000,'tx');
INSERT INTO transcript_pieces(note_id,idx,start_sec,text) VALUES (1,0,0,'reduce salt'),(1,1,30,'walk daily');
INSERT INTO settings(key,value) VALUES ('profile','{"name":"Lakshmi","lang":"te","routine":{"dinner":"20:00"}}');
INSERT INTO medicines(name,dose,when_text,times,created_at) VALUES ('Amlodipine','5 mg','after dinner','["20:00"]',1762400000);
INSERT INTO memory(kind,text,note_id,created_at) VALUES ('fact','Dr Rao: less salt',1,1762400000);
INSERT INTO memory(kind,text,note_id,created_at) VALUES ('preference','Short answers',NULL,1762400001);
INSERT INTO memory(kind,text,note_id,created_at) VALUES ('vocab','Dr Rao',NULL,1762400002);
)SQL";

std::string tmp_db() {
    const char *dir = getenv("TMPDIR");
    std::string t = std::string(dir && *dir ? dir : "/tmp") + "/sve_mem_XXXXXX";
    std::vector<char> buf(t.begin(), t.end());
    buf.push_back('\0');
    int fd = mkstemp(buf.data());
    if (fd >= 0) close(fd);
    return std::string(buf.data());
}

void drop_db(const std::string &p) {
    for (const char *suf : {"", "-wal", "-shm"}) unlink((p + suf).c_str());
}

std::string make_v0() {
    std::string p = tmp_db();
    unlink(p.c_str());
    sqlite3 *db = nullptr;
    sqlite3_open(p.c_str(), &db);
    char *e = nullptr;
    if (sqlite3_exec(db, kV0Schema, nullptr, nullptr, &e) != SQLITE_OK) {
        std::fprintf(stderr, "v0 schema: %s\n", e ? e : "?");
        sqlite3_free(e);
    }
    sqlite3_close(db);
    return p;
}

int user_version(const std::string &p) {
    sqlite3 *db = nullptr;
    sqlite3_open(p.c_str(), &db);
    sqlite3_stmt *st = nullptr;
    int v = -1;
    if (sqlite3_prepare_v2(db, "PRAGMA user_version", -1, &st, nullptr) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW)
        v = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    sqlite3_close(db);
    return v;
}

int64_t item(Store &s, const std::string &text, const char *type = "fact") {
    return s.add_memory_item({{"type", type}, {"text", text}});
}

}  // namespace

TEST(migrate_old_db_adds_conf_and_moves_facts) {
    std::string p = make_v0();
    {
        Store s(p);
        CHECK(s.ok());
        json ps = s.pieces(1);
        CHECK_EQ(ps.size(), (size_t)2);
        for (const auto &x : ps) CHECK(x.contains("conf") && x["conf"].is_null());
        json items = s.memory_items(json::object());
        CHECK_EQ(items.size(), (size_t)1);
        if (items.size() == 1) {
            const json &m = items[0];
            CHECK_EQ(m["text"].get<std::string>(), std::string("Dr Rao: less salt"));
            CHECK_EQ(m["type"].get<std::string>(), std::string("fact"));
            CHECK_EQ(m["origin"].get<std::string>(), std::string("migrated"));
            CHECK_EQ(m["review"].get<std::string>(), std::string("kept"));
            CHECK_EQ(m["confidence"].get<std::string>(), std::string("high"));
            CHECK_EQ(m["said_date"].get<std::string>().size(), (size_t)10);  // YYYY-MM-DD
            CHECK_EQ(m["first_seen"].get<int64_t>(), (int64_t)1762400000);
            // K4: its old note becomes a source with an empty quote.
            CHECK_EQ(m["sources"].size(), (size_t)1);
            if (m["sources"].size() == 1) {
                CHECK_EQ(m["sources"][0]["note_id"].get<int64_t>(), (int64_t)1);
                CHECK_EQ(m["sources"][0]["start_sec"].get<int64_t>(), (int64_t)0);
                CHECK_EQ(m["sources"][0]["quote"].get<std::string>(), std::string());
                CHECK_EQ(m["sources"][0]["title"].get<std::string>(), std::string("Doctor visit"));
            }
        }
        CHECK(s.memory("fact").empty());
        json prefs = s.memory("preference"), vocab = s.memory("vocab");
        CHECK_EQ(prefs.size(), (size_t)1);
        CHECK_EQ(vocab.size(), (size_t)1);
        CHECK_EQ(prefs[0]["text"].get<std::string>(), std::string("Short answers"));
        CHECK_EQ(prefs[0]["created_at"].get<int64_t>(), (int64_t)1762400001);
        CHECK_EQ(vocab[0]["text"].get<std::string>(), std::string("Dr Rao"));
        CHECK_EQ(s.search_memory("Short", 5).size(), (size_t)1);  // old index still serves preference/vocab
        CHECK_EQ(s.search_memory("salt", 5).size(), (size_t)0);
        CHECK_EQ(s.search_memory_items("salt", 5).size(), (size_t)1);
    }
    CHECK_EQ(user_version(p), 1);
    {
        Store again(p);
        CHECK(again.ok());
        CHECK_EQ(again.memory_items(json::object()).size(), (size_t)1);
        CHECK_EQ(again.memory("").size(), (size_t)2);
    }
    CHECK_EQ(user_version(p), 1);
    drop_db(p);
}

TEST(migrated_fact_goes_with_its_note) {
    std::string p = make_v0();
    {
        Store s(p);
        CHECK_EQ(s.memory_items(json::object()).size(), (size_t)1);
        CHECK(s.remove(1));
        CHECK(s.memory_items(json::object()).empty());
        CHECK(s.search_memory_items("salt", 5).empty());
    }
    drop_db(p);
}

TEST(always_block_unchanged_after_migration) {
    // Before: the same data in a store whose fact still sits in the old table.
    Store before(":memory:");
    before.setting_set("profile", R"({"name":"Lakshmi","lang":"te","routine":{"dinner":"20:00"}})");
    before.add_medicine(json::parse(R"({"name":"Amlodipine","dose":"5 mg","when_text":"after dinner","times":["20:00"]})"));
    before.add_memory("fact", "Dr Rao: less salt", std::nullopt);
    before.add_memory("preference", "Short answers", std::nullopt);
    before.add_memory("vocab", "Dr Rao", std::nullopt);
    const std::string expected =
        "About the person:\nName: Lakshmi\nPreferred language: Telugu\nRoutine: dinner 20:00\n\n"
        "Medicines:\n- Amlodipine 5 mg \xE2\x80\x94 after dinner (20:00) daily\n\n"
        "Preferences:\n- Short answers\n\nWords to spell this way: Dr Rao\n";
    CHECK_EQ(sve::always_block(before), expected);

    std::string p = make_v0();
    {
        Store s(p);
        CHECK_EQ(sve::always_block(s), expected);
        CHECK_EQ(sve::always_block(s, 2000, false), sve::always_block(before, 2000, false));
        CHECK_EQ(sve::transcription_vocab_line(s), sve::transcription_vocab_line(before));
        CHECK_EQ(sve::transcription_vocab_line(s), std::string("Spell these words this way: Dr Rao"));
    }
    drop_db(p);
}

TEST(note_delete_removes_only_single_source_memories) {
    Store s(":memory:");
    int64_t n1 = s.create("One", "en", nullptr, "tx", 10), n2 = s.create("Two", "en", nullptr, "tx", 10);
    int64_t a = item(s, "A pays rent"), b = item(s, "B brings cake");
    CHECK(a > 0 && b > 0);
    CHECK(s.add_memory_source(a, n1, 5, "A pays rent", 0.9));
    CHECK(s.add_memory_source(b, n1, 7, "B brings cake", std::nullopt));
    CHECK(s.add_memory_source(b, n2, 3, "cake from B", 0.8));
    CHECK(s.add_memory_source(b, n2, 3, "cake from B", 0.8));  // same source again: no change
    int64_t person = s.upsert_topic("person", "B");
    CHECK(s.link_memory_entity(b, person, "who"));
    CHECK(s.mark_memory_section(n1, 0));
    CHECK(s.remove(n1));
    CHECK(s.memory_item(a).is_null());
    json mb = s.memory_item(b);
    CHECK(mb.is_object());
    if (mb.is_object()) {
        CHECK_EQ(mb["sources"].size(), (size_t)1);
        CHECK_EQ(mb["sources"][0]["note_id"].get<int64_t>(), n2);
        CHECK_EQ(mb["people"].size(), (size_t)1);
        CHECK_EQ(mb["people"][0]["name"].get<std::string>(), std::string("B"));
        CHECK_EQ(mb["people"][0]["role"].get<std::string>(), std::string("who"));
    }
    CHECK(s.search_memory_items("rent", 5).empty());
    CHECK(!s.add_memory_source(b, n1, 0, "gone", std::nullopt));  // the note no longer exists
    CHECK(!s.mark_memory_section(n1, 1));
    CHECK(s.memory_sections_done(n1).empty());
}

TEST(chat_memory_without_source_survives_and_medicine_cascades) {
    Store s(":memory:");
    int64_t chat = s.add_memory_item({{"type", "fact"}, {"text", "Locker code is in the diary"}, {"origin", "chat"}});
    CHECK(chat > 0);
    int64_t med = s.add_medicine(json::parse(R"({"name":"Telma","dose":"40 mg"})"));
    int64_t mm = s.add_memory_item({{"type", "fact"}, {"text", "Telma 40 mg"}, {"origin", "medicine"}, {"medicine_id", med}, {"health", 1}});
    CHECK(mm > 0);
    CHECK_EQ(s.memory_items(json::object()).size(), (size_t)2);
    CHECK(s.delete_medicine(med));
    CHECK(s.memory_item(mm).is_null());
    CHECK(s.memory_item(chat).is_object());
    CHECK(s.add_memory_item({{"type", "bogus"}, {"text", "x"}}) < 0);
    CHECK(s.add_memory_item({{"type", "fact"}}) < 0);  // no text
    CHECK(!s.link_memory_entity(chat, s.upsert_topic("topic", "Home"), "boss"));
}

TEST(clear_transcript_drops_memory_sources_and_progress) {
    Store s(":memory:");
    int64_t n1 = s.create("One", "en", nullptr, "tx", 10), n2 = s.create("Two", "en", nullptr, "tx", 10);
    CHECK(s.set_sections(n1, json::parse(R"([{"idx":0},{"idx":1}])")));
    int64_t a = item(s, "only in one"), b = item(s, "in both");
    s.add_memory_source(a, n1, 0, "only in one", std::nullopt);
    s.add_memory_source(b, n1, 0, "in both", std::nullopt);
    s.add_memory_source(b, n2, 0, "in both", std::nullopt);
    CHECK(s.mark_memory_section(n1, 0));
    CHECK(s.mark_memory_section(n1, 1));
    CHECK_EQ(s.memory_sections_done(n1), json::parse("[0,1]"));
    CHECK(s.clear_transcript(n1));
    CHECK(s.memory_sections_done(n1).empty());
    CHECK(s.memory_item(a).is_null());
    json mb = s.memory_item(b);
    CHECK(mb.is_object() && mb["sources"].size() == 1);
    CHECK(s.memory_items({{"note_id", n1}}).empty());
    CHECK_EQ(s.memory_items({{"note_id", n2}}).size(), (size_t)1);
}

TEST(memory_filter_excludes_wrong_and_superseded) {
    Store s(":memory:");
    int64_t n = s.create("One", "en", nullptr, "tx", 10);
    int64_t old_ = item(s, "Rent is 9000"), cur = item(s, "Rent is 10000"), wrong = item(s, "Rent is free"),
            promise = s.add_memory_item({{"type", "promise"}, {"text", "Pay rent Friday"}, {"queue", 1}});
    int64_t oldest = item(s, "Rent is 8000");
    for (int64_t id : {old_, cur, wrong, promise, oldest}) s.add_memory_source(id, n, 0, "rent", std::nullopt);
    CHECK(s.update_memory_item(oldest, {{"superseded_by", old_}}));
    CHECK(s.update_memory_item(old_, {{"superseded_by", cur}}));
    CHECK(s.update_memory_item(wrong, {{"review", "wrong"}}));
    CHECK(!s.update_memory_item(cur, {{"id", 99}, {"origin", "chat"}}));  // not editable
    CHECK(!s.update_memory_item(cur, {{"review", "maybe"}}));               // CHECK constraint
    CHECK(!s.update_memory_item(9999, {{"text", "x"}}));

    json all = s.memory_items(json::object());
    CHECK_EQ(all.size(), (size_t)2);
    for (const auto &m : all) CHECK(m["id"] != wrong && m["id"] != old_ && m["id"] != oldest);
    CHECK_EQ(s.memory_items({{"include_wrong", true}}).size(), (size_t)3);
    CHECK_EQ(s.memory_items({{"include_superseded", true}}).size(), (size_t)4);
    CHECK_EQ(s.memory_items({{"reviews", {"wrong"}}}).size(), (size_t)1);
    CHECK_EQ(s.memory_items({{"types", {"promise"}}}).size(), (size_t)1);
    CHECK_EQ(s.memory_items({{"queue", 1}}).size(), (size_t)1);
    CHECK_EQ(s.memory_items({{"queue", 0}, {"types", {"fact", "promise"}}}).size(), (size_t)1);
    CHECK_EQ(s.memory_items({{"limit", 1}}).size(), (size_t)1);

    json found = s.search_memory_items("Rent", 10);
    CHECK_EQ(found.size(), (size_t)2);  // current and the promise; not wrong, not superseded
    for (const auto &m : found) CHECK(m["id"] == cur || m["id"] == promise);

    json c = s.memory_item(cur);
    CHECK(c["checked"].is_array());
    CHECK_EQ(c["history"].size(), (size_t)2);
    if (c["history"].size() == 2) {
        CHECK_EQ(c["history"][0]["id"].get<int64_t>(), old_);
        CHECK_EQ(c["history"][1]["id"].get<int64_t>(), oldest);
    }
    CHECK(s.update_memory_item(cur, {{"text", "Rent is 11000"}, {"checked", json::parse(R"([{"kind":"number","text":"11000"}])")},
                                     {"done", true}, {"importance", 0.5}}));
    c = s.memory_item(cur);
    CHECK_EQ(c["text"].get<std::string>(), std::string("Rent is 11000"));
    CHECK_EQ(c["checked"][0]["text"].get<std::string>(), std::string("11000"));
    CHECK(c["done"] == true);
    CHECK(s.search_memory_items("11000", 5).size() == 1 && s.search_memory_items("10000", 5).empty());
    CHECK(s.delete_memory_item(cur));
    CHECK(!s.delete_memory_item(cur));
    CHECK(s.memory_item(old_).is_object() && s.memory_item(old_)["superseded_by"].is_null());
}

TEST(notes_needing_memories_lists_unfinished_notes) {
    Store s(":memory:");
    int64_t none = s.create("No sections", "en", nullptr, "tx", 10);
    int64_t done = s.create("Done", "en", nullptr, "tx", 10);
    int64_t half = s.create("Half", "en", nullptr, "tx", 10);
    CHECK(none > 0);
    s.set_sections(done, json::parse(R"([{"idx":0}])"));
    s.set_sections(half, json::parse(R"([{"idx":0},{"idx":1}])"));
    s.mark_memory_section(done, 0);
    s.mark_memory_section(half, 0);
    CHECK(s.mark_memory_section(half, 0));  // again: fine
    CHECK_EQ(s.notes_needing_memories(10), json::array({half}));
    int64_t newer = s.create("Newer", "en", nullptr, "tx", 10);
    s.set_sections(newer, json::parse(R"([{"idx":0}])"));
    json due = s.notes_needing_memories(10);
    CHECK_EQ(due.size(), (size_t)2);
    CHECK(due.size() == 2 && due[0] == newer && due[1] == half);  // newest first (same second: higher id)
    CHECK_EQ(s.notes_needing_memories(1).size(), (size_t)1);
}

TEST(merge_topics_keeps_memory_links) {
    Store s(":memory:");
    int64_t m = item(s, "Ravi fixes the tap");
    int64_t ravi = s.upsert_topic("person", "Ravi"), ravi2 = s.upsert_topic("person", "Ravi garu");
    CHECK(s.link_memory_entity(m, ravi2, "who"));
    CHECK(s.link_memory_entity(m, ravi, "mentioned"));
    CHECK(s.merge_topics(ravi2, ravi));
    json people = s.memory_item(m)["people"];
    CHECK_EQ(people.size(), (size_t)2);
    for (const auto &p : people) CHECK(p["id"] == ravi);
}

TEST(wipe_clears_memory_tables) {
    Store s(":memory:");
    int64_t n = s.create("One", "en", nullptr, "tx", 10);
    s.set_sections(n, json::parse(R"([{"idx":0}])"));
    int64_t m = s.add_memory_item({{"type", "idea"}, {"text", "Paint the gate blue"}, {"origin", "chat"}});
    int64_t r = item(s, "Gate key with Ravi");
    s.add_memory_source(r, n, 0, "gate key", std::nullopt);
    s.link_memory_entity(r, s.upsert_topic("person", "Ravi"), "mentioned");
    s.mark_memory_section(n, 0);
    CHECK(m > 0 && r > 0);
    CHECK(s.wipe_assistant());
    CHECK(s.memory_items({{"include_wrong", true}, {"include_superseded", true}}).empty());
    CHECK(s.search_memory_items("gate", 5).empty());
    CHECK(s.memory_sections_done(n).empty());
    CHECK(s.get(n).has_value());
    CHECK(item(s, "After wipe") > 0);
}

TEST(memories_fts_finds_telugu_text) {
    Store s(":memory:");
    // "రవికి పుస్తకం ఇవ్వాలి" (give Ravi the book)
    int64_t m = item(s, "\xE0\xB0\xB0\xE0\xB0\xB5\xE0\xB0\xBF\xE0\xB0\x95\xE0\xB0\xBF "
                        "\xE0\xB0\xAA\xE0\xB1\x81\xE0\xB0\xB8\xE0\xB1\x8D\xE0\xB0\xA4\xE0\xB0\x95\xE0\xB0\x82 "
                        "\xE0\xB0\x87\xE0\xB0\xB5\xE0\xB1\x8D\xE0\xB0\xB5\xE0\xB0\xBE\xE0\xB0\xB2\xE0\xB0\xBF", "promise");
    CHECK(m > 0);
    json r = s.search_memory_items("\xE0\xB0\xAA\xE0\xB1\x81\xE0\xB0\xB8\xE0\xB1\x8D\xE0\xB0\xA4\xE0\xB0\x95\xE0\xB0\x82", 5);  // పుస్తకం
    CHECK_EQ(r.size(), (size_t)1);
    CHECK_EQ(s.search_memory_items("\xE0\xB0\xB0\xE0\xB0\xB5\xE0\xB0\xBF", 5).size(), (size_t)1);  // రవి: prefix of రవికి
    CHECK(s.search_memory_items("\xE0\xB0\xAA\xE0\xB0\xBE\xE0\xB0\xB2\xE0\xB1\x81", 5).empty());     // పాలు
}

TEST(pieces_store_optional_conf) {
    Store s(":memory:");
    int64_t n = s.create("One", "en", nullptr, "tx", 10);
    CHECK(s.set_pieces(n, json::parse(R"([{"start_sec":0,"text":"a","conf":0.75},{"start_sec":4,"text":"b"}])")));
    json ps = s.pieces(n);
    CHECK_EQ(ps.size(), (size_t)2);
    CHECK(ps[0]["conf"].is_number() && ps[0]["conf"].get<double>() == 0.75);
    CHECK(ps[1].contains("conf") && ps[1]["conf"].is_null());
}
