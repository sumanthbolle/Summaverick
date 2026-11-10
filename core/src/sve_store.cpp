#include "sve_store.h"
#include "sve_memory.h"  // memory_importance
#include <sqlite3.h>
#include <ctime>
#include <cstring>
#include <map>

namespace sve {

// Outside any transaction (neither can change inside one). journal_mode is stored in the file, foreign_keys
// is per connection.
static const char *kWal = "PRAGMA journal_mode=WAL";
static const char *kForeignKeys = "PRAGMA foreign_keys=ON";

static const char *kSchema = R"SQL(
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
    idx INTEGER NOT NULL, start_sec INTEGER NOT NULL DEFAULT 0, text TEXT NOT NULL, conf REAL,
    PRIMARY KEY(note_id, idx)
);
CREATE TABLE IF NOT EXISTS sections (
    note_id INTEGER NOT NULL REFERENCES notes(id) ON DELETE CASCADE,
    idx INTEGER NOT NULL, first_piece INTEGER NOT NULL, last_piece INTEGER NOT NULL,
    start_sec INTEGER NOT NULL, end_sec INTEGER NOT NULL, summary_json TEXT,
    PRIMARY KEY(note_id, idx)
);
-- One section's detailed notes (kind 1) or revision questions (kind 2), written part by part (engine v2). A row
-- counts only while its lang and piece range match the section's; set_sections drops rows whose range changed.
CREATE TABLE IF NOT EXISTS section_outputs (
    note_id INTEGER NOT NULL REFERENCES notes(id) ON DELETE CASCADE,
    idx INTEGER NOT NULL, kind INTEGER NOT NULL, lang TEXT NOT NULL,
    first_piece INTEGER NOT NULL, last_piece INTEGER NOT NULL, text TEXT NOT NULL, created_at INTEGER NOT NULL,
    PRIMARY KEY(note_id, idx, kind)
);
-- How often a section output was tried (a cancel does not count), and why the last try failed.
CREATE TABLE IF NOT EXISTS step_attempts (
    note_id INTEGER NOT NULL REFERENCES notes(id) ON DELETE CASCADE,
    idx INTEGER NOT NULL, kind INTEGER NOT NULL, attempts INTEGER NOT NULL DEFAULT 0, last_error TEXT,
    PRIMARY KEY(note_id, idx, kind)
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
CREATE TABLE IF NOT EXISTS memories (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    type TEXT NOT NULL CHECK(type IN ('fact','decision','promise','question','person','idea')),
    text TEXT NOT NULL, who TEXT, to_whom TEXT, due_date TEXT, due_text TEXT, said_date TEXT,
    checked TEXT NOT NULL DEFAULT '[]',
    confidence TEXT NOT NULL DEFAULT 'high' CHECK(confidence IN ('high','low')),
    review TEXT NOT NULL DEFAULT 'new' CHECK(review IN ('new','kept','corrected','wrong')),
    queue INTEGER NOT NULL DEFAULT 0, health INTEGER NOT NULL DEFAULT 0, done INTEGER NOT NULL DEFAULT 0,
    importance REAL NOT NULL DEFAULT 0,
    origin TEXT NOT NULL DEFAULT 'recording' CHECK(origin IN ('recording','chat','medicine','migrated')),
    medicine_id INTEGER REFERENCES medicines(id) ON DELETE CASCADE,
    suggestion_id INTEGER REFERENCES suggestions(id) ON DELETE SET NULL,
    superseded_by INTEGER REFERENCES memories(id) ON DELETE SET NULL,
    conflict_with INTEGER REFERENCES memories(id) ON DELETE SET NULL,
    first_seen INTEGER NOT NULL, last_seen INTEGER NOT NULL, last_used INTEGER, created_at INTEGER NOT NULL
);
CREATE TABLE IF NOT EXISTS memory_sources (
    memory_id INTEGER NOT NULL REFERENCES memories(id) ON DELETE CASCADE,
    note_id INTEGER NOT NULL REFERENCES notes(id) ON DELETE CASCADE, start_sec INTEGER NOT NULL,
    quote TEXT NOT NULL, conf REAL, PRIMARY KEY(memory_id, note_id, start_sec)
);
CREATE TABLE IF NOT EXISTS memory_entities (
    memory_id INTEGER NOT NULL REFERENCES memories(id) ON DELETE CASCADE,
    topic_id INTEGER NOT NULL REFERENCES topics(id) ON DELETE CASCADE,
    role TEXT NOT NULL CHECK(role IN ('who','to_whom','mentioned','topic')), PRIMARY KEY(memory_id, topic_id, role)
);
CREATE TABLE IF NOT EXISTS memory_progress (
    note_id INTEGER NOT NULL REFERENCES notes(id) ON DELETE CASCADE, section_idx INTEGER NOT NULL,
    PRIMARY KEY(note_id, section_idx)
);
CREATE INDEX IF NOT EXISTS memory_sources_note ON memory_sources(note_id);
CREATE INDEX IF NOT EXISTS memory_entities_topic ON memory_entities(topic_id);
-- A memory backed only by recordings goes with its last source (note delete cascades here).
CREATE TRIGGER IF NOT EXISTS memory_sources_ad AFTER DELETE ON memory_sources BEGIN
  DELETE FROM memories WHERE id=old.memory_id AND origin IN ('recording','chat','migrated')
    AND NOT EXISTS (SELECT 1 FROM memory_sources WHERE memory_id=old.memory_id);
END;
CREATE VIRTUAL TABLE IF NOT EXISTS memories_fts USING fts5(
    text, content='memories', content_rowid='id', tokenize='unicode61 remove_diacritics 0'
);
CREATE TRIGGER IF NOT EXISTS memories_ai AFTER INSERT ON memories BEGIN
  INSERT INTO memories_fts(rowid,text) VALUES (new.id,new.text);
END;
CREATE TRIGGER IF NOT EXISTS memories_ad AFTER DELETE ON memories BEGIN
  INSERT INTO memories_fts(memories_fts,rowid,text) VALUES ('delete',old.id,old.text);
END;
CREATE TRIGGER IF NOT EXISTS memories_au AFTER UPDATE OF text ON memories BEGIN
  INSERT INTO memories_fts(memories_fts,rowid,text) VALUES ('delete',old.id,old.text);
  INSERT INTO memories_fts(rowid,text) VALUES (new.id,new.text);
END;
-- A name that may be someone already known: asked, never merged silently (Task 9).
CREATE TABLE IF NOT EXISTS person_questions (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    a_topic INTEGER NOT NULL REFERENCES topics(id) ON DELETE CASCADE,
    b_topic INTEGER NOT NULL REFERENCES topics(id) ON DELETE CASCADE,
    a_context TEXT, b_context TEXT,
    status TEXT NOT NULL DEFAULT 'pending' CHECK(status IN ('pending','same','different')),
    created_at INTEGER NOT NULL
);
CREATE TABLE IF NOT EXISTS decisions (
    id INTEGER PRIMARY KEY AUTOINCREMENT, kind TEXT NOT NULL, field TEXT NOT NULL,
    from_value TEXT, to_value TEXT, created_at INTEGER NOT NULL
);
CREATE TABLE IF NOT EXISTS insights (
    id INTEGER PRIMARY KEY AUTOINCREMENT, text TEXT NOT NULL,
    note_id INTEGER REFERENCES notes(id) ON DELETE CASCADE,
    dismissed INTEGER NOT NULL DEFAULT 0, created_at INTEGER NOT NULL
);
)SQL";

Store::Store(const std::string &path) {
    if (sqlite3_open(path.c_str(), &db_) != SQLITE_OK) {
        err_ = db_ ? sqlite3_errmsg(db_) : "sqlite3_open failed";
        if (db_) { sqlite3_close(db_); db_ = nullptr; }
        return;
    }
    // Several connections may share the file (the app's model context and its store context): a writer
    // waits for another's write to finish instead of failing with SQLITE_BUSY. Set before anything runs.
    sqlite3_busy_timeout(db_, kBusyTimeoutMs);
    if (!set_wal() || !exec(kForeignKeys) || !create_schema() || !migrate()) { sqlite3_close(db_); db_ = nullptr; return; }
    sync_medicine_memories();
}

// A file already in WAL mode just reads it. Switching a new file to WAL needs every other connection off it, and
// SQLite answers SQLITE_BUSY at once there (no busy wait) when another connection is opening the same new file:
// tried again for up to the busy timeout.
bool Store::set_wal() {
    for (int waited = 0;; waited += 10) {
        if (exec(kWal)) return true;
        if ((sqlite3_errcode(db_) & 0xff) != SQLITE_BUSY || waited >= kBusyTimeoutMs) return false;
        sqlite3_sleep(10);
    }
}

// CREATE ... IF NOT EXISTS: on a file that is up to date nothing is written and no write lock is taken. Two
// connections creating a new file at once can deadlock on the lock upgrade, which SQLite answers with an
// immediate SQLITE_BUSY (no busy wait); then the schema is run again holding the write lock from the start.
bool Store::create_schema() {
    if (exec(kSchema)) return true;
    if ((sqlite3_errcode(db_) & 0xff) != SQLITE_BUSY || !begin()) return false;
    if (!exec(kSchema)) { std::string e = err_; rollback(); err_ = e; return false; }
    return commit();
}

static std::string medicine_memory_text(const json &m);

// Medicines saved before medicine memories existed get theirs now (idempotent; a failure leaves the
// medicine without one until it is saved again, and does not stop the store from opening). Reads first and
// writes only when a medicine's memory is missing or out of step, so a second open of an up-to-date file
// writes nothing; the writes run in one IMMEDIATE transaction, re-checked inside it, so two connections
// opening at once never both add the same memory.
void Store::sync_medicine_memories() {
    auto out_of_step = [this](const json &m) {
        const std::string text = medicine_memory_text(m);
        if (text.empty()) return false;
        json have = query_rows("SELECT text FROM memories WHERE origin='medicine' AND medicine_id=? ORDER BY id LIMIT 1",
                               {m["id"]});
        return have.empty() || have[0]["text"] != text;
    };
    const char *kMeds = "SELECT id,name,dose,when_text FROM medicines ORDER BY id";
    bool due = false;
    for (const auto &m : query_rows(kMeds, {})) if (out_of_step(m)) { due = true; break; }
    if (!due || !begin()) return;
    for (const auto &m : query_rows(kMeds, {})) if (out_of_step(m)) upsert_medicine_memory(m["id"].get<int64_t>());
    if (!commit()) rollback();
}

// Moves every old-table fact into memories, kept as it was (preferences and vocabulary stay in
// `memory`). Its recording becomes a source (no quote was kept), so deleting the recording still
// removes it. Each old row is deleted only after it is copied. Runs inside migrate's transaction.
bool Store::move_old_facts() {
    json facts = query_rows("SELECT id,text,note_id,created_at,date(created_at,'unixepoch','localtime') AS said "
                            "FROM memory WHERE kind='fact' ORDER BY id", {});
    for (const auto &f : facts) {
        int64_t id = add_memory_item({{"type", "fact"}, {"text", f["text"]}, {"said_date", f["said"]}, {"origin", "migrated"},
                                      {"review", "kept"}, {"confidence", "high"}, {"first_seen", f["created_at"]},
                                      {"last_seen", f["created_at"]}, {"created_at", f["created_at"]}});
        if (id <= 0) return false;
        if (!f["note_id"].is_null() &&
            !run("INSERT INTO memory_sources(memory_id,note_id,start_sec,quote) SELECT ?,id,0,'' FROM notes WHERE id=?",
                 {id, f["note_id"]}))
            return false;
        if (!run("DELETE FROM memory WHERE id=?", {f["id"]})) return false;
    }
    return true;
}

bool Store::migrate() {
    json v = query_rows("PRAGMA user_version", {});
    int64_t version = v.empty() ? 0 : v[0].value("user_version", (int64_t)0);
    if (version >= 2) return true;
    if (!begin()) return false;
    // Another connection may have migrated while this one waited for the write lock.
    v = query_rows("PRAGMA user_version", {});
    version = v.empty() ? 0 : v[0].value("user_version", (int64_t)0);
    if (version >= 2) return exec("COMMIT");
    bool ok = true;
    // 0 -> 1: a database from before the memory layer; its pieces table has no conf column.
    if (version < 1 && query_rows("SELECT 1 FROM pragma_table_info('transcript_pieces') WHERE name='conf'", {}).empty())
        ok = exec("ALTER TABLE transcript_pieces ADD COLUMN conf REAL");
    // 0 -> 1 moves the facts; 1 -> 2 sweeps, once, facts a build at version 1 still wrote to `memory`
    // (accepted fact cards, before they went to memories).
    ok = ok && move_old_facts() && exec("PRAGMA user_version=2");
    if (!ok) { std::string e = err_; exec("ROLLBACK"); err_ = e; return false; }
    return exec("COMMIT");
}

Store::~Store() { if (db_) sqlite3_close(db_); }

bool Store::exec(const char *sql) {
    char *e = nullptr;
    if (sqlite3_exec(db_, sql, nullptr, nullptr, &e) != SQLITE_OK) {
        err_ = e ? e : "exec failed";
        sqlite3_free(e);
        return false;
    }
    return true;
}

static void bind_opt(sqlite3_stmt *st, int idx, const char *v) {
    if (v) sqlite3_bind_text(st, idx, v, -1, SQLITE_TRANSIENT); else sqlite3_bind_null(st, idx);
}
static std::optional<std::string> col_opt(sqlite3_stmt *st, int i) {
    if (sqlite3_column_type(st, i) == SQLITE_NULL) return std::nullopt;
    return std::string(reinterpret_cast<const char *>(sqlite3_column_text(st, i)));
}

int64_t Store::create(const std::string &title, const std::string &lang, const char *audio_path,
                      const char *transcript, int64_t duration_seconds) {
    sqlite3_stmt *st = nullptr;
    const char *sql = "INSERT INTO notes(title,lang,created_at,duration_seconds,audio_path,transcript) VALUES(?,?,?,?,?,?)";
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) { err_ = sqlite3_errmsg(db_); return -1; }
    sqlite3_bind_text(st, 1, title.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, lang.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 3, (int64_t)time(nullptr));
    sqlite3_bind_int64(st, 4, duration_seconds);
    bind_opt(st, 5, audio_path);
    bind_opt(st, 6, transcript);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) { err_ = sqlite3_errmsg(db_); return -1; }
    return sqlite3_last_insert_rowid(db_);
}

bool Store::set_text_column(int64_t id, const char *column, const char *value) {
    // column names come from our own code only (whitelisted by the C API), never from users.
    std::string sql = std::string("UPDATE notes SET ") + column + "=? WHERE id=?";
    sqlite3_stmt *st = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) { err_ = sqlite3_errmsg(db_); return false; }
    bind_opt(st, 1, value);
    sqlite3_bind_int64(st, 2, id);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) { err_ = sqlite3_errmsg(db_); return false; }
    return sqlite3_changes(db_) > 0;
}

bool Store::remove(int64_t id) {
    sqlite3_stmt *st = nullptr;
    if (sqlite3_prepare_v2(db_, "DELETE FROM notes WHERE id=?", -1, &st, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_int64(st, 1, id);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE && sqlite3_changes(db_) > 0;
}

std::optional<NoteRow> Store::get(int64_t id) {
    sqlite3_stmt *st = nullptr;
    const char *sql = "SELECT id,title,lang,created_at,duration_seconds,audio_path,transcript,short_summary,detailed_notes,revision_questions FROM notes WHERE id=?";
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return std::nullopt;
    sqlite3_bind_int64(st, 1, id);
    std::optional<NoteRow> out;
    if (sqlite3_step(st) == SQLITE_ROW) {
        NoteRow r;
        r.id = sqlite3_column_int64(st, 0);
        r.title = reinterpret_cast<const char *>(sqlite3_column_text(st, 1));
        r.lang = reinterpret_cast<const char *>(sqlite3_column_text(st, 2));
        r.created_at = sqlite3_column_int64(st, 3);
        r.duration_seconds = sqlite3_column_int64(st, 4);
        r.audio_path = col_opt(st, 5);
        r.transcript = col_opt(st, 6);
        r.short_summary = col_opt(st, 7);
        r.detailed_notes = col_opt(st, 8);
        r.revision_questions = col_opt(st, 9);
        out = r;
    }
    sqlite3_finalize(st);
    return out;
}

// Turn free text into a tolerant FTS5 query: each word becomes a prefix term, ANDed
// (or ORed when any_word, ranked by bm25 so pieces matching more words come first).
static std::string fts_query(const std::string &q, bool any_word = false) {
    std::string out, word;
    auto flush = [&]() {
        if (word.empty()) return;
        if (!out.empty()) out += any_word ? " OR " : " ";
        out += "\"" + word + "\"*";
        word.clear();
    };
    for (unsigned char c : q) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '"' || c == '\'' || c == '*' || c == '(' || c == ')') flush();
        else word += (char)c;
    }
    flush();
    return out;
}

std::vector<NoteListRow> Store::list(const std::string &query, int limit) {
    std::vector<NoteListRow> rows;
    sqlite3_stmt *st = nullptr;
    std::string fq = fts_query(query);
    const char *sql_all =
        "SELECT id,title,lang,created_at,duration_seconds,audio_path IS NOT NULL,"
        "COALESCE(short_summary, substr(COALESCE(transcript,''),1,160)) FROM notes ORDER BY created_at DESC LIMIT ?";
    const char *sql_fts =
        "SELECT n.id,n.title,n.lang,n.created_at,n.duration_seconds,n.audio_path IS NOT NULL,"
        "snippet(notes_fts,-1,'',' ','…',24) FROM notes_fts f JOIN notes n ON n.id=f.rowid "
        "WHERE notes_fts MATCH ? ORDER BY bm25(notes_fts) LIMIT ?";
    if (fq.empty()) {
        if (sqlite3_prepare_v2(db_, sql_all, -1, &st, nullptr) != SQLITE_OK) { err_ = sqlite3_errmsg(db_); return rows; }
        sqlite3_bind_int(st, 1, limit);
    } else {
        if (sqlite3_prepare_v2(db_, sql_fts, -1, &st, nullptr) != SQLITE_OK) { err_ = sqlite3_errmsg(db_); return rows; }
        sqlite3_bind_text(st, 1, fq.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(st, 2, limit);
    }
    while (sqlite3_step(st) == SQLITE_ROW) {
        NoteListRow r;
        r.id = sqlite3_column_int64(st, 0);
        r.title = reinterpret_cast<const char *>(sqlite3_column_text(st, 1));
        r.lang = reinterpret_cast<const char *>(sqlite3_column_text(st, 2));
        r.created_at = sqlite3_column_int64(st, 3);
        r.duration_seconds = sqlite3_column_int64(st, 4);
        r.has_audio = sqlite3_column_int(st, 5) != 0;
        const unsigned char *p = sqlite3_column_text(st, 6);
        r.preview = p ? reinterpret_cast<const char *>(p) : "";
        rows.push_back(std::move(r));
    }
    sqlite3_finalize(st);
    return rows;
}

std::vector<std::pair<int64_t, std::string>> Store::audio_older_than(int days) {
    std::vector<std::pair<int64_t, std::string>> out;
    sqlite3_stmt *st = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT id,audio_path FROM notes WHERE audio_path IS NOT NULL AND created_at < ?", -1, &st, nullptr) != SQLITE_OK) return out;
    sqlite3_bind_int64(st, 1, (int64_t)time(nullptr) - (int64_t)days * 86400);
    while (sqlite3_step(st) == SQLITE_ROW)
        out.emplace_back(sqlite3_column_int64(st, 0), reinterpret_cast<const char *>(sqlite3_column_text(st, 1)));
    sqlite3_finalize(st);
    return out;
}

bool Store::setting_set(const std::string &k, const std::string &v) {
    sqlite3_stmt *st = nullptr;
    if (sqlite3_prepare_v2(db_, "INSERT INTO settings(key,value) VALUES(?,?) ON CONFLICT(key) DO UPDATE SET value=excluded.value", -1, &st, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(st, 1, k.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, v.c_str(), -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE;
}

std::optional<std::string> Store::setting_get(const std::string &k) {
    sqlite3_stmt *st = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT value FROM settings WHERE key=?", -1, &st, nullptr) != SQLITE_OK) return std::nullopt;
    sqlite3_bind_text(st, 1, k.c_str(), -1, SQLITE_TRANSIENT);
    std::optional<std::string> out;
    if (sqlite3_step(st) == SQLITE_ROW) out = std::string(reinterpret_cast<const char *>(sqlite3_column_text(st, 0)));
    sqlite3_finalize(st);
    return out;
}

// ---------------------------------------------------------------------------
// Assistant data. JSON rows use column names as keys; times/check_fields/payload are
// parsed JSON, NULL is JSON null, and flag columns are booleans.
// ---------------------------------------------------------------------------

static void bind_json(sqlite3_stmt *st, int i, const json &v) {
    if (v.is_null()) sqlite3_bind_null(st, i);
    else if (v.is_boolean()) sqlite3_bind_int(st, i, v.get<bool>() ? 1 : 0);
    else if (v.is_number_integer()) sqlite3_bind_int64(st, i, v.get<int64_t>());
    else if (v.is_number()) sqlite3_bind_double(st, i, v.get<double>());
    else if (v.is_string()) sqlite3_bind_text(st, i, v.get<std::string>().c_str(), -1, SQLITE_TRANSIENT);
    else sqlite3_bind_text(st, i, v.dump().c_str(), -1, SQLITE_TRANSIENT);
}

static bool is_flag_col(const std::string &n) {
    return n == "done" || n == "active" || n == "from_deleted" || n == "dismissed";
}
static bool is_json_col(const std::string &n) {
    return n == "times" || n == "check_fields" || n == "payload" || n == "checked";
}

json Store::query_rows(const char *sql, const std::vector<json> &binds) {
    json out = json::array();
    sqlite3_stmt *st = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) { err_ = sqlite3_errmsg(db_); return out; }
    for (size_t i = 0; i < binds.size(); i++) bind_json(st, (int)i + 1, binds[i]);
    while (sqlite3_step(st) == SQLITE_ROW) {
        json row = json::object();
        for (int c = 0; c < sqlite3_column_count(st); c++) {
            std::string name = sqlite3_column_name(st, c);
            int t = sqlite3_column_type(st, c);
            if (t == SQLITE_NULL) { row[name] = nullptr; continue; }
            if (t == SQLITE_INTEGER) {
                int64_t v = sqlite3_column_int64(st, c);
                if (is_flag_col(name)) row[name] = v != 0; else row[name] = v;
            } else if (t == SQLITE_FLOAT) {
                row[name] = sqlite3_column_double(st, c);
            } else {
                std::string s = reinterpret_cast<const char *>(sqlite3_column_text(st, c));
                if (is_json_col(name)) {
                    json p = json::parse(s, nullptr, false);
                    row[name] = p.is_discarded() ? json(nullptr) : p;
                } else row[name] = s;
            }
        }
        out.push_back(std::move(row));
    }
    sqlite3_finalize(st);
    return out;
}

bool Store::run(const char *sql, const std::vector<json> &binds, int64_t *last_id) {
    sqlite3_stmt *st = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) { err_ = sqlite3_errmsg(db_); return false; }
    for (size_t i = 0; i < binds.size(); i++) bind_json(st, (int)i + 1, binds[i]);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) { err_ = sqlite3_errmsg(db_); return false; }
    if (last_id) *last_id = sqlite3_last_insert_rowid(db_);
    return true;
}

static json jget(const json &o, const char *k, json dflt = nullptr) {
    if (!o.is_object()) return dflt;
    auto it = o.find(k);
    return it == o.end() ? dflt : *it;
}
// JSON-typed value stored as text (null stays NULL).
static json jtext(const json &v, bool null_ok = true) {
    if (v.is_null()) return null_ok ? json(nullptr) : json("[]");
    return v.is_string() ? v : json(v.dump());
}
static json now_j() { return (int64_t)time(nullptr); }

// Assistant searches match any word: questions carry words the piece never says.
static std::string to_fts(const std::string &q) { return fts_query(q, true); }

// pieces ---------------------------------------------------------------------

bool Store::set_pieces(int64_t note_id, const json &pieces) {
    if (!pieces.is_array()) { err_ = "pieces must be an array"; return false; }
    if (!begin()) return false;
    bool ok = run("DELETE FROM transcript_pieces WHERE note_id=?", {note_id});
    int idx = 0;
    for (const auto &p : pieces) {
        if (!ok) break;
        json conf = jget(p, "conf");
        ok = run("INSERT INTO transcript_pieces(note_id,idx,start_sec,text,conf) VALUES(?,?,?,?,?)",
                 {note_id, idx++, jget(p, "start_sec", 0), jget(p, "text", ""), conf.is_number() ? conf : json(nullptr)});
    }
    if (!ok) { std::string e = err_; exec("ROLLBACK"); err_ = e; return false; }
    return exec("COMMIT");
}

json Store::pieces(int64_t note_id) {
    return query_rows("SELECT idx,start_sec,text,conf FROM transcript_pieces WHERE note_id=? ORDER BY idx", {note_id});
}

json Store::search_pieces(const std::string &q, int limit) {
    std::string fq = to_fts(q);
    if (fq.empty()) return json::array();
    return query_rows(
        "SELECT p.note_id AS note_id, n.title AS title, p.start_sec AS start_sec, p.text AS text "
        "FROM pieces_fts f JOIN transcript_pieces p ON p.rowid=f.rowid JOIN notes n ON n.id=p.note_id "
        "WHERE pieces_fts MATCH ? ORDER BY bm25(pieces_fts) LIMIT ?", {fq, limit});
}

json Store::search_pieces_between(const std::string &q, int64_t from, int64_t to, int limit) {
    std::string fq = to_fts(q);
    if (fq.empty()) return json::array();
    return query_rows(
        "SELECT p.note_id AS note_id, n.title AS title, p.start_sec AS start_sec, p.text AS text "
        "FROM pieces_fts f JOIN transcript_pieces p ON p.rowid=f.rowid JOIN notes n ON n.id=p.note_id "
        "WHERE pieces_fts MATCH ? AND n.created_at >= ? AND n.created_at < ? ORDER BY bm25(pieces_fts) LIMIT ?",
        {fq, from, to, limit});
}

json Store::note_summaries(int64_t from, int64_t to, int limit, bool oldest_first) {
    const std::string order = oldest_first ? "ASC" : "DESC";
    const std::string cols = "SELECT id,title,created_at,short_summary,revision_questions FROM notes ";
    if (to == 0) return query_rows((cols + "ORDER BY created_at " + order + ", id " + order + " LIMIT ?").c_str(), {limit});
    return query_rows((cols + "WHERE created_at >= ? AND created_at < ? ORDER BY created_at " + order + ", id " + order + " LIMIT ?").c_str(),
                      {from, to, limit});
}

// suggestions ----------------------------------------------------------------

static const char *kSuggestionCols =
    "id,kind,status,payload,quote,note_id,start_sec,medicine_id,check_status,check_fields,created_at";

int64_t Store::add_suggestion(const json &s) {
    int64_t id = -1;
    json payload = jget(s, "payload", json::object());
    bool ok = run("INSERT INTO suggestions(kind,status,payload,quote,note_id,start_sec,medicine_id,check_status,check_fields,created_at) "
                  "VALUES(?,?,?,?,?,?,?,?,?,?)",
                  {jget(s, "kind"), jget(s, "status", "pending"), jtext(payload), jget(s, "quote"), jget(s, "note_id"),
                   jget(s, "start_sec"), jget(s, "medicine_id"), jget(s, "check_status"),
                   jtext(jget(s, "check_fields")), now_j()}, &id);
    return ok ? id : -1;
}

json Store::suggestions(const std::string &status) {
    std::string sql = std::string("SELECT ") + kSuggestionCols + " FROM suggestions ";
    if (status.empty()) return query_rows((sql + "ORDER BY id").c_str(), {});
    return query_rows((sql + "WHERE status=? ORDER BY id").c_str(), {status});
}

bool Store::set_suggestion_status(int64_t id, const std::string &status) {
    // "superseded" only replaces a card still waiting: a card the person accepted or declined on the screens'
    // connection a moment earlier keeps its decision.
    return run("UPDATE suggestions SET status=? WHERE id=? AND (?<>'superseded' OR status='pending')", {status, id, status}) &&
           sqlite3_changes(db_) > 0;
}

bool Store::set_suggestion_check(int64_t id, const std::string &check_status, const json &check_fields) {
    return run("UPDATE suggestions SET check_status=?, check_fields=? WHERE id=?", {check_status, jtext(check_fields), id}) &&
           sqlite3_changes(db_) > 0;
}

json Store::suggestion(int64_t id) {
    json r = query_rows((std::string("SELECT ") + kSuggestionCols + " FROM suggestions WHERE id=?").c_str(), {id});
    return r.empty() ? json(nullptr) : r[0];
}

// reminders / tasks ----------------------------------------------------------

static const char *kReminderCols =
    "id,text,times,repeat,start_date,until_date,note_id,start_sec,medicine_id,from_deleted,active,created_at";

int64_t Store::add_reminder(const json &r) {
    int64_t id = -1;
    bool ok = run("INSERT INTO reminders(text,times,repeat,start_date,until_date,note_id,start_sec,medicine_id,created_at) VALUES(?,?,?,?,?,?,?,?,?)",
                  {jget(r, "text", ""), jtext(jget(r, "times", json::array())), jget(r, "repeat"), jget(r, "start_date"),
                   jget(r, "until_date"), jget(r, "note_id"), jget(r, "start_sec"), jget(r, "medicine_id"), now_j()}, &id);
    return ok ? id : -1;
}

json Store::reminders(bool active_only) {
    std::string sql = std::string("SELECT ") + kReminderCols + " FROM reminders " + (active_only ? "WHERE active=1 " : "") + "ORDER BY id";
    return query_rows(sql.c_str(), {});
}

bool Store::set_reminder_active(int64_t id, bool active) {
    return run("UPDATE reminders SET active=? WHERE id=?", {active ? 1 : 0, id}) && sqlite3_changes(db_) > 0;
}

int64_t Store::add_task(const json &t) {
    int64_t id = -1;
    bool ok = run("INSERT INTO tasks(text,who,due_date,note_id,start_sec,created_at) VALUES(?,?,?,?,?,?)",
                  {jget(t, "text", ""), jget(t, "who"), jget(t, "due_date"), jget(t, "note_id"), jget(t, "start_sec"), now_j()}, &id);
    return ok ? id : -1;
}

json Store::tasks(bool open_only) {
    std::string sql = std::string("SELECT id,text,who,due_date,done,note_id,start_sec,from_deleted,created_at FROM tasks ") +
                      (open_only ? "WHERE done=0 " : "") + "ORDER BY id";
    return query_rows(sql.c_str(), {});
}

bool Store::set_task_done(int64_t id, bool done) {
    return run("UPDATE tasks SET done=? WHERE id=?", {done ? 1 : 0, id}) && sqlite3_changes(db_) > 0;
}

// topics ---------------------------------------------------------------------

int64_t Store::upsert_topic(const std::string &kind, const std::string &name) {
    if (!run("INSERT INTO topics(kind,name) VALUES(?,?) ON CONFLICT(kind,name) DO NOTHING", {kind, name})) return -1;
    json r = query_rows("SELECT id FROM topics WHERE kind=? AND name=?", {kind, name});
    return r.empty() ? -1 : r[0]["id"].get<int64_t>();
}

bool Store::link_topic(int64_t note_id, int64_t topic_id) {
    if (!run("INSERT OR IGNORE INTO note_topics(note_id,topic_id) VALUES(?,?)", {note_id, topic_id})) return false;
    return run("UPDATE topics SET summary_stale=1 WHERE id=?", {topic_id});
}

json Store::topics(const std::string &kind) {
    std::string sql =
        "SELECT t.id AS id, t.kind AS kind, t.name AS name, t.summary AS summary, t.summary_stale AS summary_stale, "
        "(SELECT COUNT(*) FROM note_topics nt WHERE nt.topic_id=t.id) AS note_count, "
        "(SELECT COUNT(*) FROM tasks k JOIN note_topics nt ON nt.note_id=k.note_id WHERE nt.topic_id=t.id AND k.done=0) AS open_tasks, "
        "(SELECT n.title FROM note_topics nt JOIN notes n ON n.id=nt.note_id WHERE nt.topic_id=t.id "
        " ORDER BY n.created_at DESC, n.id DESC LIMIT 1) AS last_title FROM topics t ";
    if (kind.empty()) return query_rows((sql + "ORDER BY t.name").c_str(), {});
    return query_rows((sql + "WHERE t.kind=? ORDER BY t.name").c_str(), {kind});
}

json Store::topic_notes(int64_t topic_id) {
    return query_rows("SELECT n.id AS id, n.title AS title, n.created_at AS created_at, n.short_summary AS short_summary "
                      "FROM note_topics nt JOIN notes n ON n.id=nt.note_id WHERE nt.topic_id=? "
                      "ORDER BY n.created_at DESC, n.id DESC", {topic_id});
}

json Store::note_topics(int64_t note_id, const std::string &kind) {
    return query_rows("SELECT t.id AS id, t.kind AS kind, t.name AS name FROM note_topics nt JOIN topics t ON t.id=nt.topic_id "
                      "WHERE nt.note_id=? AND t.kind=? ORDER BY nt.rowid", {note_id, kind});
}

bool Store::set_topic_summary(int64_t id, const std::string &summary) {
    return run("UPDATE topics SET summary=?, summary_stale=0 WHERE id=?", {summary, id}) && sqlite3_changes(db_) > 0;
}

bool Store::rename_topic(int64_t id, const std::string &name) {
    return run("UPDATE topics SET name=? WHERE id=?", {name, id}) && sqlite3_changes(db_) > 0;
}

bool Store::merge_topics_body(int64_t from, int64_t into) {
    return run("INSERT OR IGNORE INTO note_topics(note_id,topic_id) SELECT note_id,? FROM note_topics WHERE topic_id=?", {into, from}) &&
           // Memory links move too: deleting `from` would otherwise cascade them away.
           run("INSERT OR IGNORE INTO memory_entities(memory_id,topic_id,role) SELECT memory_id,?,role FROM memory_entities "
               "WHERE topic_id=?", {into, from}) &&
           // Questions about `from` are now about `into`; a pending one about the two of them is answered by this.
           run("UPDATE person_questions SET a_topic=? WHERE a_topic=?", {into, from}) &&
           run("UPDATE person_questions SET b_topic=? WHERE b_topic=?", {into, from}) &&
           run("DELETE FROM person_questions WHERE a_topic=b_topic AND status='pending'", {}) &&
           // Two pending questions that became the same pair: the older one stays.
           run("DELETE FROM person_questions WHERE status='pending' AND EXISTS (SELECT 1 FROM person_questions o "
               "WHERE o.id<person_questions.id AND ((o.a_topic=person_questions.a_topic AND o.b_topic=person_questions.b_topic) "
               "OR (o.a_topic=person_questions.b_topic AND o.b_topic=person_questions.a_topic)))", {}) &&
           run("DELETE FROM topics WHERE id=?", {from}) &&
           run("UPDATE topics SET summary_stale=1 WHERE id=?", {into});
}

bool Store::merge_topics(int64_t from, int64_t into) {
    if (from == into) return false;
    if (query_rows("SELECT id FROM topics WHERE id=?", {from}).empty() ||
        query_rows("SELECT id FROM topics WHERE id=?", {into}).empty()) return false;
    if (!begin()) return false;
    if (!merge_topics_body(from, into)) { std::string e = err_; exec("ROLLBACK"); err_ = e; return false; }
    return exec("COMMIT");
}

// person questions -----------------------------------------------------------

int64_t Store::add_person_question(int64_t a, int64_t b, const std::string &a_context, const std::string &b_context) {
    if (a == b) { err_ = "a person is not asked about themselves"; return -1; }
    json r = query_rows("SELECT id FROM person_questions WHERE (a_topic=?1 AND b_topic=?2) OR (a_topic=?2 AND b_topic=?1) "
                        "ORDER BY id LIMIT 1", {a, b});
    if (!r.empty()) return r[0]["id"].get<int64_t>();
    int64_t id = -1;
    bool ok = run("INSERT INTO person_questions(a_topic,b_topic,a_context,b_context,created_at) VALUES(?,?,?,?,?)",
                  {a, b, a_context, b_context, now_j()}, &id);
    return ok ? id : -1;
}

json Store::person_questions() {
    json out = json::array();
    for (auto &r : query_rows("SELECT q.id AS id, a.id AS a_id, a.name AS a_name, q.a_context AS a_context, b.id AS b_id, "
                              "b.name AS b_name, q.b_context AS b_context FROM person_questions q "
                              "JOIN topics a ON a.id=q.a_topic JOIN topics b ON b.id=q.b_topic "
                              "WHERE q.status='pending' ORDER BY q.id", {})) {
        auto ctx = [](const json &v) { return v.is_string() ? v : json(""); };
        out.push_back({{"id", r["id"]},
                       {"a", {{"id", r["a_id"]}, {"name", r["a_name"]}, {"context", ctx(r["a_context"])}}},
                       {"b", {{"id", r["b_id"]}, {"name", r["b_name"]}, {"context", ctx(r["b_context"])}}}});
    }
    return out;
}

bool Store::answer_person_question(int64_t id, bool same) {
    json r = query_rows("SELECT a_topic, b_topic FROM person_questions WHERE id=? AND status='pending'", {id});
    if (r.empty()) { err_ = "no such pending question"; return false; }
    const int64_t a = r[0]["a_topic"].get<int64_t>(), b = r[0]["b_topic"].get<int64_t>();
    if (!begin()) return false;
    bool ok = run("UPDATE person_questions SET status=? WHERE id=?", {same ? "same" : "different", id}) &&
              (!same || merge_topics_body(b, a));
    if (!ok) { std::string e = err_; exec("ROLLBACK"); err_ = e; return false; }
    return exec("COMMIT");
}

std::string Store::person_context(int64_t topic_id) {
    json r = query_rows("SELECT m.text AS t FROM memory_entities e JOIN memories m ON m.id=e.memory_id WHERE e.topic_id=? "
                        "AND m.review<>'wrong' AND m.superseded_by IS NULL ORDER BY m.last_seen DESC, m.id DESC LIMIT 1", {topic_id});
    if (!r.empty() && r[0]["t"].is_string()) return r[0]["t"].get<std::string>();
    r = query_rows("SELECT n.title AS t FROM note_topics nt JOIN notes n ON n.id=nt.note_id WHERE nt.topic_id=? "
                   "ORDER BY n.created_at DESC, n.id DESC LIMIT 1", {topic_id});
    return !r.empty() && r[0]["t"].is_string() ? r[0]["t"].get<std::string>() : "";
}

// memory ---------------------------------------------------------------------

int64_t Store::add_memory(const std::string &kind, const std::string &text, std::optional<int64_t> note_id) {
    int64_t id = -1;
    bool ok = run("INSERT INTO memory(kind,text,note_id,created_at) VALUES(?,?,?,?)",
                  {kind, text, note_id ? json(*note_id) : json(nullptr), now_j()}, &id);
    return ok ? id : -1;
}

json Store::memory(const std::string &kind) {
    const char *base = "SELECT id,kind,text,note_id,created_at FROM memory ";
    if (kind.empty()) return query_rows((std::string(base) + "ORDER BY id").c_str(), {});
    return query_rows((std::string(base) + "WHERE kind=? ORDER BY id").c_str(), {kind});
}

json Store::search_memory(const std::string &q, int limit) {
    std::string fq = to_fts(q);
    if (fq.empty()) return json::array();
    return query_rows("SELECT m.id AS id, m.kind AS kind, m.text AS text, m.note_id AS note_id, m.created_at AS created_at "
                      "FROM memory_fts f JOIN memory m ON m.id=f.rowid WHERE memory_fts MATCH ? ORDER BY bm25(memory_fts) LIMIT ?",
                      {fq, limit});
}

bool Store::update_memory(int64_t id, const std::string &text) {
    return run("UPDATE memory SET text=? WHERE id=?", {text, id}) && sqlite3_changes(db_) > 0;
}

bool Store::delete_memory(int64_t id) {
    return run("DELETE FROM memory WHERE id=?", {id}) && sqlite3_changes(db_) > 0;
}

// memories -------------------------------------------------------------------

static const char *kMemoryCols =
    "m.id AS id, m.type AS type, m.text AS text, m.who AS who, m.to_whom AS to_whom, m.due_date AS due_date, "
    "m.due_text AS due_text, m.said_date AS said_date, m.checked AS checked, m.confidence AS confidence, "
    "m.review AS review, m.queue AS queue, m.health AS health, m.done AS done, m.importance AS importance, "
    "m.origin AS origin, m.medicine_id AS medicine_id, m.suggestion_id AS suggestion_id, "
    "m.superseded_by AS superseded_by, m.conflict_with AS conflict_with, m.first_seen AS first_seen, "
    "m.last_seen AS last_seen, m.last_used AS last_used";

// Columns update_memory_item may change; id, origin, first_seen and created_at are fixed at insert.
static const char *kMemoryEditable[] = {"type", "text", "who", "to_whom", "due_date", "due_text", "said_date", "checked",
                                        "confidence", "review", "queue", "health", "done", "importance", "medicine_id",
                                        "suggestion_id", "superseded_by", "conflict_with", "last_seen", "last_used"};

int64_t Store::add_memory_item(const json &row) {
    json text = jget(row, "text");
    if (!text.is_string() || text.get<std::string>().empty()) { err_ = "memory text is required"; return -1; }
    json now = now_j();
    json first = jget(row, "first_seen", now);
    int64_t id = -1;
    bool ok = run("INSERT INTO memories(type,text,who,to_whom,due_date,due_text,said_date,checked,confidence,review,queue,health,"
                  "done,importance,origin,medicine_id,suggestion_id,superseded_by,conflict_with,first_seen,last_seen,last_used,"
                  "created_at) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
                  {jget(row, "type"), text, jget(row, "who"), jget(row, "to_whom"), jget(row, "due_date"), jget(row, "due_text"),
                   jget(row, "said_date"), jtext(jget(row, "checked"), false), jget(row, "confidence", "high"),
                   jget(row, "review", "new"), jget(row, "queue", 0), jget(row, "health", 0), jget(row, "done", 0),
                   jget(row, "importance", 0.0), jget(row, "origin", "recording"), jget(row, "medicine_id"),
                   jget(row, "suggestion_id"), jget(row, "superseded_by"), jget(row, "conflict_with"), first,
                   jget(row, "last_seen", first), jget(row, "last_used"), jget(row, "created_at", now)},
                  &id);
    return ok ? id : -1;
}

bool Store::add_memory_source(int64_t memory_id, int64_t note_id, int64_t start_sec, const std::string &quote,
                              std::optional<double> conf) {
    // The same source twice is not an error; a missing memory or note is (foreign keys).
    return run("INSERT INTO memory_sources(memory_id,note_id,start_sec,quote,conf) VALUES(?,?,?,?,?) "
               "ON CONFLICT(memory_id,note_id,start_sec) DO NOTHING",
               {memory_id, note_id, start_sec, quote, conf ? json(*conf) : json(nullptr)});
}

bool Store::link_memory_entity(int64_t memory_id, int64_t topic_id, const std::string &role) {
    return run("INSERT INTO memory_entities(memory_id,topic_id,role) VALUES(?,?,?) "
               "ON CONFLICT(memory_id,topic_id,role) DO NOTHING", {memory_id, topic_id, role});
}

json Store::memory_full(json row) {
    json id = row["id"];
    row["sources"] = query_rows("SELECT s.note_id AS note_id, s.start_sec AS start_sec, s.quote AS quote, n.title AS title "
                                "FROM memory_sources s JOIN notes n ON n.id=s.note_id WHERE s.memory_id=? "
                                "ORDER BY n.created_at, s.note_id, s.start_sec", {id});
    row["people"] = query_rows("SELECT t.id AS id, t.name AS name, e.role AS role FROM memory_entities e "
                               "JOIN topics t ON t.id=e.topic_id WHERE e.memory_id=? AND t.kind='person' ORDER BY e.rowid", {id});
    if (!row["checked"].is_array()) row["checked"] = json::array();
    return row;
}

json Store::memory_items(const json &filter) {
    std::string sql = std::string("SELECT ") + kMemoryCols + " FROM memories m WHERE 1=1";
    std::vector<json> binds;
    auto in_list = [&](const char *col, const json &vals) {
        if (!vals.is_array() || vals.empty()) return;
        sql += std::string(" AND m.") + col + " IN (";
        for (size_t i = 0; i < vals.size(); i++) {
            sql += i ? ",?" : "?";
            binds.push_back(vals[i]);
        }
        sql += ")";
    };
    json note = jget(filter, "note_id");
    if (note.is_number_integer()) {
        sql += " AND m.id IN (SELECT memory_id FROM memory_sources WHERE note_id=?)";
        binds.push_back(note);
    }
    json types = jget(filter, "types"), reviews = jget(filter, "reviews");
    in_list("type", types);
    in_list("review", reviews);
    json queue = jget(filter, "queue");
    if (queue.is_number_integer() || queue.is_boolean()) {
        sql += " AND m.queue=?";
        binds.push_back(queue);
    }
    bool wants_wrong = jget(filter, "include_wrong", false) == true;
    if (reviews.is_array())
        for (const auto &r : reviews) if (r == "wrong") wants_wrong = true;
    if (!wants_wrong) sql += " AND m.review<>'wrong'";
    if (jget(filter, "include_superseded", false) != true) sql += " AND m.superseded_by IS NULL";
    json limit = jget(filter, "limit");
    sql += " ORDER BY m.last_seen DESC, m.id DESC LIMIT ?";
    binds.push_back(limit.is_number_integer() ? limit : json(-1));
    json out = json::array();
    for (auto &r : query_rows(sql.c_str(), binds)) out.push_back(memory_full(r));
    return out;
}

json Store::memory_items_with_topics(const json &filter) {
    json rows = memory_items(filter);
    std::map<int64_t, json> by_memory;
    for (auto &r : query_rows("SELECT DISTINCT e.memory_id AS memory_id, t.id AS id, t.name AS name FROM memory_entities e "
                              "JOIN topics t ON t.id=e.topic_id WHERE t.kind<>'person' ORDER BY t.id", {}))
        by_memory[r["memory_id"].get<int64_t>()].push_back({{"id", r["id"]}, {"name", r["name"]}});
    for (auto &r : rows) {
        auto it = by_memory.find(r["id"].get<int64_t>());
        r["topics"] = it == by_memory.end() ? json::array() : it->second;
    }
    return rows;
}

json Store::topic_memories(int64_t topic_id, int64_t note_id) {
    std::string sql = std::string("SELECT ") + kMemoryCols + " FROM memories m JOIN memory_entities e ON e.memory_id=m.id "
                      "WHERE e.topic_id=? AND m.queue=0 AND m.superseded_by IS NULL" + memory_map_review_sql();
    std::vector<json> binds = {topic_id};
    if (note_id > 0) {
        sql += " AND m.id IN (SELECT memory_id FROM memory_sources WHERE note_id=?)";
        binds.push_back(note_id);
    }
    sql += " GROUP BY m.id ORDER BY m.importance DESC, m.id DESC";
    json out = json::array();
    for (auto &r : query_rows(sql.c_str(), binds)) out.push_back(memory_full(r));
    return out;
}

json Store::memory_item(int64_t id) {
    json r = query_rows((std::string("SELECT ") + kMemoryCols + " FROM memories m WHERE m.id=?").c_str(), {id});
    if (r.empty()) return nullptr;
    json row = memory_full(r[0]);
    // Older versions: every row whose superseded_by chain ends here, the one it replaced directly first.
    json hist = query_rows((std::string("WITH RECURSIVE older(id,depth) AS (SELECT id,1 FROM memories WHERE superseded_by=?1 "
                                        "UNION SELECT x.id,o.depth+1 FROM memories x JOIN older o ON x.superseded_by=o.id "
                                        "WHERE o.depth<1000) SELECT ") +
                            kMemoryCols + " FROM memories m JOIN older o ON o.id=m.id "
                                          "ORDER BY o.depth, m.first_seen DESC, m.id DESC")
                               .c_str(),
                           {id});
    row["history"] = json::array();
    for (auto &h : hist) row["history"].push_back(memory_full(h));
    return row;
}

bool Store::update_memory_item(int64_t id, const json &fields) {
    if (!fields.is_object()) return false;
    std::string sets;
    std::vector<json> binds;
    for (const char *col : kMemoryEditable) {
        if (!fields.contains(col)) continue;
        sets += std::string(sets.empty() ? "" : ",") + col + "=?";
        binds.push_back(std::strcmp(col, "checked") == 0 ? jtext(fields[col], false) : fields[col]);
    }
    if (sets.empty()) return false;
    binds.push_back(id);
    return run(("UPDATE memories SET " + sets + " WHERE id=?").c_str(), binds) && sqlite3_changes(db_) > 0;
}

bool Store::delete_memory_item(int64_t id) {
    return run("DELETE FROM memories WHERE id=?", {id}) && sqlite3_changes(db_) > 0;
}

json Store::search_memory_items(const std::string &q, int limit) {
    std::string fq = to_fts(q);
    if (fq.empty()) return json::array();
    json rows = query_rows((std::string("SELECT ") + kMemoryCols +
                            " FROM memories_fts f JOIN memories m ON m.id=f.rowid WHERE memories_fts MATCH ? "
                            "AND m.review<>'wrong' AND m.origin<>'medicine' AND m.superseded_by IS NULL AND m.queue=0 ORDER BY bm25(memories_fts) LIMIT ?")
                               .c_str(),
                           {fq, limit});
    json out = json::array();
    for (auto &r : rows) out.push_back(memory_full(r));
    return out;
}

int64_t Store::add_accepted_fact(const json &suggestion, const std::string &text) {
    json note = jget(suggestion, "note_id");
    json said = nullptr;
    if (note.is_number_integer()) {
        json r = query_rows("SELECT date(created_at,'unixepoch','localtime') AS d FROM notes WHERE id=?", {note});
        if (r.empty()) { err_ = "the card's note is gone"; return -1; }
        said = r[0]["d"];
    }
    int64_t id = add_memory_item({{"type", "fact"}, {"text", text}, {"review", "kept"}, {"said_date", said},
                                  {"suggestion_id", jget(suggestion, "id")}});
    if (id <= 0 || !note.is_number_integer()) return id;
    json start = jget(suggestion, "start_sec"), quote = jget(suggestion, "quote");
    if (!add_memory_source(id, note.get<int64_t>(), start.is_number_integer() ? start.get<int64_t>() : 0,
                           quote.is_string() ? quote.get<std::string>() : "", std::nullopt)) {
        std::string e = err_;
        delete_memory_item(id);
        err_ = e;
        return -1;
    }
    return id;
}

json Store::learned_memory(const std::string &kind) {
    json out = memory(kind);
    if (!kind.empty() && kind != "fact") return out;
    json facts = query_rows(
        "SELECT -m.id AS id, 'fact' AS kind, m.text AS text, "
        "(SELECT s.note_id FROM memory_sources s WHERE s.memory_id=m.id ORDER BY s.note_id, s.start_sec LIMIT 1) AS note_id, "
        "m.first_seen AS created_at FROM memories m WHERE m.type='fact' AND m.origin<>'medicine' AND m.review<>'wrong' "
        "AND m.superseded_by IS NULL AND m.queue=0 ORDER BY m.first_seen, m.id", {});
    for (auto &f : facts) out.push_back(f);
    return out;
}

// A negative id is a memory the Learned list shows (same filter as learned_memory); anything else is refused.
bool Store::learned_item(int64_t mid) {
    return !query_rows("SELECT 1 FROM memories WHERE id=? AND type='fact' AND origin<>'medicine' AND review<>'wrong' "
                       "AND superseded_by IS NULL AND queue=0", {mid}).empty();
}

bool Store::update_learned_memory(int64_t id, const std::string &text) {
    if (id < 0) return learned_item(-id) && update_memory_item(-id, {{"text", text}, {"review", "corrected"}});
    return update_memory(id, text);
}

bool Store::delete_learned_memory(int64_t id) {
    if (id < 0) return learned_item(-id) && delete_memory_item(-id);
    return delete_memory(id);
}

bool Store::mark_memory_section(int64_t note_id, int idx) {
    return run("INSERT INTO memory_progress(note_id,section_idx) VALUES(?,?) ON CONFLICT(note_id,section_idx) DO NOTHING",
               {note_id, idx});
}

json Store::memory_sections_done(int64_t note_id) {
    json out = json::array();
    for (const auto &r : query_rows("SELECT section_idx FROM memory_progress WHERE note_id=? ORDER BY section_idx", {note_id}))
        out.push_back(r["section_idx"]);
    return out;
}

json Store::notes_needing_memories(int limit) {
    json out = json::array();
    for (const auto &r : query_rows("SELECT n.id AS id FROM notes n WHERE EXISTS (SELECT 1 FROM sections s WHERE s.note_id=n.id "
                                    "AND NOT EXISTS (SELECT 1 FROM memory_progress p WHERE p.note_id=s.note_id AND p.section_idx=s.idx)) "
                                    "ORDER BY n.created_at DESC, n.id DESC LIMIT ?", {limit}))
        out.push_back(r["id"]);
    return out;
}

// medicines ------------------------------------------------------------------

// "<name> <dose>, <when_text>", leaving out the parts that are empty.
static std::string medicine_memory_text(const json &m) {
    auto str = [&](const char *k) { json v = jget(m, k); return v.is_string() ? v.get<std::string>() : std::string(); };
    std::string t = str("name");
    if (!str("dose").empty()) t += (t.empty() ? "" : " ") + str("dose");
    if (!str("when_text").empty()) t += (t.empty() ? "" : ", ") + str("when_text");
    return t;
}

int64_t Store::upsert_medicine_memory(int64_t medicine_id) {
    json m = query_rows("SELECT id,name,dose,when_text FROM medicines WHERE id=?", {medicine_id});
    if (m.empty()) { err_ = "no such medicine"; return -1; }
    const std::string text = medicine_memory_text(m[0]);
    if (text.empty()) return 0;
    json have = query_rows("SELECT id,text FROM memories WHERE origin='medicine' AND medicine_id=? ORDER BY id LIMIT 1",
                           {medicine_id});
    if (!have.empty()) {
        int64_t id = have[0]["id"].get<int64_t>();
        if (have[0]["text"] != text && !run("UPDATE memories SET text=?, last_seen=? WHERE id=?", {text, now_j(), id})) return -1;
        return id;
    }
    json row = {{"type", "fact"}, {"text", text}, {"origin", "medicine"}, {"medicine_id", medicine_id}, {"health", 1},
                {"review", "kept"}, {"queue", 0}, {"confidence", "high"}, {"checked", json::array()}};
    row["importance"] = memory_importance(row, 0, 0, now_j().get<int64_t>());
    return add_memory_item(row);
}

int64_t Store::add_medicine(const json &m) {
    int64_t id = -1;
    json when = jget(m, "when_text", jget(m, "when"));
    bool ok = run("INSERT INTO medicines(name,dose,when_text,times,repeat,until_date,created_at) VALUES(?,?,?,?,?,?,?)",
                  {jget(m, "name", ""), jget(m, "dose"), when, jtext(jget(m, "times", json::array())),
                   jget(m, "repeat", "daily"), jget(m, "until_date"), now_j()}, &id);
    if (!ok) return -1;
    upsert_medicine_memory(id);  // the medicine is saved either way; a failed memory is repaired at the next open
    return id;
}

json Store::medicines() {
    return query_rows("SELECT id,name,dose,when_text,times,repeat,until_date,created_at FROM medicines ORDER BY id", {});
}

bool Store::update_medicine(int64_t id, const json &m) {
    // Only fields present in `m` change; column names are fixed literals below.
    std::string sets;
    std::vector<json> binds;
    auto add = [&](const char *col, const json &v) {
        if (!sets.empty()) sets += ",";
        sets += std::string(col) + "=?";
        binds.push_back(v);
    };
    for (const char *k : {"name", "dose", "repeat", "until_date"})
        if (m.contains(k)) add(k, m[k]);
    if (m.contains("when_text")) add("when_text", m["when_text"]);
    else if (m.contains("when")) add("when_text", m["when"]);
    if (m.contains("times")) add("times", jtext(m["times"], false));
    if (sets.empty()) return false;
    binds.push_back(id);
    if (!run(("UPDATE medicines SET " + sets + " WHERE id=?").c_str(), binds) || sqlite3_changes(db_) == 0) return false;
    upsert_medicine_memory(id);  // the update is saved either way; a failed memory is repaired at the next open
    return true;
}

bool Store::delete_medicine(int64_t id) {
    return run("DELETE FROM medicines WHERE id=?", {id}) && sqlite3_changes(db_) > 0;
}

// decisions / insights -------------------------------------------------------

bool Store::log_decision(const std::string &kind, const std::string &field, const std::string &from, const std::string &to) {
    return run("INSERT INTO decisions(kind,field,from_value,to_value,created_at) VALUES(?,?,?,?,?)", {kind, field, from, to, now_j()});
}

json Store::decisions(const std::string &kind, const std::string &field, int limit) {
    return query_rows("SELECT id,kind,field,from_value,to_value,created_at FROM decisions "
                      "WHERE (?1='' OR kind=?1) AND (?2='' OR field=?2) ORDER BY id DESC LIMIT ?3", {kind, field, limit});
}

int64_t Store::add_insight(const std::string &text, std::optional<int64_t> note_id) {
    int64_t id = -1;
    bool ok = run("INSERT INTO insights(text,note_id,created_at) VALUES(?,?,?)",
                  {text, note_id ? json(*note_id) : json(nullptr), now_j()}, &id);
    return ok ? id : -1;
}

json Store::latest_insight() {
    json r = query_rows("SELECT id,text,note_id,dismissed,created_at FROM insights WHERE dismissed=0 ORDER BY id DESC LIMIT 1", {});
    return r.empty() ? json(nullptr) : r[0];
}

bool Store::dismiss_insight(int64_t id) {
    return run("UPDATE insights SET dismissed=1 WHERE id=?", {id}) && sqlite3_changes(db_) > 0;
}
// sections -------------------------------------------------------------------

bool Store::set_sections(int64_t note_id, const json &sections) {
    if (!sections.is_array()) { err_ = "sections must be an array"; return false; }
    if (!begin()) return false;
    // Attempts of a section whose piece range changes start again (its outputs are redone below).
    json old = query_rows("SELECT idx,first_piece,last_piece FROM sections WHERE note_id=?", {note_id});
    bool ok = run("DELETE FROM sections WHERE note_id=?", {note_id});
    for (const auto &s : sections) {
        if (!ok) break;
        ok = run("INSERT INTO sections(note_id,idx,first_piece,last_piece,start_sec,end_sec,summary_json) VALUES(?,?,?,?,?,?,?)",
                 {note_id, jget(s, "idx", 0), jget(s, "first_piece", 0), jget(s, "last_piece", 0),
                  jget(s, "start_sec", 0), jget(s, "end_sec", 0),
                  s.contains("summary") ? json(s["summary"].dump()) : json(nullptr)});
    }
    for (const auto &o : old) {
        if (!ok) break;
        if (query_rows("SELECT 1 FROM sections WHERE note_id=? AND idx=? AND first_piece=? AND last_piece=?",
                       {note_id, o["idx"], o["first_piece"], o["last_piece"]}).empty())
            ok = run("DELETE FROM step_attempts WHERE note_id=? AND idx=?", {note_id, o["idx"]});
    }
    // Outputs written for a range that is no longer a section's are redone, never reused.
    ok = ok && run("DELETE FROM section_outputs WHERE note_id=? AND NOT EXISTS (SELECT 1 FROM sections s WHERE "
                   "s.note_id=section_outputs.note_id AND s.idx=section_outputs.idx AND "
                   "s.first_piece=section_outputs.first_piece AND s.last_piece=section_outputs.last_piece)", {note_id});
    ok = ok && run("DELETE FROM step_attempts WHERE note_id=? AND idx NOT IN (SELECT idx FROM sections WHERE note_id=?)",
                   {note_id, note_id});
    if (!ok) { std::string e = err_; exec("ROLLBACK"); err_ = e; return false; }
    return exec("COMMIT");
}

json Store::sections(int64_t note_id) {
    json rows = query_rows("SELECT idx,first_piece,last_piece,start_sec,end_sec,summary_json FROM sections WHERE note_id=? ORDER BY idx",
                           {note_id});
    json out = json::array();
    for (auto &r : rows) {
        json summary = json::object();
        if (r["summary_json"].is_string()) {
            json parsed = json::parse(r["summary_json"].get<std::string>(), nullptr, false);
            if (!parsed.is_discarded()) summary = parsed;
        }
        r.erase("summary_json");
        r["summary"] = summary;
        out.push_back(r);
    }
    return out;
}

bool Store::clear_transcript(int64_t note_id) {
    if (!begin()) return false;
    bool ok = run("DELETE FROM sections WHERE note_id=?", {note_id}) &&
              run("DELETE FROM section_outputs WHERE note_id=?", {note_id}) &&
              run("DELETE FROM step_attempts WHERE note_id=?", {note_id}) &&
              run("DELETE FROM memory_progress WHERE note_id=?", {note_id}) &&
              run("DELETE FROM memory_sources WHERE note_id=?", {note_id});  // single-source memories go too
    ok = ok && set_text_column(note_id, "transcript", nullptr);
    if (!ok) { std::string e = err_; exec("ROLLBACK"); err_ = e; return false; }
    return exec("COMMIT");
}

// section outputs (engine v2) -------------------------------------------------

bool Store::set_section_output(int64_t note_id, int idx, int kind, const std::string &lang, int64_t first_piece,
                               int64_t last_piece, const std::string &text) {
    // Only for the section as it is now: a deleted note, or sections re-planned while the part was written, refuse it.
    if (!run("INSERT INTO section_outputs(note_id,idx,kind,lang,first_piece,last_piece,text,created_at) "
             "SELECT ?,?,?,?,?,?,?,? WHERE EXISTS (SELECT 1 FROM sections WHERE note_id=? AND idx=? AND first_piece=? "
             "AND last_piece=?) ON CONFLICT(note_id,idx,kind) DO UPDATE SET lang=excluded.lang, "
             "first_piece=excluded.first_piece, last_piece=excluded.last_piece, text=excluded.text, "
             "created_at=excluded.created_at",
             {note_id, idx, kind, lang, first_piece, last_piece, text, now_j(), note_id, idx, first_piece, last_piece}))
        return false;
    if (sqlite3_changes(db_) == 0) { err_ = "no such section"; return false; }
    return true;
}

json Store::section_outputs(int64_t note_id) {
    return query_rows("SELECT idx,kind,lang,first_piece,last_piece,text FROM section_outputs WHERE note_id=? "
                      "ORDER BY idx,kind", {note_id});
}

json Store::current_section_outputs(int64_t note_id, int kind, const std::string &lang) {
    json rows = query_rows(
        "SELECT o.text AS text FROM sections s LEFT JOIN section_outputs o ON o.note_id=s.note_id AND o.idx=s.idx "
        "AND o.kind=? AND o.lang=? AND o.first_piece=s.first_piece AND o.last_piece=s.last_piece "
        "WHERE s.note_id=? ORDER BY s.idx", {kind, lang, note_id});
    if (rows.empty()) return nullptr;
    json out = json::array();
    for (const auto &r : rows) {
        if (!r["text"].is_string()) return nullptr;
        out.push_back(r["text"]);
    }
    return out;
}

json Store::section_outputs_state(int64_t note_id, const std::string &lang) {
    json rows = query_rows(
        "SELECT s.idx AS idx, k.kind AS kind, (o.note_id IS NOT NULL) AS done, COALESCE(a.attempts,0) AS attempts "
        "FROM sections s CROSS JOIN (SELECT 1 AS kind UNION ALL SELECT 2) k "
        "LEFT JOIN section_outputs o ON o.note_id=s.note_id AND o.idx=s.idx AND o.kind=k.kind AND o.lang=? "
        "AND o.first_piece=s.first_piece AND o.last_piece=s.last_piece "
        "LEFT JOIN step_attempts a ON a.note_id=s.note_id AND a.idx=s.idx AND a.kind=k.kind "
        "WHERE s.note_id=? ORDER BY s.idx, k.kind", {lang, note_id});
    for (auto &r : rows) r["done"] = r["done"] == true || r["done"] == 1;  // "done" is a flag column already
    return rows;
}

bool Store::bump_step_attempt(int64_t note_id, int idx, int kind) {
    return run("INSERT INTO step_attempts(note_id,idx,kind,attempts) VALUES(?,?,?,1) "
               "ON CONFLICT(note_id,idx,kind) DO UPDATE SET attempts=attempts+1", {note_id, idx, kind});
}

bool Store::undo_step_attempt(int64_t note_id, int idx, int kind) {
    return run("UPDATE step_attempts SET attempts=MAX(attempts-1,0) WHERE note_id=? AND idx=? AND kind=?",
               {note_id, idx, kind}) && sqlite3_changes(db_) > 0;
}

bool Store::set_step_error(int64_t note_id, int idx, int kind, const std::string &error) {
    return run("UPDATE step_attempts SET last_error=? WHERE note_id=? AND idx=? AND kind=?", {error, note_id, idx, kind}) &&
           sqlite3_changes(db_) > 0;
}

json Store::step_attempts(int64_t note_id) {
    return query_rows("SELECT idx,kind,attempts,last_error FROM step_attempts WHERE note_id=? ORDER BY idx,kind", {note_id});
}

bool Store::mark_extracted(int64_t note_id) {
    return setting_set("extracted." + std::to_string(note_id), "1");
}

bool Store::was_extracted(int64_t note_id) {
    if (setting_get("extracted." + std::to_string(note_id))) return true;
    return !query_rows("SELECT 1 FROM suggestions WHERE note_id=? UNION ALL SELECT 1 FROM note_topics WHERE note_id=? LIMIT 1",
                       {note_id, note_id}).empty();
}

// Settings keys the assistant writes (sve_assistant.cpp). Other keys (host and device settings) stay.
static const char *kAssistantSettings[] = {"profile", "insight.last_date"};

bool Store::wipe_assistant() {
    // Children before parents, so no foreign key blocks a delete; the FTS triggers clean the indexes.
    static const char *kTables[] = {"person_questions", "memory_entities", "memory_sources", "memory_progress", "memories",
                                    "suggestions", "reminders", "tasks", "note_topics", "topics", "memory",
                                    "medicines", "decisions", "insights", "section_outputs", "step_attempts", "sections",
                                    "transcript_pieces"};
    if (!begin()) return false;
    bool ok = true;
    for (const char *t : kTables) ok = ok && run((std::string("DELETE FROM ") + t).c_str(), {});
    for (const char *k : kAssistantSettings) ok = ok && run("DELETE FROM settings WHERE key=?", {k});
    ok = ok && run("DELETE FROM settings WHERE key LIKE 'extracted.%'", {});
    if (!ok) { std::string e = err_; exec("ROLLBACK"); err_ = e; return false; }
    return exec("COMMIT");
}

std::string json_escape(const std::string &s) {
    std::string o;
    o.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; }
                else o += (char)c;
        }
    }
    return o;
}

} // namespace sve
