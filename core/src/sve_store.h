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
    // How long a connection waits for another connection's write before a statement fails with SQLITE_BUSY.
    static constexpr int kBusyTimeoutMs = 5000;
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
    // Ask: {id,title,created_at,short_summary,revision_questions} of notes created in [from, to) (epoch seconds; to 0 =
    // all notes), newest first unless oldest_first, at most limit. Never reads the transcripts.
    json note_summaries(int64_t from, int64_t to, int limit, bool oldest_first = false);
    std::vector<std::pair<int64_t, std::string>> audio_older_than(int days);

    // Explicit transaction for multi-row changes (not nested; set_pieces/merge_topics use their own).
    // IMMEDIATE: the write lock is taken at BEGIN, where the busy timeout applies. A deferred transaction that
    // reads and then writes gets SQLITE_BUSY at once (no wait) when another connection wrote in between.
    bool begin() { return exec("BEGIN IMMEDIATE"); }
    bool commit() { return exec("COMMIT"); }
    bool rollback() { return exec("ROLLBACK"); }

    bool setting_set(const std::string &k, const std::string &v);
    std::optional<std::string> setting_get(const std::string &k);

    // --- assistant data (JSON in/out; see schema in sve_store.cpp) ---
    bool set_pieces(int64_t note_id, const json &pieces);
    json pieces(int64_t note_id);
    json search_pieces(const std::string &q, int limit);
    // Only pieces of notes created in [from, to).
    json search_pieces_between(const std::string &q, int64_t from, int64_t to, int limit);

    // Section summaries: [{idx,first_piece,last_piece,start_sec,end_sec,summary:{...}}]. set replaces all
    // (fails for a missing note, so a deleted note is never resurrected). clear_transcript also drops sections.
    bool set_sections(int64_t note_id, const json &sections);
    json sections(int64_t note_id);
    bool clear_transcript(int64_t note_id);

    int64_t add_suggestion(const json &s);
    json suggestions(const std::string &status);
    bool set_suggestion_status(int64_t id, const std::string &status);
    json suggestion(int64_t id);
    bool set_suggestion_check(int64_t id, const std::string &check_status, const json &check_fields);

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
    json note_topics(int64_t note_id, const std::string &kind);  // [{id,kind,name}] in the order they were linked
    bool set_topic_summary(int64_t id, const std::string &summary);
    bool rename_topic(int64_t id, const std::string &name);
    bool merge_topics(int64_t from, int64_t into);

    // Old learned-memory table: preferences and vocabulary (facts moved to `memories` at migration).
    int64_t add_memory(const std::string &kind, const std::string &text, std::optional<int64_t> note_id);
    json memory(const std::string &kind_or_empty);
    json search_memory(const std::string &q, int limit);
    bool update_memory(int64_t id, const std::string &text);
    bool delete_memory(int64_t id);

    // Memory layer (`memories` and its sources, people/topic links and per-section progress).
    // Row JSON: {id,type,text,who,to_whom,due_date,due_text,said_date,checked:[..],confidence,review,
    // queue,health,done,importance,origin,medicine_id,suggestion_id,superseded_by,conflict_with,
    // first_seen,last_seen,sources:[{note_id,start_sec,quote,title}],people:[{id,name,role}]}.
    // A recording/chat/migrated memory is deleted with its last source (note delete, clear_transcript).
    int64_t add_memory_item(const json &row);
    bool add_memory_source(int64_t memory_id, int64_t note_id, int64_t start_sec, const std::string &quote,
                           std::optional<double> conf);
    bool link_memory_entity(int64_t memory_id, int64_t topic_id, const std::string &role);
    // filter: note_id, types [], reviews [], queue 0/1, include_wrong (false), include_superseded (false),
    // limit. Newest first. Listing "wrong" in reviews includes wrong rows.
    json memory_items(const json &filter);
    // memory_items plus "topics":[{id,name}] (non-person topics the memory is linked to), for the map.
    json memory_items_with_topics(const json &filter);
    // Memories linked to a person or topic (any role) that the map may show (memory_on_map), by importance.
    json topic_memories(int64_t topic_id, int64_t note_id = 0);  // same eligibility as the map; note_id 0 = all
    json memory_item(int64_t id);  // null when missing; adds "history" (rows it superseded, newest first)
    bool update_memory_item(int64_t id, const json &fields);  // whitelisted columns only
    bool delete_memory_item(int64_t id);
    // Rows that may reach a prompt: not wrong, not superseded, not waiting in the review queue.
    json search_memory_items(const std::string &q, int limit);
    bool mark_memory_section(int64_t note_id, int idx);
    json memory_sections_done(int64_t note_id);  // [idx,…]
    json notes_needing_memories(int limit);      // [note id,…]: a section without memories done, newest first
    // An accepted fact card becomes a kept fact memory whose source is the card's note, start_sec and
    // quote (said_date = the note's local date). No transaction of its own. -1 on failure.
    int64_t add_accepted_fact(const json &suggestion, const std::string &text);
    // The Learned list, in the old row shape {id,kind,text,note_id,created_at}: preferences and vocab
    // from `memory`, plus current fact memories (not medicine, wrong, superseded or queued) with
    // id = -(memories.id). update/delete take those ids; an edited memory becomes review 'corrected'.
    json learned_memory(const std::string &kind_or_empty);
    bool learned_item(int64_t memory_id);
    bool update_learned_memory(int64_t id, const std::string &text);
    bool delete_learned_memory(int64_t id);

    // add/update also keep the medicine's memory in step (upsert_medicine_memory); delete cascades to it.
    int64_t add_medicine(const json &m);
    json medicines();
    bool update_medicine(int64_t id, const json &m);
    bool delete_medicine(int64_t id);
    // The one memory of a saved medicine: origin 'medicine', type 'fact', health 1, review 'kept', text
    // "<name> <dose>, <when_text>" from the row. Inserted, or its text updated; the id, 0 when the row has no
    // text, -1 on error. Run for every medicine at open, so medicines saved before this existed get one.
    int64_t upsert_medicine_memory(int64_t medicine_id);

    bool log_decision(const std::string &kind, const std::string &field, const std::string &from, const std::string &to);
    json decisions(const std::string &kind, const std::string &field, int limit);

    int64_t add_insight(const std::string &text, std::optional<int64_t> note_id);
    json latest_insight();
    bool dismiss_insight(int64_t id);

    // Per-note "extraction ran" flag: settings key "extracted.<id>", deleted with the note (trigger)
    // and by wipe_assistant. was_extracted is also true for a note that already has suggestions or
    // topic links (extracted before the flag existed).
    bool mark_extracted(int64_t note_id);
    bool was_extracted(int64_t note_id);

    // Deletes everything the assistant stored, in one transaction: profile and other assistant settings,
    // medicines, memory, memories (with sources, links, progress), reminders, tasks, topics and links,
    // suggestions, decisions, insights, transcript pieces. Notes and other settings are kept (the host deletes notes). Not nested in begin().
    bool wipe_assistant();

private:
    // PRAGMA user_version upgrades, run once per version at open (0 -> 1: pieces conf, facts to memories;
    // 1 -> 2: facts written to the old table at version 1 move too).
    bool set_wal();
    bool create_schema();
    bool migrate();
    bool move_old_facts();
    void sync_medicine_memories();
    json memory_full(json row);  // adds sources and people to a memories row
    bool exec(const char *sql);
    json query_rows(const char *sql, const std::vector<json> &binds);
    bool run(const char *sql, const std::vector<json> &binds, int64_t *last_id = nullptr);
    sqlite3 *db_ = nullptr;
    std::string err_;
};

std::string json_escape(const std::string &s);

} // namespace sve
