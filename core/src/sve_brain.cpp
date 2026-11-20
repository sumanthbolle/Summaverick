// Memory C API: memories from section summaries, the review queue and the person's own changes.
// The rules that decide what may become a memory live in sve_memory.cpp (pure, unit-tested); this file
// runs the model calls they need and writes the results. Nothing here throws out of a C function.
#include "sve_core.h"
#include "sve_internal.h"
#include "sve_context.h"
#include "sve_embed.h"
#include "sve_extract.h"
#include "sve_medcheck.h"
#include "sve_brainmap.h"
#include "sve_consolidate.h"
#include "sve_memory.h"
#include "sve_prompts.h"
#include "sve_sections.h"  // memory_prompt, complete_memory_items
#include "sve_suggest.h"
#include "sve_textmatch.h"

#include <algorithm>
#include <ctime>
#include <map>
#include <optional>
#include <tuple>

using sve::json;
using sve::jstr;
using sve::jint;
using sve::jarr;
using sve::jobj;

void (*g_sve_memory_drop)(int64_t, int, const char *, const char *) = nullptr;
const char *(*g_sve_model_reply)(const char *, const char *) = nullptr;

namespace {

constexpr int kMemoryMaxOut = 900;  // the memory-only call
constexpr int kYesNoMaxOut = 3;     // the double-check

const char *kCheckSystem =
    "You check whether a quote from a recording says what a memory claims. Answer only yes or no.";

std::string lower(std::string s) {
    for (auto &c : s) if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
    return s;
}

// The local date a recording was made (a recording just before midnight keeps its own day).
std::string local_ymd(int64_t t) {
    time_t tt = (time_t)t;
    struct tm lt;
    localtime_r(&tt, &lt);
    char b[16];
    strftime(b, sizeof b, "%Y-%m-%d", &lt);
    return b;
}

void dropped(int64_t note_id, int idx, const std::string &reason, const json &cand) {
    if (g_sve_memory_drop)
        g_sve_memory_drop(note_id, idx, reason.c_str(), cand.dump(-1, ' ', false, json::error_handler_t::replace).c_str());
}

// Only a cancel from the host ends a call; other failures are handled by the caller.
struct Watch {
    sve_progress_cb cb;
    void *user;
    bool cancelled;
};
int watch_cb(const char *phase, int done, int total, void *u) {
    Watch *w = static_cast<Watch *>(u);
    if (w->cb && w->cb(phase, done, total, w->user)) w->cancelled = true;
    return w->cancelled ? 1 : 0;
}

bool fits(sve_ctx *ctx, const std::string &system, const std::string &user_prompt, int max_out) {
    if (g_sve_model_reply || !ctx->llm) return true;
    sve::Llm &llm = *ctx->llm;
    return llm.count_tokens(llm.apply_template(system, user_prompt)) + max_out + 16 <= llm.n_ctx;
}

// One greedy model call (or the test stand-in). False on failure or cancel (w.cancelled tells which).
bool model_reply(sve_ctx *ctx, const char *phase, const std::string &system, const std::string &user_prompt, int max_out,
                 int done, int total, std::string &out, Watch &w) {
    if (g_sve_model_reply) {
        const char *r = g_sve_model_reply(phase, user_prompt.c_str());
        if (!r) return false;
        out = r;
        return true;
    }
    if (!ctx->llm) return false;
    sve::Llm &llm = *ctx->llm;
    return llm.complete(llm.apply_template(system, user_prompt), max_out, out, watch_cb, &w, phase, done, total, true);
}

std::string self_name(sve::Store &st) {
    auto p = st.setting_get("profile");
    json j = p ? json::parse(*p, nullptr, false) : json();
    return j.is_object() ? lower(jstr(j, "name")) : "";
}

// A pending reminder or task card of the same note and moment whose quote and the memory's share their
// words: the shorter quote's tokens are a run in the longer's. 0 for none.
int64_t matching_suggestion(sve::Store &st, int64_t note_id, int64_t start_sec, const std::string &quote) {
    const std::vector<std::string> mine = sve::match_tokens(quote);
    if (mine.empty()) return 0;
    for (const auto &s : st.suggestions("pending")) {
        const std::string kind = jstr(s, "kind");
        if ((kind != "reminder" && kind != "task") || jint(s, "note_id", -1) != note_id || jint(s, "start_sec", -1) != start_sec)
            continue;
        const std::vector<std::string> theirs = sve::match_tokens(jstr(s, "quote"));
        if (theirs.empty()) continue;
        const bool ok = mine.size() <= theirs.size() ? sve::contains_run(theirs, mine) : sve::contains_run(mine, theirs);
        if (ok) return jint(s, "id", 0);
    }
    return 0;
}

// memory_importance with the most notes any linked person appears in (practice counts come with recall).
double importance_now(sve::Store &st, const json &row) {
    std::map<int64_t, int64_t> notes_of;
    for (const auto &t : st.topics("person")) notes_of[jint(t, "id", 0)] = jint(t, "note_count", 0);
    int met = 0;
    for (const auto &p : jarr(row, "people")) met = std::max<int>(met, (int)notes_of[jint(p, "id", 0)]);
    return sve::memory_importance(row, met, 0, (int64_t)time(nullptr));
}

bool refresh_importance(sve::Store &st, int64_t id) {
    json row = st.memory_item(id);
    return row.is_object() && st.update_memory_item(id, {{"importance", importance_now(st, row)}});
}

json null_if_empty(const std::string &s) { return s.empty() ? json(nullptr) : json(s); }

// What consolidation did with one section's memories, for the log.
struct Tally {
    int added = 0, same = 0, updated = 0, conflicts = 0;
};

// The names a grounded memory relies on: who, to_whom, then the checked names as written.
std::vector<std::string> memory_names(const json &g) {
    std::vector<std::string> out = {jstr(g, "who"), jstr(g, "to_whom")};
    for (const auto &c : jarr(g, "checked")) {
        if (jstr(c, "kind") != "name") continue;
        std::string name = jstr(c, "as_written");
        out.push_back(name.empty() ? jstr(c, "text") : name);
    }
    return out;
}

// Writes one grounded memory with its source, people, topic and card links, consolidated with the memories
// already known (sve_consolidate): the same memory again only gains this source, a later version supersedes the
// older one, a contradiction queues both. Each name becomes a known person only when it is clearly the same;
// a name that may be someone known becomes a new person and a question. The id of the row the source went to;
// -1 on failure (the caller rolls back).
int64_t store_memory(sve::Store &st, int64_t note_id, json g, int double_check, const std::string &self,
                     const sve::PersonAnswers &answers, Tally &tally) {
    const json src = jarr(g, "sources").empty() ? json::object() : jarr(g, "sources")[0];
    const int64_t start = jint(src, "start_sec", 0);
    const std::string quote = jstr(src, "quote"), text = jstr(g, "text");

    // People first: who / to_whom by role; the other names it relies on as "mentioned".
    std::vector<std::pair<int64_t, std::string>> links;
    json people = json::array();
    auto add_link = [&](int64_t tid, const char *role) {
        for (const auto &l : links) if (l.first == tid && (l.second == role || std::string(role) == "mentioned")) return;
        links.push_back({tid, role});
        people.push_back({{"id", tid}, {"role", role}});
    };
    for (const char *role : {"who", "to_whom"}) {
        int64_t tid = sve::resolve_person(st, note_id, jstr(g, role), self, text, answers);
        if (tid < 0) return -1;
        if (tid > 0) add_link(tid, role);
    }
    const std::vector<std::string> names = memory_names(g);
    for (size_t i = 2; i < names.size(); ++i) {
        int64_t tid = sve::resolve_person(st, note_id, names[i], self, text, answers);
        if (tid < 0) return -1;
        if (tid > 0) add_link(tid, "mentioned");
    }
    // The note's topic the memory names, else the note's first topic.
    json topics = st.note_topics(note_id, "topic");
    int64_t topic = 0;
    for (const auto &t : topics)
        if (sve::name_in(jstr(t, "name"), text) || sve::name_in(jstr(t, "name"), quote)) { topic = jint(t, "id", 0); break; }
    if (!topic && !topics.empty()) topic = jint(topics[0], "id", 0);

    // The memory it repeats, updates or contradicts, among current memories of its type (no embeddings yet).
    json newer = {{"type", jstr(g, "type")}, {"text", text}, {"checked", jarr(g, "checked")},
                  {"said_date", jstr(g, "said_date")}, {"people", people},
                  {"topics", topic > 0 ? json::array({{{"id", topic}}}) : json::array()}};
    const sve::Placement place =
        sve::place_memory(newer, st.memory_items_with_topics({{"types", json::array({jstr(g, "type")})}}), -1);
    if (place.pair == sve::Pair::same) {
        std::vector<std::pair<int64_t, std::string>> all = links;
        if (topic > 0) all.push_back({topic, "topic"});
        if (!sve::merge_same_memory(st, place.older, note_id, start, quote, src.contains("conf") ? src["conf"] : json(nullptr),
                                    (int64_t)time(nullptr), all))
            return -1;
        ++tally.same;
        return refresh_importance(st, place.older) ? place.older : -1;
    }
    if (place.pair == sve::Pair::contradiction) g["conflict_with"] = place.older;

    json row = {{"type", jstr(g, "type")}, {"text", text}, {"who", null_if_empty(jstr(g, "who"))},
                {"to_whom", null_if_empty(jstr(g, "to_whom"))}, {"due_text", null_if_empty(jstr(g, "due_text"))},
                {"due_date", null_if_empty(jstr(g, "due_date"))}, {"said_date", null_if_empty(jstr(g, "said_date"))},
                {"checked", jarr(g, "checked")}, {"confidence", jstr(g, "confidence") == "low" ? "low" : "high"},
                {"health", jint(g, "health", 0) ? 1 : 0}, {"review", "new"},
                {"queue", sve::goes_to_queue(g, double_check, sve::kDirectToMapDefault) ? 1 : 0}, {"origin", "recording"}};
    if (int64_t sug = matching_suggestion(st, note_id, start, quote)) row["suggestion_id"] = sug;
    const int64_t id = st.add_memory_item(row);
    if (id <= 0) return -1;
    std::optional<double> conf;
    if (src.contains("conf") && src["conf"].is_number()) conf = src["conf"].get<double>();
    if (!st.add_memory_source(id, note_id, start, quote, conf)) return -1;
    for (const auto &l : links)
        if (!st.link_memory_entity(id, l.first, l.second)) return -1;
    if (topic > 0 && !st.link_memory_entity(id, topic, "topic")) return -1;
    if (!sve::apply_placement(st, place, id)) return -1;
    if (place.pair == sve::Pair::update) ++tally.updated;
    else if (place.pair == sve::Pair::contradiction) ++tally.conflicts;
    else ++tally.added;

    return refresh_importance(st, id) ? id : -1;
}

// The section's pieces that have text, with their transcript confidence. Section piece ranges include both ends,
// [first_piece, last_piece], as the app stores them (NoteSection, SectionWriter).
void section_material(sve::Store &st, int64_t note_id, const json &section, sve::SectionPieces &pieces,
                      std::vector<std::optional<double>> &conf) {
    const int64_t first = jint(section, "first_piece", 0), last = jint(section, "last_piece", 0);
    for (const auto &p : st.pieces(note_id)) {
        const int64_t idx = jint(p, "idx", -1);
        if (idx < first || idx > last) continue;
        const std::string t = jstr(p, "text");
        if (t.find_first_not_of(" \t\r\n") == std::string::npos) continue;
        pieces.push_back({jint(p, "start_sec", 0), t});
        conf.push_back(p.contains("conf") && p["conf"].is_number() ? std::optional<double>(p["conf"].get<double>()) : std::nullopt);
    }
}

// Every section of the note has its memories (so its relations review can be made).
bool relations_due(sve::Store &st, int64_t note_id) {
    const json done = st.memory_sections_done(note_id);
    for (const auto &s : st.sections(note_id)) {
        json idx = s.contains("idx") ? s["idx"] : json(nullptr);
        if (std::find(done.begin(), done.end(), idx) == done.end()) return false;
    }
    return true;
}

}  // namespace

int sve_memories_from_section(sve_ctx *ctx, int64_t note_id, int section_idx, sve_progress_cb cb, void *user) {
    return guard("sve_memories_from_section", -1, [&]() -> int {
        if (!ctx || section_idx < 0) return -1;
        sve::Store &st = *ctx->store;
        auto note = st.get(note_id);
        if (!note) return -1;
        json section;
        for (const auto &s : st.sections(note_id))
            if (jint(s, "idx", -1) == section_idx) section = s;
        if (!section.is_object()) return -1;
        for (const auto &d : st.memory_sections_done(note_id))
            if (d == section_idx) return 0;  // already made; running again would only duplicate them

        sve::SectionPieces pieces;
        std::vector<std::optional<double>> conf;
        section_material(st, note_id, section, pieces, conf);
        const std::string lang = note->lang == "te" ? "te" : "en";

        // Candidates: one memory-only call (the section summary call is left exactly as it was).
        json cands = json::array();
        if (!pieces.empty()) {
            if (!ctx->llm && !g_sve_model_reply) return -1;
            // Only the saved words this section says clearly (N7 rev 1; a piece under conf 0.80 does not count).
            std::string said;
            for (size_t i = 0; i < pieces.size(); ++i) {
                if (i < conf.size() && conf[i] && sve::piece_unclear(*conf[i])) continue;
                said += (said.empty() ? "" : "\n") + pieces[i].second;
            }
            const std::string sys = sve::context_system(st, sve::system_prompt(lang), false, &said);
            const std::string prompt = sve::memory_prompt(pieces, lang, sve::spelling_vocab_line(st, 400, &said));
            std::string out;
            Watch w{cb, user, false};
            if (!fits(ctx, sys, prompt, kMemoryMaxOut)) {
                sve_logf(1, "memories: section %d of note %lld does not fit", section_idx, (long long)note_id);
            } else if (!model_reply(ctx, "memories", sys, prompt, kMemoryMaxOut, 0, 1, out, w)) {
                if (w.cancelled) return -2;
                // The call itself failed (e.g. the GPU refused work in the background): nothing stored, still due.
                sve_logf(1, "memories: model call failed for section %d of note %lld", section_idx, (long long)note_id);
                return -3;
            } else {
                json j;
                const std::string reply = sve::strip_thinking(out);
                if (sve::first_json_object(reply, j) && j.is_object() && j.contains("memories")) {
                    cands = sve::parse_memory_candidates(j["memories"]);
                } else {
                    // Cut off at the cap (long Telugu sections): the whole items before the cut count, no redo.
                    cands = sve::parse_memory_candidates(sve::complete_memory_items(reply));
                    sve_logf(1, "memories: reply cut off for section %d of note %lld, %zu whole items kept", section_idx,
                             (long long)note_id, cands.size());
                }
            }
        }

        // Grounding, then the yes/no check where needed. Model calls all happen before anything is written,
        // so a cancel leaves the section untouched and due.
        const std::vector<std::string> meds = sve::known_med_names(st);
        const std::string said = local_ymd(note->created_at);
        std::vector<std::pair<json, int>> keep;  // grounded row, double_check
        for (size_t i = 0; i < cands.size(); ++i) {
            sve::Grounded g = sve::ground_memory(cands[i], pieces, conf, said, meds, sve::kLowConfidence);
            if (!g.ok) { dropped(note_id, section_idx, g.reason, cands[i]); continue; }
            int dc = sve::kNotDoubleChecked;
            if (g.needs_double_check) {
                std::string out;
                Watch w{cb, user, false};
                if (model_reply(ctx, "memory_check", kCheckSystem, sve::double_check_prompt(g.row), kYesNoMaxOut, (int)i,
                                (int)cands.size(), out, w))
                    dc = sve::parse_yes_no(sve::strip_thinking(out));
                else if (w.cancelled)
                    return -2;
                if (dc == 0) { dropped(note_id, section_idx, "double_check_no", cands[i]); continue; }
            }
            keep.push_back({g.row, dc});
        }

        // Names that may be someone already known: the model's same-person check, before anything is written.
        // Its answer only decides whether to ask the person; nobody is merged without them.
        const std::string self = self_name(st);
        sve::PersonAnswers answers;
        {
            std::vector<std::string> names;
            std::map<std::string, std::string> said_about;  // name -> the first memory text naming them
            for (const auto &k : keep)
                for (const auto &n : memory_names(k.first)) {
                    const std::string t = sve::trim_copy(n);
                    if (t.empty() || (!self.empty() && lower(t) == self)) continue;
                    names.push_back(t);
                    said_about.emplace(t, jstr(k.first, "text"));
                }
            const auto checks = sve::person_checks(names, st.topics("person"));
            for (size_t i = 0; i < checks.size(); ++i) {
                const std::string &name = checks[i].first;
                const int64_t tid = jint(checks[i].second, "id", 0);
                const std::string prompt = sve::same_person_prompt(jstr(checks[i].second, "name"), st.person_context(tid), name,
                                                                   said_about[name]);
                std::string out;
                Watch w{cb, user, false};
                if (model_reply(ctx, "person_check", sve::kSamePersonSystem, prompt, kYesNoMaxOut, (int)i, (int)checks.size(), out, w))
                    answers[{name, tid}] = sve::parse_yes_no(sve::strip_thinking(out));
                else if (w.cancelled)
                    return -2;
                // No answer (no model, or the call failed): unclear, so the person is asked.
            }
        }

        if (!st.begin()) return -1;
        int stored = 0;
        Tally tally;
        const size_t questions_before = st.person_questions().size();
        try {
            for (const auto &k : keep) {
                if (store_memory(st, note_id, k.first, k.second, self, answers, tally) <= 0) { st.rollback(); return -1; }
                ++stored;
            }
            // Fails for a note deleted meanwhile (foreign key): nothing is kept for it.
            if (!st.mark_memory_section(note_id, section_idx) || !st.commit()) { st.rollback(); return -1; }
        } catch (...) {
            st.rollback();
            throw;
        }
        // The note's last section: its "who decided, asked or told what" review, made in code from its memories.
        if (relations_due(st, note_id)) {
            int n = -1;
            if (st.begin()) {
                n = sve::refresh_relations(st, note_id);
                if (n < 0 || !st.commit()) { st.rollback(); n = -1; }
            }
            if (n < 0) sve_logf(1, "relations: review for note %lld not made", (long long)note_id);
            else sve_logf(1, "relations: note %lld: %d to check", (long long)note_id, n);
        }
        const size_t questions_after = st.person_questions().size();
        sve_logf(1, "memories: section %d of note %lld: %d new, %d same again, %d updated, %d conflicts, %zu person questions",
                 section_idx, (long long)note_id, tally.added, tally.same, tally.updated, tally.conflicts,
                 questions_after > questions_before ? questions_after - questions_before : 0);
        if (cb) cb("final", 1, 1, user);
        return stored;
    });
}

char *sve_memory_sections_due(sve_ctx *ctx, int64_t note_id) {
    return guard<char *>("sve_memory_sections_due", nullptr, [&]() -> char * {
        if (!ctx) return nullptr;
        json done = ctx->store->memory_sections_done(note_id), due = json::array();
        for (const auto &s : ctx->store->sections(note_id)) {
            json idx = s.contains("idx") ? s["idx"] : json(nullptr);
            if (std::find(done.begin(), done.end(), idx) == done.end()) due.push_back(idx);
        }
        return dump(due);
    });
}

char *sve_notes_needing_memories(sve_ctx *ctx, int limit) {
    return guard<char *>("sve_notes_needing_memories", nullptr, [&]() -> char * {
        return ctx ? dump(ctx->store->notes_needing_memories(limit)) : nullptr;
    });
}

char *sve_memories_json(sve_ctx *ctx, const char *filter_json) {
    return guard<char *>("sve_memories_json", nullptr, [&]() -> char * {
        if (!ctx) return nullptr;
        json f = filter_json ? parse(filter_json) : json::object();
        if (!f.is_object()) return nullptr;
        return dump(ctx->store->memory_items(f));
    });
}

char *sve_memory_get(sve_ctx *ctx, int64_t id) {
    return guard<char *>("sve_memory_get", nullptr, [&]() -> char * { return ctx ? dump(sve::with_change(ctx->store->memory_item(id))) : nullptr; });
}

char *sve_brain_map_json(sve_ctx *ctx, int64_t note_id) {
    return guard<char *>("sve_brain_map_json", nullptr, [&]() -> char * {
        if (!ctx) return nullptr;
        sve::Store &st = *ctx->store;
        json f = json::object();  // waiting ones too: memory_on_map decides
        if (note_id > 0) f["note_id"] = note_id;
        // Meaning pairs come with embeddings (not built yet): none for now.
        // Only relations the person accepted (sve_relations_accept) draw person links.
        json rf = {{"status", "accepted"}};
        if (note_id > 0) rf["note_id"] = note_id;
        return dump(sve::build_brain_map(st.memory_items_with_topics(f), st.topics("person"), st.topics("topic"), json::array(), note_id,
                                         6, st.relations(rf), note_id > 0 ? json::array() : st.note_topic_links()));
    });
}

char *sve_topic_memories_json(sve_ctx *ctx, int64_t topic_id, int64_t note_id) {
    return guard<char *>("sve_topic_memories_json", nullptr, [&]() -> char * { return ctx ? dump(ctx->store->topic_memories(topic_id, note_id)) : nullptr; });
}

int sve_memory_touch(sve_ctx *ctx, int64_t id) {
    return guard("sve_memory_touch", -1, [&] {
        if (!ctx) return -1;
        json row = ctx->store->memory_item(id);
        if (!row.is_object()) return -1;
        json f = {{"last_used", (int64_t)time(nullptr)}};
        return ctx->store->update_memory_item(id, f) && refresh_importance(*ctx->store, id) ? 0 : -1;
    });
}

int sve_memory_review(sve_ctx *ctx, int64_t id, const char *state) {
    return guard("sve_memory_review", -1, [&] {
        if (!ctx || !state) return -1;
        const std::string s = state;
        if (s != "kept" && s != "wrong") return -1;
        sve::Store &st = *ctx->store;
        json row = st.memory_item(id);
        if (!row.is_object() || jstr(row, "origin") == "medicine") return -1;  // follows the saved medicine
        if (!st.update_memory_item(id, {{"review", s}, {"queue", 0}})) return -1;
        return refresh_importance(st, id) ? 0 : -1;
    });
}

int sve_memory_edit(sve_ctx *ctx, int64_t id, const char *text) {
    return guard("sve_memory_edit", -1, [&] {
        if (!ctx || !text) return -1;
        const std::string t = sve::trim_copy(text);
        sve::Store &st = *ctx->store;
        json row = st.memory_item(id);
        if (t.empty() || !row.is_object() || jstr(row, "origin") == "medicine") return -1;
        if (!st.update_memory_item(id, {{"text", t}, {"review", "corrected"}, {"queue", 0}})) return -1;
        return refresh_importance(st, id) ? 0 : -1;
    });
}

int sve_memory_remove(sve_ctx *ctx, int64_t id) {
    return guard("sve_memory_remove", -1, [&] {
        if (!ctx) return -1;
        json row = ctx->store->memory_item(id);
        if (!row.is_object() || jstr(row, "origin") == "medicine") return -1;
        return ctx->store->delete_memory_item(id) ? 0 : -1;
    });
}

int sve_memory_set_done(sve_ctx *ctx, int64_t id, int done) {
    return guard("sve_memory_set_done", -1, [&] {
        return ctx && ctx->store->update_memory_item(id, {{"done", done ? 1 : 0}}) ? 0 : -1;
    });
}

char *sve_review_queue_json(sve_ctx *ctx) {
    return guard<char *>("sve_review_queue_json", nullptr, [&]() -> char * {
        if (!ctx) return nullptr;
        sve::Store &st = *ctx->store;
        // Queued rows of any review but wrong: a kept memory a later recording contradicts waits here too.
        json rows = st.memory_items({{"queue", 1}});
        // Newest recording first (its own creation time), then in the order said.
        std::map<int64_t, int64_t> created;
        using Key = std::tuple<int64_t, int64_t, int64_t, int64_t>;  // note created_at, note id, -start_sec, id
        auto key = [&](const json &m) -> Key {
            const json src = jarr(m, "sources");
            int64_t note = 0, start = 0;
            for (const auto &s : src) {
                int64_t n = jint(s, "note_id", 0);
                if (!created.count(n)) { auto r = st.get(n); created[n] = r ? r->created_at : 0; }
                if (!note || created[n] > created[note]) { note = n; start = jint(s, "start_sec", 0); }
            }
            return {note ? created[note] : INT64_MIN, note, -start, jint(m, "id", 0)};
        };
        std::vector<std::pair<Key, json>> keyed;
        for (auto &m : rows) keyed.push_back({key(m), m});
        std::stable_sort(keyed.begin(), keyed.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
        json out = json::array();
        for (auto &k : keyed) {
            json m = k.second;
            json s = m["suggestion_id"].is_number_integer() ? st.suggestion(m["suggestion_id"].get<int64_t>()) : json(nullptr);
            m["suggestion"] = s.is_object() && jstr(s, "status") == "pending" ? s : json(nullptr);
            out.push_back(m);
        }
        return dump(out);
    });
}

char *sve_relation_reviews_json(sve_ctx *ctx) {
    return guard<char *>("sve_relation_reviews_json", nullptr, [&]() -> char * { return ctx ? dump(sve::relation_reviews(*ctx->store)) : nullptr; });
}

int sve_relations_accept(sve_ctx *ctx, int64_t note_id, const char *edits_json) {
    return guard("sve_relations_accept", -1, [&] {
        if (!ctx) return -1;
        json e = edits_json && *edits_json ? parse(edits_json) : json::array();
        if (!e.is_array()) return -1;
        return sve::accept_relations(*ctx->store, note_id, e) ? 0 : -1;
    });
}

int sve_relations_dismiss(sve_ctx *ctx, int64_t note_id) {
    return guard("sve_relations_dismiss", -1, [&] { return ctx && sve::dismiss_relations(*ctx->store, note_id) ? 0 : -1; });
}

int sve_relations_refresh(sve_ctx *ctx, int64_t note_id) {
    return guard("sve_relations_refresh", -1, [&] {
        if (!ctx || !ctx->store->get(note_id)) return -1;
        sve::Store &st = *ctx->store;
        if (!st.begin()) return -1;
        const int n = sve::refresh_relations(st, note_id);
        if (n < 0 || !st.commit()) { st.rollback(); return -1; }
        return n;
    });
}

char *sve_person_questions_json(sve_ctx *ctx) {
    return guard<char *>("sve_person_questions_json", nullptr, [&]() -> char * { return ctx ? dump(ctx->store->person_questions()) : nullptr; });
}

int sve_person_question_answer(sve_ctx *ctx, int64_t id, int same) {
    return guard("sve_person_question_answer", -1, [&] { return ctx && ctx->store->answer_person_question(id, same != 0) ? 0 : -1; });
}

// ---------------------------------------------------------------- meaning search (Task 11)

namespace {

// The embedder's normalised vector of text (with its prompt), or empty on failure.
std::vector<float> embed_text(sve_ctx *ctx, const std::string &prompt, const std::string &text) {
    std::vector<float> raw;
    if (!ctx->embed || !ctx->embed->embed(prompt + text, raw)) return {};
    return sve::embed_normalize(std::move(raw), sve::kEmbedDims);
}

}  // namespace

int sve_load_embedder(sve_ctx *ctx, const char *gguf_path, int n_threads) {
    if (!ctx || !gguf_path) return -1;
    return guard("sve_load_embedder", -1, [&] {
        ctx->embed.reset();
        ctx->embed_model.clear();
        auto e = std::make_unique<sve::Llm>();
        // CPU only, 2048 tokens (a piece is far shorter); Gemma (ctx->llm) is not touched.
        if (!e->load(gguf_path, 2048, n_threads, 0, true)) return -2;
        std::string name = gguf_path;
        size_t slash = name.find_last_of('/');
        if (slash != std::string::npos) name = name.substr(slash + 1);
        ctx->embed = std::move(e);
        ctx->embed_model = name + ":" + std::to_string(sve::kEmbedDims);
        return 0;
    });
}

void sve_unload_embedder(sve_ctx *ctx) {
    if (!ctx) return;
    ctx->embed.reset();
    ctx->embed_model.clear();
}

int sve_embedder_loaded(const sve_ctx *ctx) { return ctx && ctx->embed ? 1 : 0; }

int sve_embed_pending(sve_ctx *ctx, int limit, sve_progress_cb cb, void *user) {
    if (!ctx || !ctx->store) return -1;
    return guard("sve_embed_pending", -1, [&] {
        if (!ctx->embed) return -1;
        json rows = ctx->store->embed_pending(ctx->embed_model, limit);
        const int total = (int)rows.size();
        int done = 0, failed = 0;
        for (int i = 0; i < total; i++) {
            if (cb && cb("embed", i, total, user)) return -2;
            const json &r = rows[i];
            const std::string text = jstr(r, "text");
            std::vector<float> v = embed_text(ctx, sve::kEmbedDocPrompt, text);
            if (v.empty()) { failed++; continue; }  // logged by embed
            // Refused when the text changed meanwhile: still pending, embedded again next time.
            if (ctx->store->set_embedding(jstr(r, "kind"), jint(r, "ref", 0), v, ctx->embed_model, text)) done++;
        }
        if (cb && cb("embed", total, total, user)) return -2;
        // Every item of a batch failed to embed: the embedder is broken, so a loop until 0 would never end.
        if (total > 0 && failed == total) return -1;
        return done;
    });
}

char *sve_search_json(sve_ctx *ctx, const char *query, int limit) {
    if (!ctx || !ctx->store || !query) return nullptr;
    return guard("sve_search_json", (char *)nullptr, [&]() -> char * {
        sve::Store &st = *ctx->store;
        const std::string q = query;
        const std::string kq = sve::keyword_query(q);
        const std::string fq = kq.empty() ? q : kq;
        std::vector<int64_t> piece_kw, mem_kw, piece_mn, mem_mn;
        for (const auto &h : st.search_piece_refs(fq, sve::kSearchKeywordTop)) piece_kw.push_back(h["ref"].get<int64_t>());
        for (const auto &id : st.search_memory_refs(fq, sve::kSearchKeywordTop)) mem_kw.push_back(id.get<int64_t>());
        if (ctx->embed) {
            std::vector<float> qv = embed_text(ctx, sve::kEmbedQueryPrompt, q);
            if (!qv.empty()) {
                piece_mn = sve::meaning_ranked(qv, st.embeddings("piece", ctx->embed_model), sve::kSearchMeaningTop);
                mem_mn = sve::meaning_ranked(qv, st.embeddings("memory", ctx->embed_model), sve::kSearchMeaningTop);
            }
        }
        std::vector<sve::SearchHit> hits = sve::fuse_search(piece_kw, piece_mn, mem_kw, mem_mn, limit > 0 ? limit : 10);
        std::vector<int64_t> piece_ids;
        for (const auto &h : hits) if (h.kind == "piece") piece_ids.push_back(h.ref);
        std::map<int64_t, json> pieces;
        for (const auto &p : st.piece_refs(piece_ids)) pieces[p["ref"].get<int64_t>()] = p;
        json out = json::array();
        for (const auto &h : hits) {
            if (h.kind == "piece") {
                auto it = pieces.find(h.ref);
                if (it == pieces.end()) continue;  // deleted since the search
                out.push_back({{"kind", "piece"}, {"note_id", it->second["note_id"]}, {"start_sec", it->second["start_sec"]},
                               {"score", h.score}});
            } else {
                out.push_back({{"kind", "memory"}, {"memory_id", h.ref}, {"score", h.score}});
            }
        }
        return dump(out);
    });
}
