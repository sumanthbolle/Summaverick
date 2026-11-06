// Personal assistant C API: extraction, suggestions, profile, medicines, Ask, topics, insights.
// The checks that keep medicine information safe live in sve_suggest.cpp (pure, unit-tested);
// this file wires them to the store and the model. Nothing here throws out of a C function.
#include "sve_core.h"
#include "sve_internal.h"
#include "sve_context.h"
#include "sve_extract.h"
#include "sve_learn.h"
#include "sve_medcheck.h"
#include "sve_prompts.h"
#include "sve_suggest.h"

#include <algorithm>
#include <cstring>
#include <ctime>
#include <optional>

using sve::json;
using sve::jstr;
using sve::jint;
using sve::jarr;
using sve::jobj;

namespace {

char *dup(const std::string &s) {
    char *p = (char *)malloc(s.size() + 1);
    if (!p) return nullptr;
    memcpy(p, s.data(), s.size());
    p[s.size()] = 0;
    return p;
}

char *dump(const json &j) { return dup(j.dump()); }

// Runs f; any exception is logged and turned into the error value.
template <class R, class F>
R guard(const char *name, R err, F f) {
    try {
        return f();
    } catch (const std::exception &e) {
        sve_logf(2, "%s failed: %s", name, e.what());
    } catch (...) {
        sve_logf(2, "%s failed", name);
    }
    return err;
}

json parse(const char *s) {
    if (!s) return json();
    json j = json::parse(s, nullptr, false);
    return j.is_discarded() ? json() : j;
}

std::string today_iso() {
    time_t t = time(nullptr);
    struct tm lt;
    localtime_r(&t, &lt);
    char b[16];
    strftime(b, sizeof b, "%Y-%m-%d", &lt);
    return b;
}

json profile_of(sve::Store &st) {
    auto p = st.setting_get("profile");
    json j = p ? json::parse(*p, nullptr, false) : json();
    return j.is_object() ? j : json::object();
}

std::string lang_of(sve::Store &st) { return jstr(profile_of(st), "lang") == "te" ? "te" : "en"; }

std::string lower(std::string s) {
    for (auto &c : s) if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
    return s;
}

json payload_of(const json &s) {
    json p = s.is_object() && s.contains("payload") ? s["payload"] : json();
    return p.is_object() ? p : json::object();
}

json int_or_null(const json &o, const char *k) {
    int64_t v = jint(o, k, INT64_MIN);
    return v == INT64_MIN ? json(nullptr) : json(v);
}

// Medicine names the person has (profile) or the model proposed from recordings.
std::vector<std::string> known_med_names(sve::Store &st) {
    std::vector<std::string> names;
    auto add = [&](const std::string &n) {
        if (!n.empty() && std::find(names.begin(), names.end(), n) == names.end()) names.push_back(n);
    };
    for (const auto &m : st.medicines()) add(jstr(m, "name"));
    for (const char *status : {"pending", "accepted"})
        for (const auto &s : st.suggestions(status)) add(jstr(jobj(payload_of(s), "medicine"), "name"));
    return names;
}

int64_t find_medicine(sve::Store &st, const std::string &name) {
    std::string n = sve::normalize_for_match(name);
    if (n.empty()) return 0;
    for (const auto &m : st.medicines())
        if (sve::normalize_for_match(jstr(m, "name")) == n) return jint(m, "id", 0);
    return 0;
}

const char *kSafety =
    "Rules: use only the recordings, profile and memory you are given. Never invent names, numbers, doses, "
    "times or dates. Never give medical advice; only repeat what was recorded or entered. If you are unsure "
    "or it is not there, say so.";

std::string reminder_text(const std::string &name, const std::string &dose, const std::string &lang) {
    std::string nd = dose.empty() ? name : name + " " + dose;
    return lang == "te" ? nd + " \xE0\xB0\xB5\xE0\xB1\x87\xE0\xB0\xB8\xE0\xB1\x81\xE0\xB0\x95\xE0\xB1\x8B\xE0\xB0\x82\xE0\xB0\xA1\xE0\xB0\xBF"  // వేసుకోండి
                        : "Take " + nd;
}

// Pending reminder suggestion for a stored medicine (spec §7.8b). Older pending ones for the same
// medicine are superseded. Skipped when a reminder for this medicine was already proposed from the note.
void propose_medicine_reminder(sve::Store &st, int64_t med_id, const std::string &quote, const json &note_id,
                               const json &start_sec) {
    json med;
    for (const auto &m : st.medicines()) if (jint(m, "id", 0) == med_id) med = m;
    if (!med.is_object()) return;
    std::string name = jstr(med, "name"), n = sve::normalize_for_match(name);
    if (!note_id.is_null())
        for (const char *status : {"pending", "accepted"})
            for (const auto &s : st.suggestions(status))
                if (jstr(s, "kind") == "reminder" && jint(s, "note_id", -1) == note_id.get<int64_t>() &&
                    sve::normalize_for_match(jstr(jobj(payload_of(s), "medicine"), "name")) == n) return;
    for (const auto &s : st.suggestions("pending"))
        if (jstr(s, "kind") == "reminder" && jint(s, "medicine_id", -1) == med_id)
            st.set_suggestion_status(jint(s, "id", 0), "superseded");
    std::string dose = jstr(med, "dose");
    json payload = {{"text", reminder_text(name, dose, lang_of(st))}, {"when_text", jstr(med, "when_text")},
                    {"times", jarr(med, "times")}, {"repeat", med.contains("repeat") ? med["repeat"] : json(nullptr)},
                    {"until_date", med.contains("until_date") ? med["until_date"] : json(nullptr)},
                    {"medicine", {{"name", name}, {"dose", dose}, {"frequency", ""}, {"duration", ""}}}};
    st.add_suggestion({{"kind", "reminder"}, {"payload", payload}, {"quote", quote}, {"note_id", note_id},
                       {"start_sec", start_sec}, {"medicine_id", med_id}, {"check_status", "ok"},
                       {"check_fields", json::array()}});
}

// Medicine row fields from a JSON object (person's input or an accepted payload). Times default to the
// routine times implied by when_text. Returns false on an invalid repeat or date.
bool medicine_fields(sve::Store &st, const json &in, json &m) {
    m = json::object();
    if (in.contains("name")) m["name"] = sve::trim_copy(jstr(in, "name"));
    if (in.contains("dose")) m["dose"] = jstr(in, "dose");
    std::string when = in.contains("when_text") ? jstr(in, "when_text") : jstr(in, "when");
    if (in.contains("when_text") || in.contains("when")) m["when_text"] = when;
    std::vector<std::string> times = sve::valid_times(jarr(in, "times"));
    if (times.empty()) times = sve::resolve_times(when, jobj(profile_of(st), "routine"));
    if (!times.empty() || in.contains("times")) m["times"] = times;
    std::string rep = jstr(in, "repeat");
    if (!rep.empty()) {
        if (sve::sanitize_repeat(rep).empty()) return false;
        m["repeat"] = rep;
    }
    if (in.contains("until_date")) {
        std::string u = jstr(in, "until_date");
        if (!u.empty() && !sve::valid_ymd(u)) return false;
        m["until_date"] = u.empty() ? json(nullptr) : json(u);
    }
    return true;
}

int64_t accept_reminder(sve::Store &st, const json &s, const json &p) {
    json med = jobj(p, "medicine");
    std::string text = jstr(p, "text");
    if (text.empty() && !jstr(med, "name").empty()) text = reminder_text(jstr(med, "name"), jstr(med, "dose"), lang_of(st));
    if (text.empty()) return -1;
    json med_id = int_or_null(s, "medicine_id");
    std::vector<std::string> times = sve::valid_times(jarr(p, "times"));
    if (times.empty()) times = sve::resolve_times(jstr(p, "when_text"), jobj(profile_of(st), "routine"));
    std::string rep = sve::sanitize_repeat(jstr(p, "repeat"));
    if (rep.empty()) rep = (!med.is_null() || !med_id.is_null()) ? "daily" : "once";
    std::string start = jstr(p, "start_date"), until = jstr(p, "until_date");
    if (!sve::valid_ymd(start)) start = today_iso();
    if (!med_id.is_null())  // the new reminder replaces earlier ones for this medicine
        for (const auto &r : st.reminders(true))
            if (jint(r, "medicine_id", -1) == med_id.get<int64_t>()) st.set_reminder_active(jint(r, "id", 0), false);
    return st.add_reminder({{"text", text}, {"times", times}, {"repeat", rep}, {"start_date", start},
                            {"until_date", sve::valid_ymd(until) ? json(until) : json(nullptr)},
                            {"note_id", int_or_null(s, "note_id")}, {"start_sec", int_or_null(s, "start_sec")},
                            {"medicine_id", med_id}});
}

int64_t accept_profile(sve::Store &st, const json &s, const json &p) {
    std::string change = jstr(p, "change");
    json med = jobj(p, "medicine");
    json note = int_or_null(s, "note_id");
    if (change == "routine") {
        std::string t = jstr(p, "text");
        return t.empty() ? -1 : st.add_memory("fact", t, note.is_null() ? std::nullopt : std::optional<int64_t>(note.get<int64_t>()));
    }
    std::string name = jstr(med, "name");
    if (name.empty()) return -1;
    int64_t existing = find_medicine(st, name);
    if (change == "stop_medicine") {
        if (!existing) return 0;
        return st.delete_medicine(existing) ? existing : -1;
    }
    if (change != "add_medicine" && change != "update_medicine") return -1;
    json in = {{"name", name}};
    if (!jstr(med, "dose").empty()) in["dose"] = jstr(med, "dose");
    std::string when = jstr(p, "when_text");
    if (when.empty()) when = jstr(med, "frequency");
    if (!when.empty()) in["when_text"] = when;
    for (const char *k : {"times", "repeat", "until_date"}) if (p.contains(k) && !p[k].is_null()) in[k] = p[k];
    json m;
    if (!medicine_fields(st, in, m)) return -1;
    int64_t id = existing;
    if (existing) { if (!st.update_medicine(existing, m)) return -1; }
    else id = st.add_medicine(m);
    if (id > 0) propose_medicine_reminder(st, id, jstr(s, "quote"), note, int_or_null(s, "start_sec"));
    return id;
}

int64_t accept_memory(sve::Store &st, const json &s, const json &p, const std::string &kind) {
    std::string t = jstr(p, "text");
    if (t.empty()) return -1;
    if (kind == "vocab")
        for (const auto &m : st.memory("vocab"))
            if (lower(jstr(m, "text")) == lower(t)) return jint(m, "id", -1);
    json note = int_or_null(s, "note_id");
    return st.add_memory(kind, t, note.is_null() ? std::nullopt : std::optional<int64_t>(note.get<int64_t>()));
}

// Text of the piece of a note starting at start_sec (for quoting cited sources).
std::string piece_text(sve::Store &st, int64_t note_id, int64_t start_sec) {
    for (const auto &p : st.pieces(note_id)) if (jint(p, "start_sec", -1) == start_sec) return jstr(p, "text");
    return "";
}

json ask_result(const std::string &answer, const json &sources, bool not_found, bool medicine) {
    return {{"answer", answer}, {"sources", sources}, {"not_found", not_found}, {"medicine", medicine}};
}

json medicine_route(sve::Store &st, const std::string &q, const std::vector<std::string> &names, const std::string &lang) {
    std::vector<std::string> asked = sve::names_in_question(q, names);
    json meds = json::array();
    for (const auto &m : st.medicines()) {
        std::string n = sve::normalize_for_match(jstr(m, "name"));
        bool want = asked.empty();
        for (const auto &a : asked) if (sve::normalize_for_match(a) == n) want = true;
        if (want) meds.push_back(m);
    }
    json hits = json::array(), sources = json::array();
    for (const auto &name : asked.empty() ? names : asked)
        for (const auto &h : st.search_pieces(name, 3)) {
            if (hits.size() >= 3) break;
            bool dup = false;
            for (const auto &x : hits)
                dup = dup || (jint(x, "note_id", 0) == jint(h, "note_id", 0) && jint(x, "start_sec", 0) == jint(h, "start_sec", 0));
            if (dup) continue;
            hits.push_back(h);
            sources.push_back({{"note_id", jint(h, "note_id", 0)}, {"start_sec", jint(h, "start_sec", 0)}, {"title", jstr(h, "title")}});
        }
    std::string answer = sve::medicine_answer(meds, hits, lang);
    if (answer.empty()) return ask_result(sve::not_found_text(lang), json::array(), true, true);
    return ask_result(answer, sources, false, true);
}

}  // namespace

// ---------------------------------------------------------------- pieces

int sve_note_set_pieces(sve_ctx *ctx, int64_t note_id, const char *pieces_json) {
    return guard("sve_note_set_pieces", -1, [&] {
        if (!ctx) return -1;
        json in = parse(pieces_json);
        if (!in.is_array()) return -1;
        json clean = json::array();
        for (const auto &p : in)
            if (p.is_object()) clean.push_back({{"start_sec", jint(p, "start_sec", 0)}, {"text", jstr(p, "text")}});
        return ctx->store->set_pieces(note_id, clean) ? 0 : -1;
    });
}

char *sve_note_pieces(sve_ctx *ctx, int64_t note_id) {
    return guard<char *>("sve_note_pieces", nullptr, [&]() -> char * {
        return ctx ? dump(ctx->store->pieces(note_id)) : nullptr;
    });
}

// ---------------------------------------------------------------- extraction

int sve_extract(sve_ctx *ctx, int64_t note_id, sve_progress_cb cb, void *user) {
    return guard("sve_extract", -1, [&]() -> int {
        if (!ctx || !ctx->llm) return -1;
        sve::Store &st = *ctx->store;
        sve::Llm &llm = *ctx->llm;
        auto note = st.get(note_id);
        if (!note) return -1;
        json pieces = st.pieces(note_id);
        if (pieces.empty() && note->transcript) {  // older notes without pieces: no exact times
            for (const auto &part : sve::split_text(*note->transcript, 1200))
                pieces.push_back({{"start_sec", 0}, {"text", part}});
        }
        if (pieces.empty()) return 0;

        const std::string ab = sve::always_block(st), today = today_iso() + " (" + sve::weekday_name(today_iso()) + ")";
        const std::string sys = std::string("You turn a recording's transcript into proposals for a personal assistant. ") + kSafety;
        const int max_new = 900;
        // Each window's transcript must fit next to the prompt, the always block and the reply.
        int scaffold = llm.count_tokens(llm.apply_template(sys, sve::extraction_prompt(ab, "", today)));
        int budget = std::max(256, std::min(4500, llm.n_ctx - max_new - scaffold - 64));
        std::vector<json> windows = sve::group_windows(pieces, budget, [&](const std::string &s) { return llm.count_tokens(s); });

        std::vector<sve::Extracted> parts;
        const int total = (int)windows.size();
        for (int i = 0; i < total; ++i) {
            std::string prompt = llm.apply_template(sys, sve::extraction_prompt(ab, sve::pieces_with_markers(windows[i]), today));
            std::string out;
            if (!llm.complete(prompt, max_new, out, cb, user, "chunk", i, total, true)) return -2;
            sve::Extracted e = sve::parse_extraction(sve::strip_thinking(out));
            if (!e.ok) sve_logf(1, "extract: window %d/%d gave no usable JSON", i + 1, total);
            parts.push_back(std::move(e));
        }
        sve::Extracted merged = sve::merge_extracted(parts);
        if (!merged.ok) { sve_logf(1, "extract: no usable output for note %lld", (long long)note_id); return 0; }

        // A re-run replaces this note's earlier proposals that are still waiting.
        for (const auto &s : st.suggestions("pending"))
            if (jint(s, "note_id", -1) == note_id) st.set_suggestion_status(jint(s, "id", 0), "superseded");

        std::vector<std::string> known_vocab;
        for (const auto &m : st.memory("vocab")) known_vocab.push_back(lower(jstr(m, "text")));
        for (const auto &s : st.suggestions("pending"))
            if (jstr(s, "kind") == "vocab") known_vocab.push_back(lower(jstr(payload_of(s), "text")));
        int added = 0;
        for (const auto &s : sve::build_suggestions(merged, pieces, note_id, jobj(profile_of(st), "routine"), today_iso())) {
            if (jstr(s, "kind") == "vocab") {
                std::string t = lower(jstr(payload_of(s), "text"));
                if (std::find(known_vocab.begin(), known_vocab.end(), t) != known_vocab.end()) continue;
                known_vocab.push_back(t);
            }
            if (st.add_suggestion(s) > 0) ++added;
        }
        const std::string self = lower(jstr(profile_of(st), "name"));
        for (const auto &[kind, names] : {std::make_pair("topic", merged.topics), std::make_pair("person", merged.people)}) {
            json existing = st.topics(kind);
            for (const auto &n : names) {
                std::string name = n.get<std::string>();
                if (name.size() > 80 || (std::string(kind) == "person" && !self.empty() && lower(name) == self)) continue;
                for (const auto &t : existing)
                    if (lower(jstr(t, "name")) == lower(name)) name = jstr(t, "name");
                int64_t tid = st.upsert_topic(kind, name);
                if (tid > 0) st.link_topic(note_id, tid);
            }
        }
        if (cb) cb("final", total, total, user);
        return added;
    });
}

// ---------------------------------------------------------------- suggestions

char *sve_suggestions_json(sve_ctx *ctx, const char *status) {
    return guard<char *>("sve_suggestions_json", nullptr, [&]() -> char * {
        return ctx ? dump(ctx->store->suggestions(status ? status : "")) : nullptr;
    });
}

int64_t sve_suggestion_accept(sve_ctx *ctx, int64_t id, const char *edited_payload_json) {
    return guard<int64_t>("sve_suggestion_accept", -1, [&]() -> int64_t {
        if (!ctx) return -1;
        sve::Store &st = *ctx->store;
        json s = st.suggestion(id);
        if (!s.is_object() || jstr(s, "status") != "pending") return -1;
        const std::string kind = jstr(s, "kind");
        const json before = payload_of(s);
        json p = before;
        if (edited_payload_json) {
            json e = parse(edited_payload_json);
            if (!e.is_object()) return -1;
            for (auto it = e.begin(); it != e.end(); ++it) p[it.key()] = it.value();
        }
        int64_t row = -1;
        if (kind == "reminder") row = accept_reminder(st, s, p);
        else if (kind == "task") {
            std::string text = jstr(p, "text"), who = jstr(p, "who"), due = jstr(p, "due_date");
            if (!text.empty())
                row = st.add_task({{"text", text}, {"who", who.empty() ? json(nullptr) : json(who)},
                                   {"due_date", sve::valid_ymd(due) ? json(due) : json(nullptr)},
                                   {"note_id", int_or_null(s, "note_id")}, {"start_sec", int_or_null(s, "start_sec")}});
        } else if (kind == "profile") row = accept_profile(st, s, p);
        else if (kind == "vocab") row = accept_memory(st, s, p, "vocab");
        else if (kind == "fact") row = accept_memory(st, s, p, jstr(p, "memory_kind") == "preference" ? "preference" : "fact");
        if (row < 0) return -1;
        bool edited = p != before;
        if (edited) sve::record_edit(st, kind, before, p);
        st.set_suggestion_status(id, "accepted");
        st.log_decision(kind, "decision", "", edited ? "edit" : "yes");
        return row;
    });
}

int sve_suggestion_reject(sve_ctx *ctx, int64_t id) {
    return guard("sve_suggestion_reject", -1, [&] {
        if (!ctx) return -1;
        json s = ctx->store->suggestion(id);
        if (!s.is_object() || jstr(s, "status") != "pending") return -1;
        if (!ctx->store->set_suggestion_status(id, "rejected")) return -1;
        ctx->store->log_decision(jstr(s, "kind"), "decision", "", "no");
        return 0;
    });
}

// ---------------------------------------------------------------- profile and medicines

char *sve_profile_get(sve_ctx *ctx) {
    return guard<char *>("sve_profile_get", nullptr, [&]() -> char * {
        if (!ctx) return nullptr;
        auto p = ctx->store->setting_get("profile");
        json j = p ? json::parse(*p, nullptr, false) : json();
        return dump(j.is_object() ? j : json(nullptr));
    });
}

int sve_profile_set(sve_ctx *ctx, const char *js) {
    return guard("sve_profile_set", -1, [&] {
        json j = parse(js);
        if (!ctx || !j.is_object()) return -1;
        return ctx->store->setting_set("profile", j.dump()) ? 0 : -1;
    });
}

char *sve_medicines_json(sve_ctx *ctx) {
    return guard<char *>("sve_medicines_json", nullptr, [&]() -> char * { return ctx ? dump(ctx->store->medicines()) : nullptr; });
}

int64_t sve_medicine_save(sve_ctx *ctx, const char *js) {
    return guard<int64_t>("sve_medicine_save", -1, [&]() -> int64_t {
        json in = parse(js);
        if (!ctx || !in.is_object()) return -1;
        sve::Store &st = *ctx->store;
        json m;
        if (!medicine_fields(st, in, m)) return -1;
        int64_t id = jint(in, "id", 0);
        if (id == 0) {
            if (jstr(m, "name").empty()) return -1;
            id = st.add_medicine(m);
            if (id <= 0) return -1;
        } else if (m.contains("name") && jstr(m, "name").empty()) {
            return -1;
        } else if (!m.empty() && !st.update_medicine(id, m)) {
            return -1;
        }
        // Quote = what the person typed, so the card shows their own words.
        std::string quote;
        for (const auto &r : st.medicines())
            if (jint(r, "id", 0) == id)
                for (const char *k : {"name", "dose", "when_text"})
                    if (!jstr(r, k).empty()) quote += (quote.empty() ? "" : " ") + jstr(r, k);
        if (quote.empty()) return -1;  // unknown id
        propose_medicine_reminder(st, id, quote, nullptr, nullptr);
        return id;
    });
}

int sve_medicine_delete(sve_ctx *ctx, int64_t id) {
    return guard("sve_medicine_delete", -1, [&] { return ctx && ctx->store->delete_medicine(id) ? 0 : -1; });
}

// ---------------------------------------------------------------- reminders, tasks, topics, memory

char *sve_reminders_json(sve_ctx *ctx, int active_only) {
    return guard<char *>("sve_reminders_json", nullptr, [&]() -> char * { return ctx ? dump(ctx->store->reminders(active_only != 0)) : nullptr; });
}
int sve_reminder_set_active(sve_ctx *ctx, int64_t id, int active) {
    return guard("sve_reminder_set_active", -1, [&] { return ctx && ctx->store->set_reminder_active(id, active != 0) ? 0 : -1; });
}
char *sve_tasks_json(sve_ctx *ctx, int open_only) {
    return guard<char *>("sve_tasks_json", nullptr, [&]() -> char * { return ctx ? dump(ctx->store->tasks(open_only != 0)) : nullptr; });
}
int sve_task_set_done(sve_ctx *ctx, int64_t id, int done) {
    return guard("sve_task_set_done", -1, [&] { return ctx && ctx->store->set_task_done(id, done != 0) ? 0 : -1; });
}
char *sve_topics_json(sve_ctx *ctx, const char *kind) {
    return guard<char *>("sve_topics_json", nullptr, [&]() -> char * { return ctx ? dump(ctx->store->topics(kind ? kind : "")) : nullptr; });
}
char *sve_topic_notes_json(sve_ctx *ctx, int64_t topic_id) {
    return guard<char *>("sve_topic_notes_json", nullptr, [&]() -> char * { return ctx ? dump(ctx->store->topic_notes(topic_id)) : nullptr; });
}
int sve_topic_rename(sve_ctx *ctx, int64_t topic_id, const char *name) {
    return guard("sve_topic_rename", -1, [&] { return ctx && name && *name && ctx->store->rename_topic(topic_id, name) ? 0 : -1; });
}
int sve_topic_merge(sve_ctx *ctx, int64_t from_id, int64_t into_id) {
    return guard("sve_topic_merge", -1, [&] { return ctx && ctx->store->merge_topics(from_id, into_id) ? 0 : -1; });
}
char *sve_memory_json(sve_ctx *ctx, const char *kind) {
    return guard<char *>("sve_memory_json", nullptr, [&]() -> char * { return ctx ? dump(ctx->store->memory(kind ? kind : "")) : nullptr; });
}
int sve_memory_update(sve_ctx *ctx, int64_t id, const char *text) {
    return guard("sve_memory_update", -1, [&] { return ctx && text && *text && ctx->store->update_memory(id, text) ? 0 : -1; });
}
int sve_memory_delete(sve_ctx *ctx, int64_t id) {
    return guard("sve_memory_delete", -1, [&] { return ctx && ctx->store->delete_memory(id) ? 0 : -1; });
}

int64_t sve_vocab_add(sve_ctx *ctx, const char *term) {
    return guard<int64_t>("sve_vocab_add", -1, [&]() -> int64_t {
        if (!ctx || !term) return -1;
        std::string t = sve::trim_copy(term);
        if (t.empty()) return -1;
        for (const auto &m : ctx->store->memory("vocab"))
            if (lower(jstr(m, "text")) == lower(t)) return jint(m, "id", -1);
        return ctx->store->add_memory("vocab", t, std::nullopt);
    });
}

char *sve_topic_summary(sve_ctx *ctx, int64_t topic_id, const char *lang_c, sve_progress_cb cb, void *user) {
    return guard<char *>("sve_topic_summary", nullptr, [&]() -> char * {
        if (!ctx) return nullptr;
        sve::Store &st = *ctx->store;
        json topic;
        for (const auto &t : st.topics("")) if (jint(t, "id", 0) == topic_id) topic = t;
        if (!topic.is_object()) return nullptr;
        const std::string lang = lang_c && *lang_c ? lang_c : lang_of(st);
        json cached = json::parse(jstr(topic, "summary"), nullptr, false);
        bool have = cached.is_object() && jstr(cached, "lang") == lang;
        if (have && jint(topic, "summary_stale", 1) == 0) return dump(cached);
        if (!ctx->llm) return have ? dump(cached) : nullptr;

        std::string material;
        for (const auto &n : st.topic_notes(topic_id)) {
            std::string body = jstr(n, "short_summary");
            if (body.empty())
                if (auto row = st.get(jint(n, "id", 0)); row && row->transcript)
                    body = sve::truncate_utf8(*row->transcript, 2000);
            if (!body.empty()) material += "### " + jstr(n, "title") + "\n" + body + "\n\n";
        }
        json empty = {{"summary", ""}, {"key_terms", json::array()}, {"quiz", json::array()}, {"lang", lang}};
        if (material.empty()) return dump(empty);

        sve::Llm &llm = *ctx->llm;
        const std::string sys = sve::context_system(st, sve::system_prompt(lang));
        const int max_out = 700;
        std::string fitted;
        bool fb = false;
        if (!sve::reduce_to_fit(llm, sys, material, llm.n_ctx - max_out - 400 - llm.count_tokens(sys), fitted, fb, cb, user))
            return nullptr;
        std::string userp =
            "Below are notes from recordings about \"" + jstr(topic, "name") + "\".\n"
            "Reply with ONE JSON object and nothing else: "
            "{\"summary\":\"\",\"key_terms\":[{\"term\":\"\",\"meaning\":\"\"}],\"quiz\":[{\"q\":\"\",\"a\":\"\"}]}\n"
            "- summary: at most 6 sentences on what the recordings say.\n"
            "- key_terms: up to 8 important terms, each with a one-line meaning taken from the notes.\n"
            "- quiz: up to 5 questions with short answers taken from the notes.\n"
            "Use only what is in the notes. Write the values in " + sve::language_name(lang) +
            "; keys stay in English.\n\nNotes:\n\"\"\"\n" + fitted + "\n\"\"\"\nJSON:";
        std::string out;
        if (!llm.complete(llm.apply_template(sys, userp), max_out, out, cb, user, "final", 0, 1, true)) return nullptr;
        json j;
        if (!sve::first_json_object(sve::strip_thinking(out), j)) { sve_logf(1, "topic summary: no JSON"); return nullptr; }
        json res = sve::sanitize_topic_summary(j);
        res["lang"] = lang;
        st.set_topic_summary(topic_id, res.dump());
        return dump(res);
    });
}

// ---------------------------------------------------------------- Ask

char *sve_assistant_ask(sve_ctx *ctx, const char *question, sve_progress_cb cb, void *user) {
    return guard<char *>("sve_assistant_ask", nullptr, [&]() -> char * {
        if (!ctx || !question) return nullptr;
        sve::Store &st = *ctx->store;
        const std::string q = question, lang = lang_of(st);
        const std::vector<std::string> names = known_med_names(st);
        // Medicine questions are answered from stored facts only, never generated (spec §7.8a).
        if (sve::is_medicine_question(q, names)) return dump(medicine_route(st, q, names, lang));
        if (!ctx->llm) return nullptr;

        std::string kq = sve::keyword_query(q);
        sve::Relevant rel = sve::relevant(st, kq.empty() ? q : kq, 6000);
        if (rel.sources.empty()) return dump(ask_result(sve::not_found_text(lang), json::array(), true, false));

        sve::Llm &llm = *ctx->llm;
        const std::string lname = sve::language_name(lang);
        const std::string sys = sve::context_system(st, std::string("You are Summaverick, a careful personal assistant. ") + kSafety);
        const std::string userp =
            "Material from the person's recordings and memory. Each recording excerpt starts with a label such as [S1].\n\n" +
            rel.block + "\nQuestion: " + q + "\n\n"
            "Answer in a few short sentences in " + lname + ", using only the material above. After each statement put "
            "the label of the excerpt it comes from, for example [S1]. If the material does not answer the question, "
            "reply with exactly NOT_FOUND and nothing else.";
        std::string out;
        if (!llm.complete(llm.apply_template(sys, userp), 300, out, cb, user, "final", 0, 1, true)) return nullptr;
        sve::Cited c = sve::apply_citations(sve::strip_thinking(out), rel.sources);
        if (c.not_found) return dump(ask_result(sve::not_found_text(lang), json::array(), true, false));
        if (sve::mentions_medicine(c.answer, names)) {
            // A generated answer drifted into medicines: show the cited words verbatim instead.
            json hits = json::array();
            for (const auto &s : c.sources) {
                json h = s;
                h["text"] = piece_text(st, jint(s, "note_id", 0), jint(s, "start_sec", 0));
                hits.push_back(h);
            }
            std::string a = sve::medicine_answer(json::array(), hits, lang);
            if (a.empty()) return dump(ask_result(sve::not_found_text(lang), json::array(), true, true));
            return dump(ask_result(a, c.sources, false, true));
        }
        return dump(ask_result(c.answer, c.sources, false, false));
    });
}

// ---------------------------------------------------------------- insights

char *sve_insight_generate(sve_ctx *ctx, int64_t note_id) {
    return guard<char *>("sve_insight_generate", nullptr, [&]() -> char * {
        if (!ctx || !ctx->llm) return nullptr;
        sve::Store &st = *ctx->store;
        const std::string today = today_iso();
        if (st.setting_get("insight.last_date").value_or("") == today) return dup("null");
        auto notes = st.list("", 10);
        if (notes.empty()) return dup("null");
        std::string material = "Recent recordings (newest first):\n";
        for (const auto &n : notes) material += "- " + n.title + ": " + sve::truncate_utf8(n.preview, 400) + "\n";
        json tasks = st.tasks(true);
        if (!tasks.empty()) {
            material += "\nOpen tasks:\n";
            for (size_t i = 0; i < tasks.size() && i < 10; ++i) {
                std::string due = jstr(tasks[i], "due_date");
                material += "- " + jstr(tasks[i], "text") + (due.empty() ? "" : " (due " + due + ")") + "\n";
            }
        }
        const std::string lang = lang_of(st);
        sve::Llm &llm = *ctx->llm;
        const std::string sys = sve::context_system(st, std::string("You are Summaverick, a careful personal assistant. ") + kSafety);
        const std::string userp = material + "\nToday is " + today + ".\n"
            "Write ONE short, helpful insight for the person, at most 2 sentences, in " + sve::language_name(lang) +
            ": a topic that keeps coming up, a deadline that is close, or a suggestion to revise a topic with its quiz. "
            "Use only the material above. Do not mention medicines, doses or health advice. "
            "If nothing useful stands out, reply with exactly NONE.";
        std::string out;
        if (!llm.complete(llm.apply_template(sys, userp), 120, out, nullptr, nullptr, "final", 0, 1, true)) return nullptr;
        std::string text = sve::strip_thinking(out);
        while (!text.empty() && (text.front() == '"' || text.front() == ' ')) text.erase(0, 1);
        while (!text.empty() && (text.back() == '"' || text.back() == ' ')) text.pop_back();
        if (text.empty() || text.find("NONE") != std::string::npos) return dup("null");
        if (sve::mentions_medicine(text, known_med_names(st))) { sve_logf(1, "insight dropped: mentions a medicine"); return dup("null"); }
        std::optional<int64_t> nid;
        if (note_id > 0 && st.get(note_id)) nid = note_id;
        int64_t id = st.add_insight(text, nid);
        if (id <= 0) return nullptr;
        st.setting_set("insight.last_date", today);
        return dump({{"id", id}, {"text", text}});
    });
}

char *sve_insight_latest(sve_ctx *ctx) {
    return guard<char *>("sve_insight_latest", nullptr, [&]() -> char * { return ctx ? dump(ctx->store->latest_insight()) : nullptr; });
}

int sve_insight_dismiss(sve_ctx *ctx, int64_t id) {
    return guard("sve_insight_dismiss", -1, [&] { return ctx && ctx->store->dismiss_insight(id) ? 0 : -1; });
}
