#include "sve_store.h"
#include <sqlite3.h>
#include <ctime>
#include <cstring>

namespace sve {

static const char *kSchema = R"SQL(
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
)SQL";

Store::Store(const std::string &path) {
    if (sqlite3_open(path.c_str(), &db_) != SQLITE_OK) {
        err_ = db_ ? sqlite3_errmsg(db_) : "sqlite3_open failed";
        if (db_) { sqlite3_close(db_); db_ = nullptr; }
        return;
    }
    if (!exec(kSchema)) { sqlite3_close(db_); db_ = nullptr; }
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

// Turn free text into a tolerant FTS5 query: each word becomes a prefix term, ANDed.
static std::string fts_query(const std::string &q) {
    std::string out, word;
    auto flush = [&]() {
        if (word.empty()) return;
        if (!out.empty()) out += " ";
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
    return n == "times" || n == "check_fields" || n == "payload";
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

static std::string to_fts(const std::string &q) { return fts_query(q); }

// pieces ---------------------------------------------------------------------

bool Store::set_pieces(int64_t note_id, const json &pieces) {
    if (!pieces.is_array()) { err_ = "pieces must be an array"; return false; }
    if (!exec("BEGIN")) return false;
    bool ok = run("DELETE FROM transcript_pieces WHERE note_id=?", {note_id});
    int idx = 0;
    for (const auto &p : pieces) {
        if (!ok) break;
        ok = run("INSERT INTO transcript_pieces(note_id,idx,start_sec,text) VALUES(?,?,?,?)",
                 {note_id, idx++, jget(p, "start_sec", 0), jget(p, "text", "")});
    }
    if (!ok) { std::string e = err_; exec("ROLLBACK"); err_ = e; return false; }
    return exec("COMMIT");
}

json Store::pieces(int64_t note_id) {
    return query_rows("SELECT idx,start_sec,text FROM transcript_pieces WHERE note_id=? ORDER BY idx", {note_id});
}

json Store::search_pieces(const std::string &q, int limit) {
    std::string fq = to_fts(q);
    if (fq.empty()) return json::array();
    return query_rows(
        "SELECT p.note_id AS note_id, n.title AS title, p.start_sec AS start_sec, p.text AS text "
        "FROM pieces_fts f JOIN transcript_pieces p ON p.rowid=f.rowid JOIN notes n ON n.id=p.note_id "
        "WHERE pieces_fts MATCH ? ORDER BY bm25(pieces_fts) LIMIT ?", {fq, limit});
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
    return run("UPDATE suggestions SET status=? WHERE id=?", {status, id}) && sqlite3_changes(db_) > 0;
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

bool Store::set_topic_summary(int64_t id, const std::string &summary) {
    return run("UPDATE topics SET summary=?, summary_stale=0 WHERE id=?", {summary, id}) && sqlite3_changes(db_) > 0;
}

bool Store::rename_topic(int64_t id, const std::string &name) {
    return run("UPDATE topics SET name=? WHERE id=?", {name, id}) && sqlite3_changes(db_) > 0;
}

bool Store::merge_topics(int64_t from, int64_t into) {
    if (from == into) return false;
    if (query_rows("SELECT id FROM topics WHERE id=?", {from}).empty() ||
        query_rows("SELECT id FROM topics WHERE id=?", {into}).empty()) return false;
    if (!exec("BEGIN")) return false;
    bool ok = run("INSERT OR IGNORE INTO note_topics(note_id,topic_id) SELECT note_id,? FROM note_topics WHERE topic_id=?", {into, from}) &&
              run("DELETE FROM topics WHERE id=?", {from}) &&
              run("UPDATE topics SET summary_stale=1 WHERE id=?", {into});
    if (!ok) { std::string e = err_; exec("ROLLBACK"); err_ = e; return false; }
    return exec("COMMIT");
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

// medicines ------------------------------------------------------------------

int64_t Store::add_medicine(const json &m) {
    int64_t id = -1;
    json when = jget(m, "when_text", jget(m, "when"));
    bool ok = run("INSERT INTO medicines(name,dose,when_text,times,repeat,until_date,created_at) VALUES(?,?,?,?,?,?,?)",
                  {jget(m, "name", ""), jget(m, "dose"), when, jtext(jget(m, "times", json::array())),
                   jget(m, "repeat", "daily"), jget(m, "until_date"), now_j()}, &id);
    return ok ? id : -1;
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
    return run(("UPDATE medicines SET " + sets + " WHERE id=?").c_str(), binds) && sqlite3_changes(db_) > 0;
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
