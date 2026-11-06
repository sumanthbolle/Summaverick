// SQLite notes store with FTS5 search. Shared by every host.
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <optional>
#include "third_party/nlohmann/json.hpp"

struct sqlite3;

namespace sve {

using json = nlohmann::json;

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

    // --- assistant data (JSON in/out; see schema in sve_store.cpp) ---
    bool set_pieces(int64_t note_id, const json &pieces);
    json pieces(int64_t note_id);
    json search_pieces(const std::string &q, int limit);

    int64_t add_suggestion(const json &s);
    json suggestions(const std::string &status);
    bool set_suggestion_status(int64_t id, const std::string &status);
    json suggestion(int64_t id);

    int64_t add_reminder(const json &r);
    json reminders(bool active_only);
    bool set_reminder_active(int64_t id, bool active);

    int64_t add_task(const json &t);
    json tasks(bool open_only);
    bool set_task_done(int64_t id, bool done);

    int64_t upsert_topic(const std::string &kind, const std::string &name);
    bool link_topic(int64_t note_id, int64_t topic_id);
    json topics(const std::string &kind);
    json topic_notes(int64_t topic_id);
    bool set_topic_summary(int64_t id, const std::string &summary);
    bool rename_topic(int64_t id, const std::string &name);
    bool merge_topics(int64_t from, int64_t into);

    int64_t add_memory(const std::string &kind, const std::string &text, std::optional<int64_t> note_id);
    json memory(const std::string &kind_or_empty);
    json search_memory(const std::string &q, int limit);
    bool update_memory(int64_t id, const std::string &text);
    bool delete_memory(int64_t id);

    int64_t add_medicine(const json &m);
    json medicines();
    bool update_medicine(int64_t id, const json &m);
    bool delete_medicine(int64_t id);

    bool log_decision(const std::string &kind, const std::string &field, const std::string &from, const std::string &to);
    json decisions(const std::string &kind, const std::string &field, int limit);

    int64_t add_insight(const std::string &text, std::optional<int64_t> note_id);
    json latest_insight();
    bool dismiss_insight(int64_t id);

private:
    bool exec(const char *sql);
    json query_rows(const char *sql, const std::vector<json> &binds);
    bool run(const char *sql, const std::vector<json> &binds, int64_t *last_id = nullptr);
    sqlite3 *db_ = nullptr;
    std::string err_;
};

std::string json_escape(const std::string &s);

} // namespace sve
