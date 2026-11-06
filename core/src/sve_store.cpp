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
