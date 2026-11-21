#include "sve_store.h"
#include "sve_memory.h"  // memory_importance
#include "sve_embed.h"   // vector_blob
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
    cut INTEGER NOT NULL DEFAULT 0,  -- the reply reached its token cap (headings after the cut are missing)
    PRIMARY KEY(note_id, idx, kind)
);
-- How often a section output was tried in `lang` (a cancel does not count; another lang starts again at 1), how
-- many of those tries failed for a real reason (a kill or a GPU refusal is not one), and why the last try failed.
CREATE TABLE IF NOT EXISTS step_attempts (
    note_id INTEGER NOT NULL REFERENCES notes(id) ON DELETE CASCADE,
    idx INTEGER NOT NULL, kind INTEGER NOT NULL, attempts INTEGER NOT NULL DEFAULT 0, last_error TEXT,
    lang TEXT, failures INTEGER NOT NULL DEFAULT 0,
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
-- Meaning-search vectors (EmbeddingGemma, Task 11): a transcript piece (ref = its implicit rowid; never VACUUM) or a memory (ref = its id),
-- kEmbedDims floats as bytes, and the model that made it. They go with their piece or memory, and when a memory's
-- text changes (it is embedded again).
CREATE TABLE IF NOT EXISTS embeddings (
    kind TEXT NOT NULL CHECK(kind IN ('piece','memory')), ref INTEGER NOT NULL, vec BLOB NOT NULL,
    model TEXT NOT NULL, PRIMARY KEY(kind, ref)
);
CREATE TRIGGER IF NOT EXISTS pieces_ad_embed AFTER DELETE ON transcript_pieces BEGIN
  DELETE FROM embeddings WHERE kind='piece' AND ref=old.rowid;
END;
CREATE TRIGGER IF NOT EXISTS pieces_au_embed AFTER UPDATE OF text ON transcript_pieces BEGIN
  DELETE FROM embeddings WHERE kind='piece' AND ref=old.rowid;
END;
-- Who decided, asked or told what in one recording (derived from its memories, sve_consolidate.h), for the
-- person to check: pending until they accept (with their corrections) or dismiss the note's review; only accepted
-- rows reach the map. Rows go with their note and their memory; a person's topic id moves with merge_topics.
-- relation: decided (person -> decision), asked (person -> question or person), told (person -> person),
-- involved_in (person -> note), about (decision/question -> topic). from_kind 'person' with no topic and no name: nobody said.
CREATE TABLE IF NOT EXISTS relations (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    note_id INTEGER NOT NULL REFERENCES notes(id) ON DELETE CASCADE,
    memory_id INTEGER REFERENCES memories(id) ON DELETE CASCADE,
    relation TEXT NOT NULL CHECK(relation IN ('decided','asked','told','involved_in','about')),
    from_kind TEXT NOT NULL CHECK(from_kind IN ('person','me','decision','question')),
    from_topic INTEGER REFERENCES topics(id) ON DELETE SET NULL, from_name TEXT NOT NULL DEFAULT '',
    to_kind TEXT NOT NULL CHECK(to_kind IN ('person','me','decision','question','topic','note')),
    to_topic INTEGER REFERENCES topics(id) ON DELETE SET NULL, to_text TEXT NOT NULL DEFAULT '',
    quote TEXT, start_sec INTEGER,
    status TEXT NOT NULL DEFAULT 'pending' CHECK(status IN ('pending','accepted','rejected')),
    edited INTEGER NOT NULL DEFAULT 0, edits TEXT, created_at INTEGER NOT NULL, decided_at INTEGER
);
CREATE INDEX IF NOT EXISTS relations_note ON relations(note_id, status);
CREATE TRIGGER IF NOT EXISTS memories_ad_embed AFTER DELETE ON memories BEGIN
  DELETE FROM embeddings WHERE kind='memory' AND ref=old.id;
END;
CREATE TRIGGER IF NOT EXISTS memories_au_embed AFTER UPDATE OF text ON memories BEGIN
  DELETE FROM embeddings WHERE kind='memory' AND ref=old.id;
END;
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
    if (!exec(kSchema)) {
        if ((sqlite3_errcode(db_) & 0xff) != SQLITE_BUSY || !begin()) return false;
        if (!exec(kSchema)) { std::string e = err_; rollback(); err_ = e; return false; }
        if (!commit()) return false;
    }
    return add_missing_columns();
}

// Columns added to tables after they first shipped in a build: section_outputs.cut, step_attempts.lang and
// step_attempts.failures. Writes
// only when one is missing; re-checked inside the IMMEDIATE transaction, as another connection may add it first.
bool Store::add_missing_columns() {
    static const char *kCols[][3] = {{"section_outputs", "cut", "ALTER TABLE section_outputs ADD COLUMN cut INTEGER NOT NULL DEFAULT 0"},
                                     {"step_attempts", "lang", "ALTER TABLE step_attempts ADD COLUMN lang TEXT"},
                                     {"step_attempts", "failures",
                                      "ALTER TABLE step_attempts ADD COLUMN failures INTEGER NOT NULL DEFAULT 0"}};
    auto missing = [&](const char *t, const char *c) {
        return query_rows((std::string("SELECT 1 FROM pragma_table_info('") + t + "') WHERE name=?").c_str(), {c}).empty();
    };
    bool any = false;
    for (const auto &c : kCols) any = any || missing(c[0], c[1]);
    if (!any) return true;
    if (!begin()) return false;
    bool ok = true;
    for (const auto &c : kCols)
        if (ok && missing(c[0], c[1])) ok = exec(c[2]);
    if (!ok) { std::string e = err_; rollback(); err_ = e; return false; }
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
    return n == "done" || n == "active" || n == "from_deleted" || n == "dismissed" || n == "cut";
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

int Store::reclassify_non_people(bool (*is_person)(const std::string &), bool (*is_description)(const std::string &)) {
    int moved = 0;
    for (const auto &t : topics("person")) {
        const int64_t id = t.value("id", (int64_t)0);
        const std::string name = t.value("name", std::string());
        if (id <= 0 || is_person(name)) continue;
        if (!begin()) return -1;
        bool ok = run("DELETE FROM relations WHERE from_topic=? OR to_topic=?", {id, id});
        json same = query_rows("SELECT id FROM topics WHERE kind='topic' AND lower(name)=lower(?) LIMIT 1", {name});
        if (ok && is_description(name)) {
            ok = run("DELETE FROM memory_entities WHERE topic_id=?", {id}) && run("DELETE FROM note_topics WHERE topic_id=?", {id}) &&
                 run("DELETE FROM topics WHERE id=?", {id});
        } else if (ok && !same.empty()) {
            const int64_t into = same[0].value("id", (int64_t)0);
            ok = run("INSERT OR IGNORE INTO note_topics(note_id, topic_id) SELECT note_id, ? FROM note_topics WHERE topic_id=?", {into, id}) &&
                 run("UPDATE OR IGNORE memory_entities SET topic_id=?, role='topic' WHERE topic_id=?", {into, id}) &&
                 run("DELETE FROM memory_entities WHERE topic_id=?", {id}) &&
                 run("DELETE FROM note_topics WHERE topic_id=?", {id}) &&
                 run("DELETE FROM topics WHERE id=?", {id});
        } else if (ok) {
            ok = run("UPDATE topics SET kind='topic' WHERE id=?", {id}) &&
                 run("UPDATE OR IGNORE memory_entities SET role='topic' WHERE topic_id=?", {id}) &&
                 run("DELETE FROM memory_entities WHERE topic_id=? AND role<>'topic'", {id});
        }
        if (!ok) { rollback(); return -1; }
        if (!commit()) return -1;
        moved++;
    }
    return moved;
}

int Store::drop_mentioned_only_people() {
    json ids = query_rows(
        "SELECT t.id AS id FROM topics t WHERE t.kind='person' "
        "AND EXISTS (SELECT 1 FROM memory_entities e WHERE e.topic_id=t.id AND e.role='mentioned') "
        "AND NOT EXISTS (SELECT 1 FROM memory_entities e WHERE e.topic_id=t.id AND e.role<>'mentioned') "
        "AND NOT EXISTS (SELECT 1 FROM relations r WHERE (r.from_topic=t.id OR r.to_topic=t.id) AND r.status='accepted')", {});
    if (ids.empty()) return 0;
    if (!begin()) return -1;
    int n = 0;
    for (const auto &r : ids) {
        const int64_t id = r.value("id", (int64_t)0);
        if (!run("DELETE FROM relations WHERE from_topic=? OR to_topic=?", {id, id}) ||
            !run("DELETE FROM memory_entities WHERE topic_id=?", {id}) || !run("DELETE FROM note_topics WHERE topic_id=?", {id}) ||
            !run("DELETE FROM topics WHERE id=?", {id})) { rollback(); return -1; }
        n++;
    }
    return commit() ? n : -1;
}

int Store::retry_notes_without_memories() {
    json ids = query_rows("SELECT DISTINCT p.note_id AS id FROM memory_progress p "
                          "WHERE NOT EXISTS (SELECT 1 FROM memory_sources s WHERE s.note_id=p.note_id)", {});
    if (ids.empty()) return 0;
    if (!run("DELETE FROM memory_progress WHERE note_id NOT IN (SELECT DISTINCT note_id FROM memory_sources)", {})) return -1;
    return (int)ids.size();
}

json Store::note_topic_links() {
    return query_rows("SELECT note_id, topic_id FROM note_topics ORDER BY note_id, rowid", {});
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
           // Relations name the person by topic: they follow the merge (deleting `from` would only blank them).
           run("UPDATE relations SET from_topic=? WHERE from_topic=?", {into, from}) &&
           run("UPDATE relations SET to_topic=? WHERE to_topic=?", {into, from}) &&
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
                      "WHERE e.topic_id=?" + memory_map_review_sql();
    std::vector<json> binds = {topic_id};
    if (note_id > 0) {
        sql += " AND m.id IN (SELECT memory_id FROM memory_sources WHERE note_id=?)";
        binds.push_back(note_id);
    }
    sql += " GROUP BY m.id ORDER BY m.importance DESC, (m.queue=1 AND m.review='new') ASC, m.id DESC";
    json out = json::array();
    for (auto &r : query_rows(sql.c_str(), binds)) {
        json row = memory_full(r);
        row["waiting"] = memory_waiting(row);
        out.push_back(row);
    }
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
              run("DELETE FROM embeddings WHERE kind='piece' AND ref IN (SELECT rowid FROM transcript_pieces WHERE note_id=?)",
                  {note_id}) &&
              run("DELETE FROM memory_sources WHERE note_id=?", {note_id});  // single-source memories go too
    ok = ok && set_text_column(note_id, "transcript", nullptr);
    if (!ok) { std::string e = err_; exec("ROLLBACK"); err_ = e; return false; }
    return exec("COMMIT");
}

// section outputs (engine v2) -------------------------------------------------

bool Store::set_section_output(int64_t note_id, int idx, int kind, const std::string &lang, int64_t first_piece,
                               int64_t last_piece, const std::string &text, bool cut) {
    // Only for the section as it is now: a deleted note, or sections re-planned while the part was written, refuse it.
    if (!run("INSERT INTO section_outputs(note_id,idx,kind,lang,first_piece,last_piece,text,created_at,cut) "
             "SELECT ?,?,?,?,?,?,?,?,? WHERE EXISTS (SELECT 1 FROM sections WHERE note_id=? AND idx=? AND first_piece=? "
             "AND last_piece=?) ON CONFLICT(note_id,idx,kind) DO UPDATE SET lang=excluded.lang, "
             "first_piece=excluded.first_piece, last_piece=excluded.last_piece, text=excluded.text, "
             "created_at=excluded.created_at, cut=excluded.cut",
             {note_id, idx, kind, lang, first_piece, last_piece, text, now_j(), cut ? 1 : 0, note_id, idx, first_piece,
              last_piece}))
        return false;
    if (sqlite3_changes(db_) == 0) { err_ = "no such section"; return false; }
    return true;
}

int Store::put_section_output(int64_t note_id, int idx, int kind, const std::string &lang, int64_t first_piece,
                              int64_t last_piece, const std::string &text, bool cut) {
    if (set_section_output(note_id, idx, kind, lang, first_piece, last_piece, text, cut)) return 1;
    return err_ == "no such section" ? 0 : -1;
}

json Store::section_outputs(int64_t note_id) {
    return query_rows("SELECT idx,kind,lang,first_piece,last_piece,text,cut FROM section_outputs WHERE note_id=? "
                      "ORDER BY idx,kind", {note_id});
}

json Store::current_section_outputs(int64_t note_id, int kind, const std::string &lang, bool with_gaps) {
    json rows = query_rows(
        "SELECT o.text AS text FROM sections s LEFT JOIN section_outputs o ON o.note_id=s.note_id AND o.idx=s.idx "
        "AND o.kind=? AND o.lang=? AND o.first_piece=s.first_piece AND o.last_piece=s.last_piece "
        "WHERE s.note_id=? ORDER BY s.idx", {kind, lang, note_id});
    if (rows.empty()) return nullptr;
    json out = json::array();
    bool any = false;
    for (const auto &r : rows) {
        if (!r["text"].is_string() && !with_gaps) return nullptr;
        any = any || r["text"].is_string();
        out.push_back(r["text"].is_string() ? r["text"] : json(nullptr));
    }
    return any ? out : json(nullptr);
}

json Store::section_outputs_state(int64_t note_id, const std::string &lang) {
    json rows = query_rows(
        "SELECT s.idx AS idx, k.kind AS kind, (o.note_id IS NOT NULL) AS done, "
        "COALESCE(CASE WHEN a.lang IS ? THEN a.attempts END,0) AS attempts, "
        "CASE WHEN a.lang IS ? THEN a.last_error END AS last_error, "
        "COALESCE(CASE WHEN a.lang IS ? THEN a.failures END,0) AS failures "
        "FROM sections s CROSS JOIN (SELECT 1 AS kind UNION ALL SELECT 2) k "
        "LEFT JOIN section_outputs o ON o.note_id=s.note_id AND o.idx=s.idx AND o.kind=k.kind AND o.lang=? "
        "AND o.first_piece=s.first_piece AND o.last_piece=s.last_piece "
        "LEFT JOIN step_attempts a ON a.note_id=s.note_id AND a.idx=s.idx AND a.kind=k.kind "
        "WHERE s.note_id=? ORDER BY s.idx, k.kind", {lang, lang, lang, lang, note_id});
    for (auto &r : rows) r["done"] = r["done"] == true || r["done"] == 1;  // "done" is a flag column already
    return rows;
}

bool Store::bump_step_attempt(int64_t note_id, int idx, int kind, const std::string &lang) {
    // Tries in another language are not tries of this one: the count and the error start again.
    return run("INSERT INTO step_attempts(note_id,idx,kind,attempts,lang) VALUES(?,?,?,1,?) "
               "ON CONFLICT(note_id,idx,kind) DO UPDATE SET "
               "attempts=CASE WHEN lang IS excluded.lang THEN attempts+1 ELSE 1 END, "
               "last_error=CASE WHEN lang IS excluded.lang THEN last_error ELSE NULL END, "
               "failures=CASE WHEN lang IS excluded.lang THEN failures ELSE 0 END, lang=excluded.lang",
               {note_id, idx, kind, lang});
}

bool Store::undo_step_attempt(int64_t note_id, int idx, int kind) {
    return run("UPDATE step_attempts SET attempts=MAX(attempts-1,0) WHERE note_id=? AND idx=? AND kind=?",
               {note_id, idx, kind}) && sqlite3_changes(db_) > 0;
}

bool Store::set_step_error(int64_t note_id, int idx, int kind, const std::string &error, bool real) {
    return run("UPDATE step_attempts SET last_error=?, failures=failures+? WHERE note_id=? AND idx=? AND kind=?",
               {error, real ? 1 : 0, note_id, idx, kind}) &&
           sqlite3_changes(db_) > 0;
}

json Store::step_attempts(int64_t note_id) {
    return query_rows("SELECT idx,kind,attempts,last_error,lang,failures FROM step_attempts WHERE note_id=? ORDER BY idx,kind",
                      {note_id});
}

int Store::clear_section_outputs(int64_t note_id, int kind, int idx) {
    if (!begin()) return -1;
    int n = 0;
    bool ok = true;
    for (const char *t : {"section_outputs", "step_attempts"}) {
        ok = ok && run((std::string("DELETE FROM ") + t + " WHERE note_id=? AND (?=-1 OR kind=?) AND (?=-1 OR idx=?)").c_str(),
                       {note_id, kind, kind, idx, idx});
        if (ok) n += sqlite3_changes(db_);
    }
    if (!ok) { std::string e = err_; exec("ROLLBACK"); err_ = e; return -1; }
    return exec("COMMIT") ? n : -1;
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
    static const char *kTables[] = {"relations", "embeddings", "person_questions", "memory_entities", "memory_sources", "memory_progress", "memories",
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

// meaning-search vectors ---------------------------------------------------

// Memories that may reach a prompt (as search_memory_items): not wrong, superseded, queued or a medicine's.
static const char *kCurrentMemory = "m.review<>'wrong' AND m.origin<>'medicine' AND m.superseded_by IS NULL AND m.queue=0";

bool Store::set_embedding(const std::string &kind, int64_t ref, const std::vector<float> &vec, const std::string &model,
                          const std::string &text) {
    if (vec.empty() || (kind != "piece" && kind != "memory")) { err_ = "bad embedding"; return false; }
    // Only for a piece or memory that still exists with the text that was embedded (it may be deleted, edited or
    // written again under the same rowid while its vector is made).
    const std::string sql = std::string("INSERT OR REPLACE INTO embeddings(kind,ref,vec,model) SELECT ?,?,?,? WHERE EXISTS (") +
                            (kind == "piece" ? "SELECT 1 FROM transcript_pieces WHERE rowid=? AND text=?"
                                               : "SELECT 1 FROM memories WHERE id=? AND text=?") + ")";
    sqlite3_stmt *st = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) { err_ = sqlite3_errmsg(db_); return false; }
    const std::string blob = vector_blob(vec);
    sqlite3_bind_text(st, 1, kind.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 2, ref);
    sqlite3_bind_blob(st, 3, blob.data(), (int)blob.size(), SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 4, model.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 5, ref);
    sqlite3_bind_text(st, 6, text.c_str(), (int)text.size(), SQLITE_TRANSIENT);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) { err_ = sqlite3_errmsg(db_); return false; }
    return sqlite3_changes(db_) > 0;
}

std::vector<std::pair<int64_t, std::vector<float>>> Store::embeddings(const std::string &kind, const std::string &model) {
    std::vector<std::pair<int64_t, std::vector<float>>> out;
    const char *sql = kind == "memory"
        ? "SELECT e.ref, e.vec FROM embeddings e JOIN memories m ON m.id=e.ref WHERE e.kind='memory' AND e.model=? AND "
          "m.review<>'wrong' AND m.origin<>'medicine' AND m.superseded_by IS NULL AND m.queue=0 ORDER BY e.ref"
        : "SELECT e.ref, e.vec FROM embeddings e JOIN transcript_pieces p ON p.rowid=e.ref WHERE e.kind='piece' AND e.model=? "
          "ORDER BY e.ref";
    if (kind != "piece" && kind != "memory") return out;
    sqlite3_stmt *st = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) { err_ = sqlite3_errmsg(db_); return out; }
    sqlite3_bind_text(st, 1, model.c_str(), -1, SQLITE_TRANSIENT);
    while (sqlite3_step(st) == SQLITE_ROW) {
        const void *b = sqlite3_column_blob(st, 1);
        int n = sqlite3_column_bytes(st, 1);
        std::vector<float> v = blob_vector(b && n > 0 ? std::string((const char *)b, (size_t)n) : std::string());
        if (!v.empty()) out.push_back({sqlite3_column_int64(st, 0), std::move(v)});
    }
    sqlite3_finalize(st);
    return out;
}

int64_t Store::embedding_count(const std::string &model) {
    json r = query_rows("SELECT COUNT(*) AS n FROM embeddings WHERE model=?", {model});
    return r.empty() ? 0 : r[0].value("n", (int64_t)0);
}

json Store::embed_pending(const std::string &model, int limit) {
    json out = json::array();
    if (limit <= 0) return out;
    // Pieces of notes that still have their transcript, newest notes first; a cleared transcript is not embedded again.
    json pieces = query_rows(
        "SELECT p.rowid AS ref, p.note_id AS note_id, p.idx AS idx, p.text AS text FROM transcript_pieces p "
        "JOIN notes n ON n.id=p.note_id WHERE n.transcript IS NOT NULL AND trim(p.text)<>'' AND NOT EXISTS "
        "(SELECT 1 FROM embeddings e WHERE e.kind='piece' AND e.ref=p.rowid AND e.model=?) "
        "ORDER BY n.created_at DESC, n.id DESC, p.idx ASC LIMIT ?", {model, limit});
    for (auto &r : pieces) { r["kind"] = "piece"; out.push_back(std::move(r)); }
    int left = limit - (int)out.size();
    if (left <= 0) return out;
    json mems = query_rows((std::string("SELECT m.id AS ref, m.text AS text FROM memories m WHERE ") + kCurrentMemory +
                            " AND trim(m.text)<>'' AND NOT EXISTS (SELECT 1 FROM embeddings e WHERE e.kind='memory' AND "
                            "e.ref=m.id AND e.model=?) ORDER BY m.last_seen DESC, m.id DESC LIMIT ?").c_str(),
                           {model, left});
    for (auto &r : mems) { r["kind"] = "memory"; out.push_back(std::move(r)); }
    return out;
}

json Store::search_piece_refs(const std::string &q, int limit) {
    std::string fq = to_fts(q);
    if (fq.empty()) return json::array();
    return query_rows(
        "SELECT p.rowid AS ref, p.note_id AS note_id, p.start_sec AS start_sec FROM pieces_fts f "
        "JOIN transcript_pieces p ON p.rowid=f.rowid WHERE pieces_fts MATCH ? ORDER BY bm25(pieces_fts) LIMIT ?", {fq, limit});
}

json Store::piece_refs(const std::vector<int64_t> &refs) {
    json out = json::array();
    for (int64_t r : refs) {
        json row = query_rows("SELECT rowid AS ref, note_id, start_sec FROM transcript_pieces WHERE rowid=?", {r});
        if (!row.empty()) out.push_back(row[0]);
    }
    return out;
}

json Store::search_memory_refs(const std::string &q, int limit) {
    json out = json::array();
    std::string fq = to_fts(q);
    if (fq.empty()) return out;
    for (const auto &r : query_rows((std::string("SELECT m.id AS id FROM memories_fts f JOIN memories m ON m.id=f.rowid "
                                                 "WHERE memories_fts MATCH ? AND ") + kCurrentMemory +
                                     " ORDER BY bm25(memories_fts) LIMIT ?").c_str(), {fq, limit}))
        out.push_back(r["id"]);
    return out;
}

// relations (who decided, asked or told what; sve_consolidate.h) ---------------

static const char *kRelationCols =
    "r.id AS id, r.note_id AS note_id, r.memory_id AS memory_id, r.relation AS relation, r.from_kind AS from_kind, "
    "r.from_topic AS from_topic, COALESCE(ft.name, r.from_name) AS from_name, r.to_kind AS to_kind, r.to_topic AS to_topic, "
    "CASE WHEN r.to_kind IN ('person','topic') THEN COALESCE(tt.name, r.to_text) ELSE r.to_text END AS to_text, "
    "r.quote AS quote, r.start_sec AS start_sec, r.status AS status, r.edited AS edited, r.edits AS edits, "
    "r.created_at AS created_at, r.decided_at AS decided_at";

int64_t Store::add_relation(const json &r) {
    int64_t id = -1;
    bool ok = run("INSERT INTO relations(note_id,memory_id,relation,from_kind,from_topic,from_name,to_kind,to_topic,to_text,"
                  "quote,start_sec,status,created_at) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?)",
                  {jget(r, "note_id"), jget(r, "memory_id"), jget(r, "relation"), jget(r, "from_kind"), jget(r, "from_topic"),
                   jget(r, "from_name", ""), jget(r, "to_kind"), jget(r, "to_topic"), jget(r, "to_text", ""), jget(r, "quote"),
                   jget(r, "start_sec"), jget(r, "status", "pending"), now_j()},
                  &id);
    return ok ? id : -1;
}

json Store::relations(const json &filter) {
    std::string sql = std::string("SELECT ") + kRelationCols +
                      " FROM relations r LEFT JOIN topics ft ON ft.id=r.from_topic LEFT JOIN topics tt ON tt.id=r.to_topic WHERE 1=1";
    std::vector<json> binds;
    json note = jget(filter, "note_id"), status = jget(filter, "status");
    if (note.is_number_integer()) { sql += " AND r.note_id=?"; binds.push_back(note); }
    if (status.is_string()) { sql += " AND r.status=?"; binds.push_back(status); }
    sql += " ORDER BY r.note_id, r.id";
    json out = query_rows(sql.c_str(), binds);
    for (auto &r : out) {
        r["edited"] = r["edited"].is_number_integer() && r["edited"].get<int64_t>() != 0;
        if (r["edits"].is_string()) {
            json e = json::parse(r["edits"].get<std::string>(), nullptr, false);
            r["edits"] = e.is_discarded() ? json(nullptr) : e;
        }
    }
    return out;
}

json Store::relation_review_notes() {
    return query_rows("SELECT n.id AS note_id, n.title AS title, n.created_at AS created_at FROM notes n WHERE EXISTS "
                      "(SELECT 1 FROM relations r WHERE r.note_id=n.id AND r.status='pending') ORDER BY n.created_at DESC, n.id DESC", {});
}

bool Store::update_relation(int64_t id, const json &fields) {
    static const char *kEditable[] = {"status", "from_kind", "from_topic", "from_name", "to_kind", "to_topic", "to_text",
                                      "edited", "edits", "decided_at"};
    if (!fields.is_object()) return false;
    std::string sets;
    std::vector<json> binds;
    for (const char *col : kEditable) {
        if (!fields.contains(col)) continue;
        sets += std::string(sets.empty() ? "" : ",") + col + "=?";
        binds.push_back(std::strcmp(col, "edits") == 0 && !fields[col].is_null() ? json(fields[col].dump()) : fields[col]);
    }
    if (sets.empty()) return false;
    binds.push_back(id);
    return run(("UPDATE relations SET " + sets + " WHERE id=?").c_str(), binds) && sqlite3_changes(db_) > 0;
}

int Store::delete_relations(int64_t note_id, const std::string &status) {
    if (!run("DELETE FROM relations WHERE note_id=? AND status=?", {note_id, status})) return -1;
    return sqlite3_changes(db_);
}

bool Store::move_note_person(int64_t note_id, int64_t from_topic, int64_t into_topic, const std::string &old_name,
                             const std::string &new_name) {
    if (from_topic <= 0 || from_topic == into_topic) return true;
    const char *mine = "(SELECT memory_id FROM memory_sources WHERE note_id=?)";
    bool ok = true;
    if (into_topic > 0)
        ok = run((std::string("INSERT OR IGNORE INTO memory_entities(memory_id,topic_id,role) SELECT memory_id,?,role FROM "
                              "memory_entities WHERE topic_id=? AND memory_id IN ") + mine).c_str(),
                 {into_topic, from_topic, note_id}) &&
             link_topic(note_id, into_topic);
    ok = ok && run((std::string("DELETE FROM memory_entities WHERE topic_id=? AND memory_id IN ") + mine).c_str(),
                   {from_topic, note_id}) &&
         run("DELETE FROM note_topics WHERE note_id=? AND topic_id=?", {note_id, from_topic});
    // The memories' own words for who and to whom follow the name the person gave.
    for (const char *col : {"who", "to_whom"})
        ok = ok && run((std::string("UPDATE memories SET ") + col + "=? WHERE lower(trim(" + col + "))=lower(trim(?)) AND id IN " +
                        mine).c_str(),
                       {new_name.empty() ? json(nullptr) : json(new_name), old_name, note_id});
    const json into = into_topic > 0 ? json(into_topic) : json(nullptr);
    const char *kind = into_topic > 0 ? "person" : "me";
    ok = ok && run("UPDATE relations SET from_topic=?, from_kind=?, from_name=? WHERE note_id=? AND from_topic=?",
                   {into, kind, new_name, note_id, from_topic}) &&
         run("UPDATE relations SET to_topic=?, to_kind=?, to_text=? WHERE note_id=? AND to_topic=?",
             {into, kind, new_name, note_id, from_topic});
    // A name nobody else uses any more (a mishearing) goes; one other recordings still use stays.
    return ok && run("DELETE FROM topics WHERE id=? AND kind='person' AND NOT EXISTS (SELECT 1 FROM note_topics WHERE topic_id=?) "
                     "AND NOT EXISTS (SELECT 1 FROM memory_entities WHERE topic_id=?)", {from_topic, from_topic, from_topic});
}

bool Store::set_memory_who(int64_t memory_id, int64_t topic_id, const std::string &who) {
    return run("UPDATE memories SET who=? WHERE id=?", {who.empty() ? json(nullptr) : json(who), memory_id}) &&
           run("DELETE FROM memory_entities WHERE memory_id=? AND role='who'", {memory_id}) &&
           (topic_id <= 0 || link_memory_entity(memory_id, topic_id, "who"));
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
