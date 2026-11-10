// Memory C API: memories from section summaries, the review queue and the person's own changes.
// The rules that decide what may become a memory live in sve_memory.cpp (pure, unit-tested); this file
// runs the model calls they need and writes the results. Nothing here throws out of a C function.
#include "sve_core.h"
#include "sve_internal.h"
#include "sve_context.h"
#include "sve_extract.h"
#include "sve_medcheck.h"
#include "sve_brainmap.h"
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

// A person topic for a spoken name, by the extraction's rules: at most 80 bytes, never the person
// themselves, an existing person of the same name (any case) reused, linked to the note. 0 for none.
int64_t person_topic(sve::Store &st, int64_t note_id, std::string name, const std::string &self) {
    name = sve::trim_copy(name);
    if (name.empty() || name.size() > 80 || sve::is_pronoun(name) || (!self.empty() && lower(name) == self)) return 0;
    for (const auto &t : st.topics("person"))
        if (lower(jstr(t, "name")) == lower(name)) name = jstr(t, "name");
    int64_t tid = st.upsert_topic("person", name);
    if (tid <= 0 || !st.link_topic(note_id, tid)) return -1;
    return tid;
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

// Writes one grounded memory with its source, people, topic and card links. -1 on failure (the caller
// rolls back).
int64_t store_memory(sve::Store &st, int64_t note_id, const json &g, int double_check, const std::string &self) {
    const json src = jarr(g, "sources").empty() ? json::object() : jarr(g, "sources")[0];
    const int64_t start = jint(src, "start_sec", 0);
    const std::string quote = jstr(src, "quote"), text = jstr(g, "text");
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

    // who / to_whom by role; the other names it relies on as "mentioned".
    std::vector<int64_t> roled;
    for (const char *role : {"who", "to_whom"}) {
        int64_t tid = person_topic(st, note_id, jstr(g, role), self);
        if (tid < 0 || (tid > 0 && !st.link_memory_entity(id, tid, role))) return -1;
        if (tid > 0) roled.push_back(tid);
    }
    for (const auto &c : jarr(g, "checked")) {
        if (jstr(c, "kind") != "name") continue;
        std::string name = jstr(c, "as_written");
        if (name.empty()) name = jstr(c, "text");
        int64_t tid = person_topic(st, note_id, name, self);
        if (tid < 0) return -1;
        if (tid > 0 && std::find(roled.begin(), roled.end(), tid) == roled.end() && !st.link_memory_entity(id, tid, "mentioned"))
            return -1;
    }
    // The note's topic the memory names, else the note's first topic.
    json topics = st.note_topics(note_id, "topic");
    int64_t topic = 0;
    for (const auto &t : topics)
        if (sve::name_in(jstr(t, "name"), text) || sve::name_in(jstr(t, "name"), quote)) { topic = jint(t, "id", 0); break; }
    if (!topic && !topics.empty()) topic = jint(topics[0], "id", 0);
    if (topic > 0 && !st.link_memory_entity(id, topic, "topic")) return -1;

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

        const std::string self = self_name(st);
        if (!st.begin()) return -1;
        int stored = 0;
        try {
            for (const auto &k : keep) {
                if (store_memory(st, note_id, k.first, k.second, self) <= 0) { st.rollback(); return -1; }
                ++stored;
            }
            // Fails for a note deleted meanwhile (foreign key): nothing is kept for it.
            if (!st.mark_memory_section(note_id, section_idx) || !st.commit()) { st.rollback(); return -1; }
        } catch (...) {
            st.rollback();
            throw;
        }
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
    return guard<char *>("sve_memory_get", nullptr, [&]() -> char * { return ctx ? dump(ctx->store->memory_item(id)) : nullptr; });
}

char *sve_brain_map_json(sve_ctx *ctx, int64_t note_id) {
    return guard<char *>("sve_brain_map_json", nullptr, [&]() -> char * {
        if (!ctx) return nullptr;
        sve::Store &st = *ctx->store;
        json f = {{"queue", 0}};
        if (note_id > 0) f["note_id"] = note_id;
        // Meaning pairs come with embeddings (not built yet): none for now.
        return dump(sve::build_brain_map(st.memory_items_with_topics(f), st.topics("person"), st.topics("topic"), json::array(), note_id));
    });
}

char *sve_topic_memories_json(sve_ctx *ctx, int64_t topic_id) {
    return guard<char *>("sve_topic_memories_json", nullptr, [&]() -> char * { return ctx ? dump(ctx->store->topic_memories(topic_id)) : nullptr; });
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
        json rows = st.memory_items({{"queue", 1}, {"reviews", json::array({"new"})}});
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
