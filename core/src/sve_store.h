// SQLite notes store with FTS5 search. Shared by every host.
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <optional>

struct sqlite3;

namespace sve {

struct NoteRow {
    int64_t id = 0;
    std::string title, lang;
    int64_t created_at = 0;
    int64_t duration_seconds = 0;
    std::optional<std::string> audio_path, transcript, short_summary, detailed_notes, revision_questions;
};

struct NoteListRow {
    int64_t id = 0;
    std::string title, lang, preview;
    int64_t created_at = 0, duration_seconds = 0;
    bool has_audio = false;
};

class Store {
public:
    explicit Store(const std::string &path);
    ~Store();
    bool ok() const { return db_ != nullptr; }
    const std::string &last_error() const { return err_; }

    int64_t create(const std::string &title, const std::string &lang, const char *audio_path,
                   const char *transcript, int64_t duration_seconds);
    bool set_text_column(int64_t id, const char *column, const char *value); // value NULL => SQL NULL
    bool remove(int64_t id);
    std::optional<NoteRow> get(int64_t id);
    std::vector<NoteListRow> list(const std::string &query, int limit);
    std::vector<std::pair<int64_t, std::string>> audio_older_than(int days);

    bool setting_set(const std::string &k, const std::string &v);
    std::optional<std::string> setting_get(const std::string &k);

private:
    bool exec(const char *sql);
    sqlite3 *db_ = nullptr;
    std::string err_;
};

std::string json_escape(const std::string &s);

} // namespace sve
