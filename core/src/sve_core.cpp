// Summaverick Everyday — portable core implementation.
// Hosts: iOS (today), Android (next), the Summaverick Everyday device (Linux/ARM).
#include "sve_core.h"
#include "sve_internal.h"
#include <mutex>
#include "sve_prompts.h"
#include "sve_context.h"
#include "sve_audio.h"
#include "sve_sections.h"
#include "sve_assemble.h"
#include "sve_budget.h"
#include "sve_threads.h"
#include "sve_whisper_text.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <fstream>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using sve::reduce_to_fit;
using sve::split_text;
using sve::strip_thinking;
using sve::read_wav16k;

// ---------------------------------------------------------------- logging
static sve_log_cb g_log = nullptr;
static void *g_log_user = nullptr;

void sve_logf(int level, const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (g_log) g_log(level, buf, g_log_user);
    else fprintf(stderr, "[sve] %s\n", buf);
}

static void llama_log_bridge(ggml_log_level level, const char *text, void *) {
    // llama is chatty at INFO; only forward warnings and errors to the host.
    if (level >= GGML_LOG_LEVEL_WARN && g_log) {
        std::string s(text);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        if (!s.empty()) g_log(level == GGML_LOG_LEVEL_ERROR ? 2 : 1, s.c_str(), g_log_user);
    }
}

void sve_set_log(sve_log_cb cb, void *user) { g_log = cb; g_log_user = user; }

static char *dup_str(const std::string &s) {
    char *p = (char *)malloc(s.size() + 1);
    if (!p) return nullptr;
    memcpy(p, s.data(), s.size());
    p[s.size()] = 0;
    return p;
}

static std::once_flag g_backend_once;  // sve_init may run on two threads at once (two contexts)

const char *sve_version(void) { return SVE_VERSION; }

sve_ctx *sve_init(const char *db_path) {
    std::call_once(g_backend_once, [] {
        llama_log_set(llama_log_bridge, nullptr);
        llama_backend_init();
    });
    auto *c = new sve_ctx();
    c->store = std::make_unique<sve::Store>(db_path ? db_path : ":memory:");
    if (!c->store->ok()) {
        sve_logf(2, "store open failed: %s", c->store->last_error().c_str());
        delete c;
        return nullptr;
    }
    return c;
}

void sve_free(sve_ctx *ctx) {
    if (!ctx) return;
    sve_unload_audio(ctx);
#ifdef SVE_WITH_WHISPER
    sve_unload_asr(ctx);
#endif
    delete ctx;
}

// ---------------------------------------------------------------- LLM API
int sve_load_llm(sve_ctx *ctx, const char *gguf_path, int n_ctx, int n_threads, int n_gpu_layers) {
    if (!ctx || !gguf_path) return -1;
    sve_unload_audio(ctx);
    ctx->llm.reset();
    auto l = std::make_unique<sve::Llm>();
    if (!l->load(gguf_path, n_ctx, n_threads, n_gpu_layers, false, ctx->pair_enabled ? 2 : 1)) return -2;
    ctx->llm = std::move(l);
    return 0;
}

void sve_set_pair_enabled(sve_ctx *ctx, int on) {
    if (!ctx) return;
    ctx->pair_enabled = on != 0;
}

void sve_unload_llm(sve_ctx *ctx) { if (ctx) { sve_unload_audio(ctx); ctx->llm.reset(); } }
int sve_llm_loaded(const sve_ctx *ctx) { return ctx && ctx->llm ? 1 : 0; }
int sve_llm_needs_reload(const sve_ctx *ctx) { return ctx && ctx->llm && ctx->llm->n_gpu_layers > 0 && ctx->llm->decode_failed ? 1 : 0; }

int sve_set_threads(sve_ctx *ctx, int n_decode, int n_batch) {
    if (!ctx || !ctx->llm || !ctx->llm->ctx) return -1;
    sve::Llm &l = *ctx->llm;
    sve::ThreadPair t = sve::resolve_threads((int)std::thread::hardware_concurrency(), n_decode, n_batch);
    if (t.decode == l.n_threads && t.batch == l.n_threads_batch) return 0;
    llama_set_n_threads(l.ctx, t.decode, t.batch);
    l.n_threads = t.decode;
    l.n_threads_batch = t.batch;
    sve_logf(0, "threads: %d decode, %d batch", t.decode, t.batch);
    return 0;
}

// System prompt of the notes outputs: the always block (profile, vocabulary as spellings only if said) in front, without the medicines:
// notes are free text and must not pick up the profile's doses (spec §4). Only the saved words that `material`
// (the text the notes are written from) says are listed (N7 rev 1).
static std::string notes_system(sve_ctx *ctx, const std::string &lang, const std::string &material) {
    const std::string said = sve::said_text(material);  // [unclear] lines do not count as said
    return sve::context_system(*ctx->store, sve::system_prompt(lang), false, &said);
}

// The end of a notes output (sve_generate, sve_generate_from_sections): the first heading put back if the model
// continued from it, [unclear] tags removed, and a reply cut at the token cap trimmed to its last whole sentence or
// bullet (N7 rev 1). Then the two checks of the owner's note 19: an abbreviation's expansion that `source` (what the
// output was written from) never says is removed, and an action item that says there are none becomes "None".
static std::string finish_output_cut(std::string out, const std::string &prefix, int max_out, const std::string &source,
                                     bool cut);
static std::string finish_output(const sve::Llm &llm, std::string out, const std::string &prefix, int max_out,
                                 const std::string &source) {
    return finish_output_cut(std::move(out), prefix, max_out, source, llm.last_cut);
}
// finish_output for a reply that may not be llm's last one (a sequence of Llm::complete_pair): cut says whether it
// stopped at its cap.
static std::string finish_output_cut(std::string out, const std::string &prefix, int max_out, const std::string &source,
                                     bool cut) {
    // The material's [unclear] tags (also those carried over in reduce_to_fit's chunk bullets) are never shown.
    out = strip_thinking(sve::strip_unclear_tags(out));
    // The prompt ended with the first heading/label; put it back if the model continued from it.
    if (!prefix.empty() && out.rfind(prefix, 0) != 0) out = prefix + " " + out;
    if (cut) {
        const std::string t = sve::trim_cut_reply(out);
        sve_logf(1, "notes output reached its %d-token cap: trimmed to the last whole line (%zu -> %zu bytes)", max_out,
                 out.size(), t.size());
        out = t;
    }
    return sve::none_action_items(sve::drop_invented_expansions(out, source));
}

int sve_transcript_fits(sve_ctx *ctx, const char *transcript, const char *lang_c) {
    try {
        if (!ctx || !ctx->llm || !transcript) return -1;
        sve::Llm &llm = *ctx->llm;
        const std::string text(transcript);
        if (strip_thinking(text).empty()) return 1;   // sve_generate returns "" without the model
        const int sys_tokens = llm.count_tokens(notes_system(ctx, lang_c ? lang_c : "en", text));
        // The check reduce_to_fit makes first, with the budget sve_generate gives it.
        return sve::notes_material_fits(llm.count_tokens(text), llm.n_ctx, sys_tokens) ? 1 : 0;
    } catch (...) {
        return -1;
    }
}

char *sve_generate(sve_ctx *ctx, const char *transcript, sve_output_kind kind, const char *lang_c,
                   sve_progress_cb cb, void *user) {
    if (!ctx || !ctx->llm || !transcript) return nullptr;
    sve::Llm &llm = *ctx->llm;
    std::string lang = lang_c ? lang_c : "en";
    std::string text(transcript);
    if (strip_thinking(text).empty()) return dup_str("");

    // The always block (profile, vocabulary) goes in front of every system prompt (spec §7.3), here
    // without the medicines: notes are free text and must not pick up the profile's doses (spec §4).
    const std::string sys = notes_system(ctx, lang, text);  // the same system prompt sve_transcript_fits counts
    const int max_out = sve::notes_max_out(llm.n_ctx);
    // prompt scaffolding (kNotesScaffold) plus the system prompt (reduce_to_fit floors this)
    const int budget = sve::notes_budget(llm.n_ctx, llm.count_tokens(sys));

    std::string material;
    bool from_bullets = false;
    if (!reduce_to_fit(llm, sys, text, budget, material, from_bullets, cb, user)) return nullptr;

    std::string userp, prefix;
    switch (kind) {
        case SVE_OUT_SHORT_SUMMARY: userp = sve::short_summary_prompt(material, from_bullets); break;
        case SVE_OUT_DETAILED_NOTES: userp = sve::detailed_notes_prompt(material, from_bullets); prefix = "## Topic"; break;
        case SVE_OUT_REVISION_QUESTIONS: userp = sve::revision_questions_prompt(material, from_bullets); break;
        default: return nullptr;
    }
    std::string prompt = llm.apply_template(sys, userp);
    std::string out;
    if (!llm.complete(prompt, max_out, out, cb, user, "final", 0, 1)) return nullptr;
    return dup_str(finish_output(llm, out, prefix, max_out, text));
}

char *sve_generate_from_sections(sve_ctx *ctx, int64_t note_id, sve_output_kind kind, const char *lang_c,
                                 sve_progress_cb cb, void *user) {
    try {
        if (!ctx || !ctx->llm) return nullptr;
        sve::json secs = ctx->store->sections(note_id);
        if (!secs.is_array() || secs.empty()) return nullptr;
        sve::Llm &llm = *ctx->llm;
        const std::string lang = lang_c ? lang_c : "en";
        // Same budget as sve_generate; the saved words are those the sections say.
        const std::string sys = notes_system(ctx, lang, sve::sections_text(secs));
        const int sys_tokens = llm.count_tokens(sys);
        const int max_out = sve::notes_max_out(llm.n_ctx);
        const int budget = sve::notes_budget(llm.n_ctx, sys_tokens);
        auto count = [&llm](const std::string &t) { return llm.count_tokens(t); };

        std::vector<std::string> groups = sve::section_groups(secs, budget, count);
        if (groups.empty()) return nullptr;
        std::string material = groups[0];
        bool from_bullets = false;
        if (groups.size() > 1) {
            // Hierarchical: summarise each group (room as in reduce_to_fit's chunk step), then write from those.
            const int chunk_out = sve::chunk_max_out(llm.n_ctx);
            const int chunk_budget = sve::chunk_budget(llm.n_ctx, sys_tokens);
            groups = sve::section_groups(secs, std::max(budget, chunk_budget), count);
            std::string bullets;
            const int total = (int)groups.size();
            for (int i = 0; i < total; ++i) {
                std::string piece;
                if (!llm.complete(llm.apply_template(sys, sve::chunk_prompt(groups[i], i, total)), chunk_out, piece, cb, user, "chunk", i, total))
                    return nullptr;
                bullets += strip_thinking(piece) + "\n";
            }
            from_bullets = true;
            material.clear();
            bool more = false;
            if (!reduce_to_fit(llm, sys, bullets, budget, material, more, cb, user)) return nullptr;
        }

        std::string userp, prefix;
        switch (kind) {
            case SVE_OUT_SHORT_SUMMARY: userp = sve::short_summary_prompt(material, from_bullets); break;
            case SVE_OUT_DETAILED_NOTES: userp = sve::detailed_notes_prompt(material, from_bullets); prefix = "## Topic"; break;
            case SVE_OUT_REVISION_QUESTIONS: userp = sve::revision_questions_prompt(material, from_bullets); break;
            default: return nullptr;
        }
        std::string out;
        if (!llm.complete(llm.apply_template(sys, userp), max_out, out, cb, user, "final", 0, 1)) return nullptr;
        // Checked against the note's transcript and the sections it was written from.
        std::string source = sve::sections_text(secs);
        if (auto n = ctx->store->get(note_id); n && n->transcript) source += "\n" + *n->transcript;
        return dup_str(finish_output(llm, out, prefix, max_out, source));
    } catch (const std::exception &e) {
        sve_logf(2, "generate from sections failed: %s", e.what());
        return nullptr;
    } catch (...) {
        return nullptr;
    }
}

static bool section_kind(sve_output_kind kind, sve::SectionOutputKind &out) {
    if (kind == SVE_OUT_DETAILED_NOTES) { out = sve::SectionOutputKind::Notes; return true; }
    if (kind == SVE_OUT_REVISION_QUESTIONS) { out = sve::SectionOutputKind::Questions; return true; }
    return false;
}

// One part's reply checked as a stored part must be: finish_output against the part's own words and its summary
// (source), not empty, and for questions at least one readable question. False with error (and a log line) when not.
static bool check_part(sve::SectionOutputKind sk, const std::string &raw, bool cut, const std::string &source, int idx,
                       int attempt, std::string &done, std::string &error) {
    const std::string prefix = sk == sve::SectionOutputKind::Notes ? "## Topic" : "";
    done = finish_output_cut(raw, prefix, sve::section_output_max_out(sk), source, cut);
    if (sve::strip_unclear_tags(done).find_first_not_of(" \t\r\n") == std::string::npos || done == prefix) {
        error = "empty reply";
        sve_logf(1, "section %d output attempt %d was empty", idx, attempt + 1);
        return false;
    }
    // Questions that assemble_questions cannot read would give an empty part: not stored, tried again.
    if (sk == sve::SectionOutputKind::Questions && sve::count_questions(done) == 0) {
        error = "no questions in the reply";
        sve_logf(1, "section %d questions attempt %d had no question", idx, attempt + 1);
        return false;
    }
    return true;
}

// A part of a note: its own pieces' text (unclear ones marked, as sve_generate's material), its summary as a guide,
// its place among the sections (total 0: written during the recording, "of N" left out), and its piece range.
struct PartInput {
    int64_t first = 0, last = 0;
    std::string text, guide;
    int part = 1, total = 0;
};

static bool part_input(sve_ctx *ctx, int64_t note_id, int idx, PartInput &in) {
    if (!ctx->store->get(note_id)) return false;  // the note is gone
    const sve::json secs = ctx->store->sections(note_id);
    sve::json sec;
    for (const auto &s : secs)
        if (s.value("idx", -1) == idx) sec = s;
    if (!sec.is_object()) return false;
    in.first = sec.value("first_piece", (int64_t)0);
    in.last = sec.value("last_piece", (int64_t)0);
    in.text = sve::notes_material(sve::section_pieces(ctx->store->pieces(note_id), in.first, in.last));
    in.guide = sve::sections_text(sve::json::array({sec}));
    in.total = (int)secs.size();
    in.part = 1;
    for (const auto &s : secs) { if (s.value("idx", -1) == idx) break; ++in.part; }
    return true;
}

// A part written during the recording, from its pieces ([{"start_sec","text","conf"?}], the section's own) and its
// summary (as sve_section_summarize returns it): part idx + 1 of an unknown total.
static bool part_input_text(const char *pieces_json, const char *summary_json, int idx, PartInput &in) {
    if (!pieces_json || !summary_json || idx < 0) return false;
    const sve::json pieces = sve::json::parse(pieces_json, nullptr, false);
    const sve::json summary = sve::json::parse(summary_json, nullptr, false);
    if (!pieces.is_array() || !summary.is_object()) return false;
    int64_t start = 0;
    if (!pieces.empty() && pieces[0].is_object() && pieces[0].contains("start_sec") && pieces[0]["start_sec"].is_number())
        start = std::max<int64_t>(0, pieces[0]["start_sec"].get<int64_t>());
    in.text = sve::notes_material(pieces);
    in.guide = sve::sections_text(sve::json::array({{{"start_sec", start}, {"summary", summary}}}));
    in.part = idx + 1;
    in.total = 0;
    return true;
}

// Where a part's tries and replies go: the note's store (sve_section_output, sve_section_outputs_pair) or, for a part
// written during the recording, only memory (the app keeps it in the sidecar and puts it in the note at Stop).
struct PartSink {
    virtual ~PartSink() = default;
    // A try is about to run (counted before the model runs); false: refused (the note is gone).
    virtual bool begin(int kind) = 0;
    // A cancel takes the try back.
    virtual void undo(int kind) = 0;
    // A checked reply; false: refused (the note was deleted or its sections re-planned).
    virtual bool put(int kind, const std::string &text, bool cut) = 0;
    // The try failed; real: not a GPU refused in the background.
    virtual void fail(int kind, const std::string &error, bool real) = 0;
};

struct StoreSink : PartSink {
    sve::Store &st;
    int64_t note_id;
    int idx;
    std::string lang;
    int64_t first, last;
    StoreSink(sve::Store &s, int64_t n, int i, std::string l, int64_t f, int64_t la)
        : st(s), note_id(n), idx(i), lang(std::move(l)), first(f), last(la) {}
    bool begin(int k) override { return st.bump_step_attempt(note_id, idx, k, lang); }
    void undo(int k) override { st.undo_step_attempt(note_id, idx, k); }
    bool put(int k, const std::string &text, bool cut) override {
        if (st.set_section_output(note_id, idx, k, lang, first, last, text, cut)) return true;
        sve_logf(1, "section %d output not stored (note deleted or sections re-planned)", idx);
        return false;
    }
    void fail(int k, const std::string &error, bool real) override { st.set_step_error(note_id, idx, k, error, real); }
};

struct TextSink : PartSink {
    int idx;
    bool got[2] = {false, false}, cut[2] = {false, false};
    std::string text[2];
    explicit TextSink(int i) : idx(i) {}
    static int slot(int k) { return k == SVE_OUT_REVISION_QUESTIONS ? 1 : 0; }
    bool begin(int) override { return true; }
    void undo(int) override {}
    bool put(int k, const std::string &t, bool c) override {
        got[slot(k)] = true; text[slot(k)] = t; cut[slot(k)] = c;
        return true;
    }
    void fail(int k, const std::string &error, bool) override {
        sve_logf(1, "live part %d %s not written: %s", idx + 1, k == SVE_OUT_DETAILED_NOTES ? "notes" : "questions",
                 error.c_str());
    }
};

// Forwards the host's progress and remembers a cancel.
struct CancelWatch { sve_progress_cb cb; void *user; bool cancelled; };
static int watch_cb(const char *ph, int d, int t, void *u) {
    CancelWatch *x = static_cast<CancelWatch *>(u);
    if (x->cb && x->cb(ph, d, t, x->user)) x->cancelled = true;
    return x->cancelled ? 1 : 0;
}

// One part of one kind, its tries and its reply going to sink. greedy_only: the try was already counted and its
// sampled reply failed (sve_section_outputs_pair), so only the greedy retry runs. cancelled (optional) says
// whether NULL was a cancel. Returns the reply put in sink (dup'd), NULL when none was.
static char *section_output_run(sve_ctx *ctx, const PartInput &in, int idx, sve_output_kind kind, const char *lang_c,
                                PartSink &sink, sve_progress_cb cb, void *user, bool greedy_only, bool *cancelled_out) {
    if (cancelled_out) *cancelled_out = false;
    try {
        sve::SectionOutputKind sk;
        if (!ctx || !ctx->llm || !section_kind(kind, sk)) return nullptr;
        const std::string lang = lang_c ? lang_c : "en";
        const int k = (int)kind;
        const std::string &text = in.text;
        if (!greedy_only && !sink.begin(k)) return nullptr;  // counted before the model runs
        if (strip_thinking(text).empty()) {  // silence or music: an empty part, done
            if (!sink.put(k, "", false)) return nullptr;
            return dup_str("");
        }
        sve::Llm &llm = *ctx->llm;
        const std::string sys = notes_system(ctx, lang, text);
        const int max_out = sve::section_output_max_out(sk);
        // A part normally fits as it is; one that does not is reduced first, as in sve_generate.
        const int budget = sve::notes_budget(llm.n_ctx, llm.count_tokens(sys));
        CancelWatch w{cb, user, false};
        // A cancel (the host's background time is up) takes this call's count back, so it is never a "real" failure.
        // That holds even when the sampled try already failed and the greedy retry was cancelled: the failed try is
        // not counted either, and the step is simply tried again later (with both tries again).
        auto cancelled = [&]() -> char * {
            sink.undo(k);
            if (cancelled_out) *cancelled_out = true;
            return nullptr;
        };
        std::string material;
        bool from_bullets = false;
        if (!reduce_to_fit(llm, sys, text, budget, material, from_bullets, watch_cb, &w)) {
            if (w.cancelled) return cancelled();
            sink.fail(k, "the part could not be shortened to fit", true);
            return nullptr;
        }
        const std::string userp = sve::section_output_prompt(sk, material, in.guide, in.part, in.total, from_bullets);
        const std::string prompt = llm.apply_template(sys, userp);
        const char *phase = sk == sve::SectionOutputKind::Notes ? "section_notes" : "section_questions";
        std::string error = "no reply";
        for (int attempt = greedy_only ? 1 : 0; attempt < 2; ++attempt) {  // sampled, then greedy
            std::string out;
            if (!llm.complete(prompt, max_out, out, watch_cb, &w, phase, attempt, 2, attempt == 1)) {
                if (w.cancelled) return cancelled();
                error = llm.decode_failed ? "the run failed on the GPU" : "the run failed";
                sve_logf(1, "section %d output attempt %d failed", idx, attempt + 1);
                continue;
            }
            std::string done;
            if (!check_part(sk, out, llm.last_cut, text + "\n" + in.guide, idx, attempt, done, error)) continue;
            // cut: the reply reached its cap, so headings after the cut (Action items) may be missing; the
            // assembly then never says "None" for the action items.
            if (!sink.put(k, done, llm.last_cut)) return nullptr;
            return dup_str(done);
        }
        // A GPU refused in the background is a cut-off, not a real failure: its reason is kept, not counted.
        sink.fail(k, error, error != "the run failed on the GPU");
        return nullptr;
    } catch (const std::exception &e) {
        sve_logf(2, "section output failed: %s", e.what());
        sink.fail((int)kind, e.what(), true);
        return nullptr;
    } catch (...) {
        return nullptr;
    }
}

char *sve_section_output(sve_ctx *ctx, int64_t note_id, int idx, sve_output_kind kind, const char *lang_c,
                         sve_progress_cb cb, void *user) {
    try {
        sve::SectionOutputKind sk;
        if (!ctx || !ctx->llm || !section_kind(kind, sk)) return nullptr;
        PartInput in;
        if (!part_input(ctx, note_id, idx, in)) return nullptr;
        StoreSink sink(*ctx->store, note_id, idx, lang_c ? lang_c : "en", in.first, in.last);
        return section_output_run(ctx, in, idx, kind, lang_c, sink, cb, user, false, nullptr);
    } catch (...) {
        return nullptr;
    }
}

char *sve_section_output_text(sve_ctx *ctx, const char *pieces_json, const char *summary_json, int idx,
                              sve_output_kind kind, const char *lang_c, int *cut, sve_progress_cb cb, void *user) {
    if (cut) *cut = 0;
    try {
        sve::SectionOutputKind sk;
        if (!ctx || !ctx->llm || !section_kind(kind, sk)) return nullptr;
        PartInput in;
        if (!part_input_text(pieces_json, summary_json, idx, in)) return nullptr;
        TextSink sink(idx);
        char *r = section_output_run(ctx, in, idx, kind, lang_c, sink, cb, user, false, nullptr);
        if (r && cut) *cut = sink.cut[TextSink::slot((int)kind)] ? 1 : 0;
        return r;
    } catch (...) {
        return nullptr;
    }
}

int sve_section_output_put(sve_ctx *ctx, int64_t note_id, int idx, sve_output_kind kind, const char *lang_c,
                           int first_piece, int last_piece, const char *text, int cut) {
    try {
        sve::SectionOutputKind sk;
        if (!ctx || !text || !section_kind(kind, sk)) return -1;
        return ctx->store->put_section_output(note_id, idx, (int)kind, lang_c ? lang_c : "en", first_piece, last_piece,
                                              text, cut != 0);
    } catch (...) {
        return -1;
    }
}

// sve_section_outputs_pair's work, its tries and replies going to sink. Returns how many kinds were put (0-2), -2 on
// cancel, -1 on error.
static int section_outputs_pair_run(sve_ctx *ctx, const PartInput &in, int idx, const char *lang_c, PartSink &sink,
                                    sve_progress_cb cb, void *user) {
    static const sve_output_kind kKinds[2] = {SVE_OUT_DETAILED_NOTES, SVE_OUT_REVISION_QUESTIONS};
    // A kind whose try this call counted and that is neither put nor handed to a single call: an exception records
    // its reason, as section_output_run does.
    bool owned[2] = {false, false};
    try {
        if (!ctx || !ctx->llm) return -1;
        const std::string lang = lang_c ? lang_c : "en";
        sve::Llm &llm = *ctx->llm;
        static const sve::SectionOutputKind kSk[2] = {sve::SectionOutputKind::Notes, sve::SectionOutputKind::Questions};
        // One kind by itself (the single call), from its first try (greedy_only false) or only its greedy retry.
        // 1 put, 0 not, -2 cancelled.
        auto single = [&](int i, bool greedy_only) -> int {
            bool cancelled = false;
            char *r = section_output_run(ctx, in, idx, kKinds[i], lang_c, sink, cb, user, greedy_only, &cancelled);
            if (r) { free(r); return 1; }
            return cancelled ? -2 : 0;
        };
        auto both_single = [&]() -> int {
            int n = 0;
            for (int i = 0; i < 2; ++i) {
                const int r = single(i, false);
                if (r == -2) return -2;
                n += r;
            }
            return n;
        };
        // Silence (put "" without the model) or a part that must be reduced first: one at a time.
        const std::string sys = notes_system(ctx, lang, in.text);
        const int sys_tokens = llm.count_tokens(sys);
        if (strip_thinking(in.text).empty() || !sve::notes_material_fits(llm.count_tokens(in.text), llm.n_ctx, sys_tokens))
            return both_single();
        // The shared prefix (system, the part's place, guide and material) and each kind's instruction after it.
        const std::string shared = sve::section_prompt_shared(in.text, in.guide, in.part, in.total);
        std::string head[2], tail[2];
        bool split = true;
        for (int i = 0; i < 2; ++i) {
            const std::string suffix = sve::section_prompt_suffix(kSk[i]);
            split = split && sve::split_templated(llm.apply_template(sys, shared + suffix), shared, suffix, head[i], tail[i]);
        }
        if (!split || head[0] != head[1] ||
            !sve::section_pair_fits(ctx->pair_enabled ? (int)llama_n_seq_max(llm.ctx) : 1, llm.n_ctx, (int)llama_n_ctx_seq(llm.ctx),
                                    llm.count_tokens(head[0]) + 1, llm.count_tokens(tail[0]), llm.count_tokens(tail[1]))) {
            sve_logf(1, "section %d: notes and questions written one at a time (no room for both together)", idx);
            return both_single();
        }
        // Both tries counted before the model runs; a cancel takes back those not put.
        if (!sink.begin((int)kKinds[0])) return -1;
        owned[0] = true;
        if (!sink.begin((int)kKinds[1])) {
            sink.undo((int)kKinds[0]);
            owned[0] = false;
            return -1;
        }
        owned[1] = true;
        const std::string source = in.text + "\n" + in.guide;
        bool ended[2] = {false, false}, stored[2] = {false, false}, refused = false;
        std::string error[2];
        // Each reply is checked and put as soon as its sequence ends, so it stays after a cancel.
        auto on_done = [&](int i, const std::string &out, bool cut) {
            ended[i] = true;
            std::string done;
            if (!check_part(kSk[i], out, cut, source, idx, 0, done, error[i])) return;
            if (!sink.put((int)kKinds[i], done, cut)) { refused = true; return; }
            stored[i] = true;
            owned[i] = false;
        };
        CancelWatch w{cb, user, false};
        const int caps[2] = {sve::section_output_max_out(kSk[0]), sve::section_output_max_out(kSk[1])};
        const bool ok = llm.complete_pair(head[0], tail, caps, on_done, watch_cb, &w, "section_pair");
        const int n = (stored[0] ? 1 : 0) + (stored[1] ? 1 : 0);
        if (!ok && w.cancelled) {
            for (int i = 0; i < 2; ++i)
                if (!stored[i]) sink.undo((int)kKinds[i]);
            owned[0] = owned[1] = false;
            return -2;
        }
        if (refused) return n;
        int got = n;
        for (int i = 0; i < 2; ++i) {
            if (stored[i]) continue;
            int r;
            owned[i] = false;  // the single call records its own outcome from here
            if (ended[i] || llm.last_pair_decode_failed) {
                // The sampled reply failed (or the run did): the greedy retry, as the single call's second try.
                r = single(i, true);
            } else {
                // The pair stopped before this kind was tried at all: its count back, and a whole single call.
                sink.undo((int)kKinds[i]);
                r = single(i, false);
            }
            if (r == -2) return -2;
            got += r;
        }
        return got;
    } catch (const std::exception &e) {
        sve_logf(2, "section outputs pair failed: %s", e.what());
        for (int i = 0; i < 2; ++i)
            if (owned[i]) sink.fail((int)kKinds[i], e.what(), true);
        return -1;
    } catch (...) {
        for (int i = 0; i < 2; ++i)
            if (owned[i]) sink.fail((int)kKinds[i], "unknown error", true);
        return -1;
    }
}

int sve_section_outputs_pair(sve_ctx *ctx, int64_t note_id, int idx, const char *lang_c, sve_progress_cb cb,
                             void *user) {
    try {
        if (!ctx || !ctx->llm) return -1;
        PartInput in;
        if (!part_input(ctx, note_id, idx, in)) return -1;
        StoreSink sink(*ctx->store, note_id, idx, lang_c ? lang_c : "en", in.first, in.last);
        return section_outputs_pair_run(ctx, in, idx, lang_c, sink, cb, user);
    } catch (...) {
        return -1;
    }
}

char *sve_section_outputs_pair_text(sve_ctx *ctx, const char *pieces_json, const char *summary_json, int idx,
                                    const char *lang_c, sve_progress_cb cb, void *user) {
    try {
        if (!ctx || !ctx->llm) return nullptr;
        PartInput in;
        if (!part_input_text(pieces_json, summary_json, idx, in)) return nullptr;
        TextSink sink(idx);
        const int r = section_outputs_pair_run(ctx, in, idx, lang_c, sink, cb, user);
        if (r == -1) return nullptr;
        sve::json out = sve::json::object();
        static const char *kNames[2] = {"notes", "questions"};
        for (int i = 0; i < 2; ++i)
            out[kNames[i]] = sink.got[i] ? sve::json{{"text", sink.text[i]}, {"cut", sink.cut[i]}} : sve::json(nullptr);
        out["cancelled"] = r == -2;
        return dup_str(out.dump());
    } catch (...) {
        return nullptr;
    }
}

char *sve_section_outputs_state(sve_ctx *ctx, int64_t note_id, const char *lang_c) {
    try {
        if (!ctx) return nullptr;
        return dup_str(ctx->store->section_outputs_state(note_id, lang_c ? lang_c : "en").dump());
    } catch (...) {
        return nullptr;
    }
}

char *sve_assemble_output(sve_ctx *ctx, int64_t note_id, sve_output_kind kind, const char *lang_c) {
    try {
        sve::SectionOutputKind sk;
        if (!ctx || !section_kind(kind, sk)) return nullptr;
        const sve::json texts = ctx->store->current_section_outputs(note_id, (int)kind, lang_c ? lang_c : "en");
        if (!texts.is_array()) return nullptr;  // a part is missing
        if (sk == sve::SectionOutputKind::Questions) return dup_str(sve::assemble_questions(texts));
        return dup_str(sve::assemble_notes(ctx->store->sections(note_id), texts));
    } catch (...) {
        return nullptr;
    }
}

char *sve_assemble_output_with_gaps(sve_ctx *ctx, int64_t note_id, sve_output_kind kind, const char *lang_c,
                                    const char *placeholder) {
    try {
        sve::SectionOutputKind sk;
        if (!ctx || !section_kind(kind, sk)) return nullptr;
        const sve::json texts = ctx->store->current_section_outputs(note_id, (int)kind, lang_c ? lang_c : "en", true);
        if (!texts.is_array()) return nullptr;  // no part done
        if (sk == sve::SectionOutputKind::Questions) return dup_str(sve::assemble_questions(texts));
        const std::string ph = placeholder ? placeholder : "";
        return dup_str(sve::assemble_notes(ctx->store->sections(note_id), texts, &ph));
    } catch (...) {
        return nullptr;
    }
}

int sve_section_outputs_clear(sve_ctx *ctx, int64_t note_id, int kind, int idx) {
    try {
        if (!ctx) return -1;
        return ctx->store->clear_section_outputs(note_id, kind, idx);
    } catch (...) {
        return -1;
    }
}

char *sve_title_from_sections(sve_ctx *ctx, int64_t note_id, const char *lang_c) {
    try {
        if (!ctx || !ctx->llm) return nullptr;
        sve::json ch = sve::merge_chapters(ctx->store->sections(note_id));
        if (!ch.is_array() || ch.empty()) return nullptr;
        std::string excerpt;
        for (const auto &c : ch) {
            if (!c.contains("title") || !c["title"].is_string()) continue;
            excerpt += c["title"].get<std::string>() + ".\n";
            if (excerpt.size() > 1200) break;
        }
        return sve_suggest_title(ctx, excerpt.c_str(), lang_c);  // trims to 1200 bytes at a character boundary
    } catch (...) {
        return nullptr;
    }
}

char *sve_notes_material(const char *pieces_json) {
    try {
        if (!pieces_json) return nullptr;
        const sve::json arr = sve::json::parse(pieces_json, nullptr, false);
        if (!arr.is_array()) return nullptr;
        return dup_str(sve::notes_material(arr));
    } catch (...) {
        return nullptr;
    }
}

char *sve_plan_sections(const char *pieces_json, int64_t end_sec_of_last, int is_final) {
    try {
        return dup_str(sve::plan_sections_json(pieces_json ? pieces_json : "[]", end_sec_of_last, is_final != 0));
    } catch (...) {
        return nullptr;
    }
}

char *sve_chapters_json(sve_ctx *ctx, int64_t note_id) {
    try {
        if (!ctx) return nullptr;
        return dup_str(sve::merge_chapters(ctx->store->sections(note_id)).dump(-1, ' ', false, sve::json::error_handler_t::replace));
    } catch (...) {
        return nullptr;
    }
}

char *sve_ask(sve_ctx *ctx, const char *context_text, const char *question, const char *lang_c,
              sve_progress_cb cb, void *user) {
    if (!ctx || !ctx->llm || !context_text || !question) return nullptr;
    sve::Llm &llm = *ctx->llm;
    std::string lang = lang_c ? lang_c : "en";
    const std::string sys = sve::context_system(*ctx->store, sve::system_prompt(lang));
    const int max_out = std::max(96, std::min(300, llm.n_ctx / 6));
    std::string material;
    bool fb = false;
    if (!reduce_to_fit(llm, sys, context_text, llm.n_ctx - max_out - 300 - llm.count_tokens(sys), material, fb, cb, user)) return nullptr;
    std::string prompt = llm.apply_template(sys, sve::ask_prompt(material, question));
    std::string out;
    if (!llm.complete(prompt, max_out, out, cb, user, "final", 0, 1)) return nullptr;
    return dup_str(strip_thinking(out));
}

char *sve_suggest_title(sve_ctx *ctx, const char *transcript, const char *lang_c) {
    if (!ctx || !ctx->llm || !transcript) return nullptr;
    std::string excerpt(transcript);
    if (excerpt.size() > 1200) {
        excerpt.resize(1200);
        while (!excerpt.empty() && (static_cast<unsigned char>(excerpt.back()) & 0xC0) == 0x80) excerpt.pop_back();
    }
    std::string prompt = ctx->llm->apply_template(sve::context_system(*ctx->store, sve::system_prompt(lang_c ? lang_c : "en"), false, &excerpt),
                                                  sve::title_prompt(excerpt));
    std::string out;
    if (!ctx->llm->complete(prompt, 24, out, nullptr, nullptr, "final", 0, 1)) return nullptr;
    out = strip_thinking(out);
    // first line only, strip quotes
    size_t nl = out.find('\n');
    if (nl != std::string::npos) out.resize(nl);
    for (const char *lead : {"Title:", "title:", "Title -", "Title"}) {
        if (out.rfind(lead, 0) == 0) { out.erase(0, strlen(lead)); break; }
    }
    while (!out.empty() && (out.front() == '"' || out.front() == '\'' || out.front() == ' ')) out.erase(0, 1);
    while (!out.empty() && (out.back() == '"' || out.back() == '\'' || out.back() == ' ' || out.back() == '.')) out.pop_back();
    return dup_str(out);
}


// ---------------------------------------------------------------- audio understanding (Gemma 4 via mtmd)
#ifdef SVE_WITH_MTMD
// Loads the projector from `path` with `n_threads` encoder threads (any projector loaded is freed first).
static int load_projector(sve_ctx *ctx, const std::string &path, int n_threads) {
    sve_unload_audio(ctx);
    mtmd_context_params mp = mtmd_context_params_default();
    // Same backend as the model: a CPU-only load (n_gpu_layers 0) keeps the encoder off the GPU too,
    // so transcription keeps running while the phone is locked (iOS refuses background GPU work).
    mp.use_gpu = ctx->llm->n_gpu_layers > 0;
    mp.print_timings = false;
    mp.n_threads = n_threads;
    mp.warmup = false;
    ctx->mtmd = mtmd_init_from_file(path.c_str(), ctx->llm->model, mp);
    if (!ctx->mtmd) { sve_logf(2, "failed to load audio projector %s", path.c_str()); return -2; }
    if (!mtmd_support_audio(ctx->mtmd)) {
        sve_logf(2, "projector %s has no audio encoder", path.c_str());
        sve_unload_audio(ctx);
        return -3;
    }
    ctx->mmproj_path = path;
    ctx->audio_threads = n_threads;
    return 0;
}

int sve_load_audio(sve_ctx *ctx, const char *mmproj_path) {
    if (!ctx || !ctx->llm || !mmproj_path) return -1;
    // The audio encoder is a short compute-bound burst per piece: it gets the prefill's threads (more
    // than decoding). Mac CPU: encode 1.04 s -> 0.59 s per 28 s piece, transcripts identical
    // (docs/superpowers/2026-11-09-piece-speed.md). sve_set_audio_threads changes them later.
    return load_projector(ctx, mmproj_path, sve::audio_threads(0, ctx->llm->n_threads_batch));
}

int sve_set_audio_threads(sve_ctx *ctx, int n) {
    if (!ctx || !ctx->llm || !ctx->mtmd) return -1;
    const int want = sve::audio_threads(n, ctx->llm->n_threads_batch);
    if (!sve::audio_threads_need_reload(ctx->audio_threads, want)) return 0;
    const int old = ctx->audio_threads;
    const std::string path = ctx->mmproj_path;
    const auto t0 = std::chrono::steady_clock::now();
    const int rc = load_projector(ctx, path, want);
    const long ms = (long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    if (rc == 0) {
        sve_logf(0, "audio threads %d (reload %ld ms)", want, ms);
        return 0;
    }
    sve_logf(2, "audio threads %d: reload failed after %ld ms; loading it again with %d", want, ms, old);
    if (load_projector(ctx, path, old) != 0)
        sve_logf(2, "audio threads %d: the retry failed too; the audio projector is unloaded (the next load loads it)", old);
    return -2;
}

void sve_unload_audio(sve_ctx *ctx) {
    if (ctx && ctx->mtmd) { mtmd_free(ctx->mtmd); ctx->mtmd = nullptr; ctx->audio_threads = 0; }
}

int sve_audio_loaded(const sve_ctx *ctx) { return ctx && ctx->mtmd ? 1 : 0; }

// SVE_TIMING=1 logs where each audio piece's time goes (off in the app).
static bool timing_on() {
    const char *v = getenv("SVE_TIMING");
    return v && strcmp(v, "1") == 0;
}

// Transcribes one piece of 16 kHz mono audio with the loaded projector. False on failure/cancel.
static bool transcribe_piece(sve_ctx *ctx, const float *samples, size_t n, const std::string &prev,
                             const std::string &vocab, sve_progress_cb cb, void *user, int done, int total,
                             std::string &out, double *conf = nullptr, bool second_hearing = false) {
    sve::Llm &llm = *ctx->llm;
    const std::string prompt = llm.apply_template(sve::kTranscriberSystem,
                                                  sve::transcription_instruction(mtmd_get_marker(ctx->mtmd), vocab, prev));
    llama_memory_clear(llama_get_memory(llm.ctx), true);
    mtmd_bitmap *bmp = mtmd_bitmap_init_from_audio(n, samples);
    mtmd_input_chunks *chunks = mtmd_input_chunks_init();
    mtmd_input_text text{};
    text.text = prompt.c_str();
    text.text_len = prompt.size();
    text.add_special = true;
    text.parse_special = true;
    const mtmd_bitmap *bmps[1] = {bmp};
    int rc = mtmd_tokenize(ctx->mtmd, chunks, &text, bmps, 1);
    if (rc == 0 && g_sve_token_log)
        for (size_t i = 0; i < mtmd_input_chunks_size(chunks); ++i) {
            size_t nt = 0;
            const llama_token *t = mtmd_input_chunk_get_tokens_text(mtmd_input_chunks_get(chunks, i), &nt);
            if (t) sve_log_tokens('i', t, nt);
        }
    // The steps of mtmd_helper_eval_chunks, one chunk at a time, so the audio encode and the
    // prefill can be timed apart (SVE_TIMING=1).
    using clk = std::chrono::steady_clock;
    auto secs = [](clk::time_point a, clk::time_point b) { return std::chrono::duration<double>(b - a).count(); };
    double t_encode = 0, t_prefill = 0;
    llama_pos n_past = 0;
    const int32_t n_batch = (int32_t)llama_n_batch(llm.ctx);
    const size_t n_chunks = rc == 0 ? mtmd_input_chunks_size(chunks) : 0;
    for (size_t i = 0; rc == 0 && i < n_chunks; ++i) {
        const mtmd_input_chunk *chunk = mtmd_input_chunks_get(chunks, i);
        const bool logits_last = i + 1 == n_chunks;
        const auto t0 = clk::now();
        if (mtmd_input_chunk_get_type(chunk) == MTMD_INPUT_CHUNK_TYPE_TEXT) {
            rc = mtmd_helper_eval_chunk_single(ctx->mtmd, llm.ctx, chunk, n_past, 0, n_batch, logits_last, &n_past);
            t_prefill += secs(t0, clk::now());
            continue;
        }
        rc = mtmd_encode_chunk(ctx->mtmd, chunk);
        const auto t1 = clk::now();
        t_encode += secs(t0, t1);
        if (rc == 0)
            rc = mtmd_helper_decode_image_chunk(ctx->mtmd, llm.ctx, chunk, mtmd_get_output_embd(ctx->mtmd), n_past, 0,
                                                n_batch, &n_past, nullptr, nullptr);
        t_prefill += secs(t1, clk::now());
    }
    mtmd_input_chunks_free(chunks);
    mtmd_bitmap_free(bmp);
    if (rc != 0) {
        llm.decode_failed = true;   // on the GPU: the next run loads the model again (sve_llm_needs_reload)
        sve_logf(2, "audio piece %d/%d failed to evaluate (%d)", done + 1, total, rc);
        return false;
    }
    out.clear();
    int room = std::min(768, llm.n_ctx - (int)n_past - 4);
    int n_out = 0;
    const auto t2 = clk::now();
    const bool ok = llm.generate(room, true, out, cb, user, "chunk", done, total, conf, &n_out, second_hearing);
    if (timing_on())
        sve_logf(0, "piece timing: encode %.1f s, prefill %.1f s (%d tok), decode %.1f s (%d tok)", t_encode, t_prefill,
                 (int)n_past, secs(t2, clk::now()), n_out);
    if (!ok) return false;
    out = strip_thinking(out);
    return true;
}

// One piece, heard a second time when its conf is low (sve_should_retry_piece): same samples, same prompt
// (vocabulary line, previous text), so the same planner cut and context; the second hearing samples
// (Llm::generate second_hearing) because a greedy repeat of the same input is the same text. The text with the
// higher conf is kept; at most one retry. A second hearing that fails keeps the first, unless it was a cancel.
// label: the piece number in the log (sve_transcribe_next_piece: the piece's start second). live: a recording's
// piece (sve_transcribe_next_piece with is_final 0), never heard twice (R-N7-g). reheard: set when it was.
static bool hear_piece(sve_ctx *ctx, const float *samples, size_t n, const std::string &prev, const std::string &vocab,
                       sve_progress_cb cb, void *user, int done, int total, int label, std::string &out, double &conf,
                       bool live, bool &reheard) {
    conf = std::nan("");
    reheard = false;
    if (!transcribe_piece(ctx, samples, n, prev, vocab, cb, user, done, total, out, &conf)) return false;
    if (!sve_rehear_piece(conf, live)) return true;
    reheard = true;
    std::string again;
    double again_conf = std::nan("");
    if (!transcribe_piece(ctx, samples, n, prev, vocab, cb, user, done, total, again, &again_conf, true))
        return !(cb && cb("chunk", done, total, user));
    const bool second = again_conf > conf;
    sve_logf(0, "piece %d conf %.2f: transcribed again (%.2f), kept the %s hearing", label, conf, again_conf,
             second ? "second" : "first");
    if (second) { out = again; conf = again_conf; }
    return true;
}

// The shared core of sve_transcribe_next_piece and sve_transcribe_span: these samples with the transcription prompt,
// the vocabulary line (words the person fixed earlier) and the previous text, heard twice when conf is low.
static bool hear_samples(sve_ctx *ctx, const float *samples, size_t n, const char *prev_text, sve_progress_cb cb,
                         void *user, int label, bool live, std::string &text, double &conf, bool &reheard) {
    const std::string vocab = sve::transcription_vocab_line(*ctx->store, 400);
    return hear_piece(ctx, samples, n, prev_text ? prev_text : "", vocab, cb, user, 0, 1, label, text, conf, live,
                      reheard);
}

static bool audio_ready(sve_ctx *ctx) {
    if (!ctx || !ctx->llm || !ctx->mtmd) return false;
    const int rate = mtmd_get_audio_sample_rate(ctx->mtmd);
    if (rate > 0 && rate != (int)sve::kAudioRate) { sve_logf(2, "projector wants %d Hz audio", rate); return false; }
    return true;
}

struct TimedPiece { int64_t start_sec; std::string text; double conf; };

// Transcribes in ~28 s pieces; each result is (piece start in whole seconds, text). False on failure/cancel.
static bool transcribe_pieces(sve_ctx *ctx, const char *wav_path, sve_progress_cb cb, void *user,
                              std::vector<TimedPiece> &result) {
    if (!wav_path || !audio_ready(ctx)) return false;
    std::vector<float> pcm;
    if (!read_wav16k(wav_path, pcm)) { sve_logf(2, "could not read %s", wav_path); return false; }

    std::vector<std::pair<size_t, size_t>> pieces;
    for (size_t pos = 0;;) {
        sve::PiecePlan plan = sve::plan_next_piece(pcm, 0, pos, pcm.size(), true);
        if (plan.kind != sve::PiecePlan::piece) break;
        pieces.push_back({plan.start, plan.stop});
        pos = plan.stop;
    }

    // Words the person fixed earlier (spec §7.4), bounded so the audio prompt stays small.
    const std::string vocab = sve::transcription_vocab_line(*ctx->store, 400);
    std::string prev;
    const int total = (int)pieces.size();
    int reheard = 0;
    for (int i = 0; i < total; ++i) {
        if (cb && cb("chunk", i, total, user)) return false;
        std::string piece;
        double conf = std::nan("");
        bool again = false;
        if (!hear_piece(ctx, pcm.data() + pieces[i].first, pieces[i].second - pieces[i].first, prev, vocab,
                        cb, user, i, total, i + 1, piece, conf, false, again)) return false;
        reheard += again ? 1 : 0;
        sve_logf(0, "piece %d/%d conf %.4f", i + 1, total, conf);
        if (!piece.empty()) { result.push_back({(int64_t)(pieces[i].first / sve::kAudioRate), piece, conf}); prev = piece; }
    }
    sve_logf(0, "re-heard %d of %d pieces", reheard, total);
    if (cb) cb("final", total, total, user);
    return true;
}

int g_sve_section_max_out = 512;  // 3 to 6 points (N7 rev 1: back from 1024 for up to 10)
bool g_sve_force_greedy = false;
FILE *g_sve_token_log = nullptr;

void sve_log_tokens(char tag, const int32_t *ids, size_t n) {
    if (!g_sve_token_log) return;
    for (size_t i = 0; i < n; ++i) fprintf(g_sve_token_log, "%c %d\n", tag, (int)ids[i]);
}

char *sve_section_summarize(sve_ctx *ctx, const char *pieces_json, const char *lang_c, sve_progress_cb cb, void *user) {
    try {
        if (!ctx || !ctx->llm || !pieces_json) return nullptr;
        sve::json arr = sve::json::parse(pieces_json, nullptr, false);
        if (!arr.is_array()) return nullptr;
        // Pieces heard with conf < 0.80 are marked [unclear] in the prompt only (N7 rev 1); the fallback and the
        // vocabulary check use the plain text.
        const sve::SectionInput in = sve::section_input(arr);
        if (in.plain.empty()) return nullptr;  // no piece has text: nothing to summarise
        sve::Llm &llm = *ctx->llm;
        const std::string lang = lang_c ? lang_c : "en";
        const int kMaxOut = g_sve_section_max_out;
        // Only the saved words this section says, in the system prompt and the vocabulary line (N7 rev 1).
        const std::string sys = sve::context_system(*ctx->store, sve::system_prompt(lang), false, &in.said);
        const std::string prompt = llm.apply_template(
            sys, sve::section_prompt(in.marked, lang, sve::spelling_vocab_line(*ctx->store, 400, &in.said)));
        const std::vector<std::string> vocab = sve::saved_vocab(*ctx->store);
        const sve::SectionPieces &pieces = in.plain;
        const std::vector<int64_t> &starts = in.starts;
        auto dump = [](const sve::json &j) { return dup_str(j.dump(-1, ' ', false, sve::json::error_handler_t::replace)); };
        // A section that cannot fit the context with room to answer goes straight to the fallback.
        if (llm.count_tokens(prompt) + kMaxOut + 16 > llm.n_ctx) return dump(sve::fallback_section_summary(pieces));
        for (int attempt = 0; attempt < 2; ++attempt) {  // sampled, then greedy
            std::string out;
            // Only a cancel from the host ends the call; any other failure goes on to the retry, then the fallback.
            struct Watch { sve_progress_cb cb; void *user; bool cancelled; } w{cb, user, false};
            auto watch = [](const char *ph, int d, int t, void *u) -> int {
                Watch *x = static_cast<Watch *>(u);
                if (x->cb && x->cb(ph, d, t, x->user)) x->cancelled = true;
                return x->cancelled ? 1 : 0;
            };
            if (!llm.complete(prompt, kMaxOut, out, watch, &w, "section", attempt, 2, attempt == 1)) {
                if (w.cancelled) return nullptr;
                sve_logf(1, "section summary attempt %d failed", attempt + 1);
                continue;
            }
            sve::json j;
            if (sve::parse_section_summary(strip_thinking(out), starts, j)) {
                // A saved word the section never says is not one of its terms, hints or points (N7 rev 1).
                if (const int n = sve::drop_unsaid_vocab(j, vocab, in.said)) {
                    sve_logf(1, "section summary: %d term(s)/hint(s)/point(s) with a saved word not said here dropped", n);
                    if (j["points"].empty()) return dump(sve::fallback_section_summary(pieces));
                }
                return dump(j);
            }
            sve_logf(1, "section summary attempt %d did not parse", attempt + 1);
        }
        return dump(sve::fallback_section_summary(pieces));
    } catch (const std::exception &e) {
        sve_logf(2, "section summary failed: %s", e.what());
        return nullptr;
    } catch (...) {
        return nullptr;
    }
}

char *sve_transcribe_next_piece(sve_ctx *ctx, const char *wav_path, int64_t start_sample, int is_final,
                                const char *prev_text, sve_progress_cb cb, void *user) {
    try {
        if (!wav_path || start_sample < 0 || !audio_ready(ctx)) return nullptr;
        sve::WavInfo info;
        switch (sve::wav_probe(wav_path, info)) {
            case sve::WavProbe::ok: break;
            case sve::WavProbe::incomplete:
            case sve::WavProbe::bad:
                // While recording, a header not (fully) written yet, or still zeroed, is not an error.
                if (!is_final) return dup_str("{\"status\":\"wait\"}");
                sve_logf(2, "%s is not a readable 16 kHz 16-bit WAV", wav_path);
                return nullptr;
        }
        const size_t start = (size_t)start_sample, total = (size_t)info.frames;
        // Nothing to read until 28 s + 3 s are there (the planner's rule; checked first to skip the read).
        if (!is_final && (total <= start || total - start < sve::kPieceTarget + sve::kPieceSearch))
            return dup_str("{\"status\":\"wait\"}");
        std::vector<float> pcm;
        if (start < total && !sve::wav_read(wav_path, info, start, sve::kPieceTarget, pcm)) {
            sve_logf(2, "could not read %s", wav_path);
            return nullptr;
        }
        // A short read (I/O error) must not be planned as silence: plan with what was read.
        const size_t total_read = pcm.size() < std::min(total - std::min(start, total), sve::kPieceTarget) ? start + pcm.size() : total;
        sve::PiecePlan plan = sve::plan_next_piece(pcm, start, start, total_read, is_final != 0);
        if (plan.kind == sve::PiecePlan::wait) return dup_str("{\"status\":\"wait\"}");
        if (plan.kind == sve::PiecePlan::end) {
            auto h = ctx->hearings.find(wav_path);
            if (h != ctx->hearings.end()) {
                sve_logf(0, "re-heard %d of %d pieces", h->second.second, h->second.first);
                ctx->hearings.erase(h);
            }
            return dup_str("{\"status\":\"end\"}");
        }
        if (cb && cb("chunk", 0, 1, user)) return nullptr;
        std::string text;
        double conf = std::nan("");
        // Live (is_final 0): heard once; after Stop the pieces still to do may be heard twice (R-N7-g).
        bool again = false;
        if (!hear_samples(ctx, pcm.data() + (plan.start - start), plan.stop - plan.start, prev_text, cb, user,
                          (int)(plan.start / sve::kAudioRate), is_final == 0, text, conf, again))
            return nullptr;
        auto &h = ctx->hearings[wav_path];
        h.first += 1;
        h.second += again ? 1 : 0;
        sve::json j = {{"status", "piece"}, {"start_sec", (int64_t)(plan.start / sve::kAudioRate)},
                       {"end_sample", (int64_t)plan.stop}, {"text", text}, {"conf", conf}};
        return dup_str(j.dump(-1, ' ', false, sve::json::error_handler_t::replace));
    } catch (const std::exception &e) {
        sve_logf(2, "transcribe failed: %s", e.what());
        return nullptr;
    } catch (...) {
        sve_logf(2, "transcribe failed");
        return nullptr;
    }
}

char *sve_transcribe_span(sve_ctx *ctx, const char *wav_path, int64_t start_sample, int64_t end_sample,
                          const char *prev_text, sve_progress_cb cb, void *user) {
    try {
        if (!wav_path || start_sample < 0 || end_sample <= start_sample ||
            end_sample - start_sample > (int64_t)sve::kPieceTarget || !audio_ready(ctx))
            return nullptr;
        sve::WavInfo info;
        if (sve::wav_probe(wav_path, info) != sve::WavProbe::ok) {
            sve_logf(2, "%s is not a readable 16 kHz 16-bit WAV", wav_path);
            return nullptr;
        }
        const uint64_t n = (uint64_t)(end_sample - start_sample);
        if ((uint64_t)end_sample > info.frames) { sve_logf(2, "span ends past the end of %s", wav_path); return nullptr; }
        std::vector<float> pcm;
        if (!sve::wav_read(wav_path, info, (uint64_t)start_sample, n, pcm) || pcm.size() != n) {
            sve_logf(2, "could not read %s", wav_path);
            return nullptr;
        }
        if (cb && cb("chunk", 0, 1, user)) return nullptr;
        std::string text;
        double conf = std::nan("");
        bool again = false;   // after Stop (Whisper is unloaded first), so a low conf is heard twice
        if (!hear_samples(ctx, pcm.data(), pcm.size(), prev_text, cb, user, (int)(start_sample / sve::kAudioRate),
                          false, text, conf, again))
            return nullptr;
        sve::json j = {{"text", text}, {"conf", conf}};
        return dup_str(j.dump(-1, ' ', false, sve::json::error_handler_t::replace));
    } catch (const std::exception &e) {
        sve_logf(2, "transcribe span failed: %s", e.what());
        return nullptr;
    } catch (...) {
        sve_logf(2, "transcribe span failed");
        return nullptr;
    }
}

char *sve_transcribe_audio(sve_ctx *ctx, const char *wav_path, sve_progress_cb cb, void *user) {
    try {
        std::vector<TimedPiece> pieces;
        if (!transcribe_pieces(ctx, wav_path, cb, user, pieces)) return nullptr;
        std::string out;
        for (const auto &p : pieces) { out += p.text; out += "\n"; }
        return dup_str(strip_thinking(out));
    } catch (const std::exception &e) {
        sve_logf(2, "transcribe failed: %s", e.what());
        return nullptr;
    } catch (...) {
        sve_logf(2, "transcribe failed");
        return nullptr;
    }
}

char *sve_transcribe_audio_pieces(sve_ctx *ctx, const char *wav_path, sve_progress_cb cb, void *user) {
    try {
        std::vector<TimedPiece> pieces;
        if (!transcribe_pieces(ctx, wav_path, cb, user, pieces)) return nullptr;
        sve::json arr = sve::json::array();
        for (const auto &p : pieces) arr.push_back({{"start_sec", p.start_sec}, {"text", p.text}, {"conf", p.conf}});
        return dup_str(arr.dump());
    } catch (const std::exception &e) {
        sve_logf(2, "transcribe failed: %s", e.what());
        return nullptr;
    } catch (...) {
        sve_logf(2, "transcribe failed");
        return nullptr;
    }
}
#else
int sve_load_audio(sve_ctx *, const char *) { return -1; }
void sve_unload_audio(sve_ctx *) {}
int sve_audio_loaded(const sve_ctx *) { return 0; }
int sve_set_audio_threads(sve_ctx *, int) { return -1; }
char *sve_transcribe_audio(sve_ctx *, const char *, sve_progress_cb, void *) { return nullptr; }
char *sve_transcribe_audio_pieces(sve_ctx *, const char *, sve_progress_cb, void *) { return nullptr; }
char *sve_transcribe_next_piece(sve_ctx *, const char *, int64_t, int, const char *, sve_progress_cb, void *) { return nullptr; }
#endif

// ---------------------------------------------------------------- notes API
static const char *column_for(sve_output_kind k) {
    switch (k) {
        case SVE_OUT_SHORT_SUMMARY: return "short_summary";
        case SVE_OUT_DETAILED_NOTES: return "detailed_notes";
        case SVE_OUT_REVISION_QUESTIONS: return "revision_questions";
    }
    return nullptr;
}

int64_t sve_note_create(sve_ctx *ctx, const char *title, const char *lang, const char *audio_path,
                        const char *transcript, int64_t duration_seconds) {
    if (!ctx) return -1;
    return ctx->store->create(title ? title : "Untitled", lang ? lang : "en", audio_path, transcript, duration_seconds);
}

int sve_note_set_transcript(sve_ctx *ctx, int64_t id, const char *t) {
    return ctx && ctx->store->set_text_column(id, "transcript", t) ? 0 : -1;
}
int sve_note_set_output(sve_ctx *ctx, int64_t id, sve_output_kind kind, const char *text) {
    const char *col = column_for(kind);
    return ctx && col && ctx->store->set_text_column(id, col, text) ? 0 : -1;
}
int sve_note_set_title(sve_ctx *ctx, int64_t id, const char *title) {
    return ctx && title && ctx->store->set_text_column(id, "title", title) ? 0 : -1;
}
int sve_note_delete(sve_ctx *ctx, int64_t id) { return ctx && ctx->store->remove(id) ? 0 : -1; }
int sve_note_clear_audio(sve_ctx *ctx, int64_t id) { return ctx && ctx->store->set_text_column(id, "audio_path", nullptr) ? 0 : -1; }
int sve_note_set_audio_path(sve_ctx *ctx, int64_t id, const char *audio_path) {
    return ctx && audio_path && *audio_path && ctx->store->set_text_column(id, "audio_path", audio_path) ? 0 : -1;
}
int sve_note_clear_transcript(sve_ctx *ctx, int64_t id) { return ctx && ctx->store->clear_transcript(id) ? 0 : -1; }

static void json_opt(std::string &o, const char *key, const std::optional<std::string> &v) {
    o += "\"";
    o += key;
    o += "\":";
    if (v) { o += "\""; o += sve::json_escape(*v); o += "\""; } else o += "null";
}

char *sve_note_get_json(sve_ctx *ctx, int64_t id) {
    if (!ctx) return nullptr;
    auto r = ctx->store->get(id);
    if (!r) return nullptr;
    std::string o = "{\"id\":" + std::to_string(r->id) + ",\"title\":\"" + sve::json_escape(r->title) +
                    "\",\"lang\":\"" + sve::json_escape(r->lang) + "\",\"created_at\":" + std::to_string(r->created_at) +
                    ",\"duration_seconds\":" + std::to_string(r->duration_seconds) + ",";
    json_opt(o, "audio_path", r->audio_path); o += ",";
    json_opt(o, "transcript", r->transcript); o += ",";
    json_opt(o, "short_summary", r->short_summary); o += ",";
    json_opt(o, "detailed_notes", r->detailed_notes); o += ",";
    json_opt(o, "revision_questions", r->revision_questions);
    o += "}";
    return dup_str(o);
}

char *sve_notes_list_json(sve_ctx *ctx, const char *query, int limit) {
    if (!ctx) return nullptr;
    auto rows = ctx->store->list(query ? query : "", limit > 0 ? limit : 200);
    std::string o = "[";
    for (size_t i = 0; i < rows.size(); ++i) {
        const auto &r = rows[i];
        if (i) o += ",";
        o += "{\"id\":" + std::to_string(r.id) + ",\"title\":\"" + sve::json_escape(r.title) + "\",\"lang\":\"" +
             sve::json_escape(r.lang) + "\",\"created_at\":" + std::to_string(r.created_at) + ",\"duration_seconds\":" +
             std::to_string(r.duration_seconds) + ",\"has_audio\":" + (r.has_audio ? "true" : "false") +
             ",\"preview\":\"" + sve::json_escape(r.preview) + "\"}";
    }
    o += "]";
    return dup_str(o);
}

char *sve_notes_audio_older_than_json(sve_ctx *ctx, int max_age_days) {
    if (!ctx) return nullptr;
    auto rows = ctx->store->audio_older_than(max_age_days);
    std::string o = "[";
    for (size_t i = 0; i < rows.size(); ++i) {
        if (i) o += ",";
        o += "{\"id\":" + std::to_string(rows[i].first) + ",\"audio_path\":\"" + sve::json_escape(rows[i].second) + "\"}";
    }
    o += "]";
    return dup_str(o);
}

int sve_setting_set(sve_ctx *ctx, const char *key, const char *value) {
    return ctx && key && value && ctx->store->setting_set(key, value) ? 0 : -1;
}
char *sve_setting_get(sve_ctx *ctx, const char *key) {
    if (!ctx || !key) return nullptr;
    auto v = ctx->store->setting_get(key);
    return v ? dup_str(*v) : nullptr;
}

void sve_free_string(char *s) { free(s); }

// ---------------------------------------------------------------- whisper
#ifdef SVE_WITH_WHISPER

int sve_load_asr(sve_ctx *ctx, const char *path, int use_gpu, int n_threads) {
    if (!ctx || !path) return -1;
    sve_unload_asr(ctx);
    whisper_context_params cp = whisper_context_default_params();
    cp.use_gpu = use_gpu != 0;
    cp.flash_attn = true;
    // No default state: asr_state is the only one, so the language check and the decode share it.
    ctx->whisper = whisper_init_from_file_with_params_no_state(path, cp);
    if (!ctx->whisper) return -2;
    ctx->asr_state = whisper_init_state(ctx->whisper);
    if (!ctx->asr_state) { whisper_free(ctx->whisper); ctx->whisper = nullptr; return -2; }
    // The decode of a piece reuses its language check's encoder run (SVE patch in whisper.cpp). SVE_WHISPER_REENCODE=1
    // turns that off, to compare the text both ways on the phone.
    const char *reencode = getenv("SVE_WHISPER_REENCODE");
    whisper_set_encode_reuse_with_state(ctx->asr_state, !(reencode && reencode[0] == '1'));
    unsigned hw = std::thread::hardware_concurrency();
    ctx->whisper_threads = n_threads > 0 ? n_threads : (int)std::max(1u, std::min(hw, 4u));
    return 0;
}

void sve_unload_asr(sve_ctx *ctx) {
    if (!ctx) return;
    if (ctx->asr_state) { whisper_free_state(ctx->asr_state); ctx->asr_state = nullptr; }
    if (ctx->whisper) { whisper_free(ctx->whisper); ctx->whisper = nullptr; }
}

int sve_asr_loaded(sve_ctx *ctx) { return ctx && ctx->whisper && ctx->asr_state ? 1 : 0; }

// A cancel from the host's progress callback stops whisper_full (checked after each encoder and decoder pass).
struct WhisperWatch { sve_progress_cb cb; void *user; int done, total; bool cancelled; };
static bool whisper_watch_abort(void *u) {
    auto *w = static_cast<WhisperWatch *>(u);
    if (!w->cancelled && w->cb && w->cb("chunk", w->done, w->total, w->user)) w->cancelled = true;
    return w->cancelled;
}

// The encoder sized to the piece: 50 frames per second plus 64, at most the model's 1500.
static int whisper_piece_ctx(whisper_context *w, int64_t n) {
    return (int)std::min<int64_t>(std::min(1500, whisper_n_audio_ctx(w)), n * 50 / (int64_t)sve::kAudioRate + 64);
}

// Whether one piece is decoded by Whisper (Telugu) or left to Gemma: Whisper's language detection, the encoder sized
// to the piece, compared on te and en only (sve::lang_choice). A detection that fails goes to Gemma.
static sve::LangChoice whisper_piece_lang(sve_ctx *ctx, const float *s, int64_t n) {
    whisper_context *w = ctx->whisper;
    whisper_state *st = ctx->asr_state;
    const int te = whisper_lang_id("te"), en = whisper_lang_id("en");
    std::vector<float> probs(whisper_lang_max_id() + 1, 0.0f);
    // whisper_lang_auto_detect_with_state runs the encoder itself (with the audio_ctx set here), then one decoder
    // step; a separate whisper_encode_with_state would encode the piece twice.
    if (te < 0 || en < 0 || whisper_pcm_to_mel_with_state(w, st, s, (int)n, ctx->whisper_threads) != 0 ||
        whisper_set_audio_ctx_with_state(w, st, whisper_piece_ctx(w, n)) != 0 ||
        whisper_lang_auto_detect_with_state(w, st, 0, ctx->whisper_threads, probs.data()) < 0) {
        sve_logf(1, "whisper language check failed: piece left to the audio model");
        return sve::LangChoice::Gemma;
    }
    const sve::LangChoice c = sve::lang_choice(probs[te], probs[en]);
    // One line per piece checked, for tuning min_te/margin on the phone (a group stops at its first unsure piece).
    // The three likeliest languages over all of Whisper's, to see where the probability goes on the fine-tune.
    int top[3] = {-1, -1, -1};
    for (int id = 0; id < (int)probs.size(); ++id)
        for (int k = 0; k < 3; ++k)
            if (top[k] < 0 || probs[id] > probs[top[k]]) {
                for (int m = 2; m > k; --m) top[m] = top[m - 1];
                top[k] = id;
                break;
            }
    auto name = [](int id) { const char *l = id >= 0 ? whisper_lang_str(id) : nullptr; return l ? l : "?"; };
    auto p_of = [&](int id) { return id >= 0 ? probs[id] : 0.0f; };
    sve_logf(0, "whisper lang p_te=%.2f p_en=%.2f → %s (piece %.1f s; top %s %.2f, %s %.2f, %s %.2f)", probs[te],
             probs[en], c == sve::LangChoice::Telugu ? "te" : "gemma", n / (double)sve::kAudioRate, name(top[0]),
             p_of(top[0]), name(top[1]), p_of(top[1]), name(top[2]), p_of(top[2]));
    return c;
}

// One Telugu piece: greedy, no timestamps (the fine-tune has none, and with them the first letter breaks), no
// temperature fallback, no context, encoder sized to the piece. Adds its text tokens' probabilities to p_sum/p_n.
static bool whisper_decode_piece(sve_ctx *ctx, const float *s, int64_t n, WhisperWatch &watch, std::string &text,
                                 double &p_sum, int &p_n) {
    whisper_context *w = ctx->whisper;
    whisper_state *st = ctx->asr_state;
    whisper_full_params wp = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    wp.n_threads = ctx->whisper_threads;
    wp.language = "te";
    wp.detect_language = false;
    wp.translate = false;
    wp.no_context = true;
    wp.no_timestamps = true;
    wp.temperature_inc = 0.0f;
    wp.audio_ctx = whisper_piece_ctx(w, n);
    wp.print_progress = false;
    wp.print_realtime = false;
    wp.print_timestamps = false;
    wp.print_special = false;
    wp.abort_callback = whisper_watch_abort;
    wp.abort_callback_user_data = &watch;
    if (whisper_full_with_state(w, st, wp, s, (int)n) != 0 || watch.cancelled) return false;
    text.clear();
    const whisper_token eot = whisper_token_eot(w);   // ids from eot up are special tokens
    const int n_seg = whisper_full_n_segments_from_state(st);
    for (int i = 0; i < n_seg; ++i) {
        if (const char *t = whisper_full_get_segment_text_from_state(st, i)) text += t;
        const int n_tok = whisper_full_n_tokens_from_state(st, i);
        for (int k = 0; k < n_tok; ++k) {
            if (whisper_full_get_token_id_from_state(st, i, k) >= eot) continue;
            p_sum += whisper_full_get_token_p_from_state(st, i, k);
            ++p_n;
        }
    }
    return true;
}

char *sve_whisper_next_piece(sve_ctx *ctx, const char *wav_path, int64_t start_sample, int is_final,
                             sve_progress_cb cb, void *user) {
    try {
        if (!sve_asr_loaded(ctx) || !wav_path || start_sample < 0) return nullptr;
        sve::WavInfo info;
        switch (sve::wav_probe(wav_path, info)) {
            case sve::WavProbe::ok: break;
            case sve::WavProbe::incomplete:
            case sve::WavProbe::bad:
                // While recording, a header not (fully) written yet, or still zeroed, is not an error.
                if (!is_final) return dup_str("{\"status\":\"wait\"}");
                sve_logf(2, "%s is not a readable 16 kHz 16-bit WAV", wav_path);
                return nullptr;
        }
        const uint64_t start = (uint64_t)start_sample, total = info.frames;
        // A group is up to 28 s; the planner waits for 28 s + 3 s, and with more audio after the group it may cut
        // the last piece at a pause, so 31 s are read (checked first to skip the read).
        const uint64_t window = sve::kPieceTarget + sve::kPieceSearch;
        if (!is_final && (total <= start || total - start < window)) return dup_str("{\"status\":\"wait\"}");
        std::vector<float> pcm;
        if (start < total && !sve::wav_read(wav_path, info, start, window, pcm)) {
            sve_logf(2, "could not read %s", wav_path);
            return nullptr;
        }
        // Planned in the window's own sample numbers; a short read (I/O error) is planned with what was read.
        const sve::Group g = sve::plan_group(pcm.data(), (int64_t)pcm.size(), 0, is_final != 0);
        if (g.status == sve::Group::Wait) return dup_str("{\"status\":\"wait\"}");
        if (g.status == sve::Group::End) return dup_str("{\"status\":\"end\"}");
        const int n = (int)g.spans.size();
        auto piece = [&](const std::string &text, double conf, bool needs_gemma) {
            sve::json j = {{"status", "piece"}, {"start_sec", start_sample / (int64_t)sve::kAudioRate},
                           {"end_sample", start_sample + g.end_sample}, {"text", text}, {"conf", conf},
                           {"needs_gemma", needs_gemma}};
            return dup_str(j.dump(-1, ' ', false, sve::json::error_handler_t::replace));
        };
        // Each piece is checked and then decoded at once, so the decode reuses the check's encoder run (the state
        // keeps only the last encode). Any piece not clearly Telugu sends the whole group to the audio model, and the
        // text decoded so far in the group is dropped.
        using clk = std::chrono::steady_clock;
        auto ms_since = [](clk::time_point t) {
            return (long)std::chrono::duration_cast<std::chrono::milliseconds>(clk::now() - t).count();
        };
        std::string joined;
        double p_sum = 0;
        int p_n = 0;
        for (int i = 0; i < n; ++i) {
            if (cb && cb("chunk", i, n, user)) return nullptr;
            const sve::Span &sp = g.spans[i];
            const auto t_lang = clk::now();
            if (whisper_piece_lang(ctx, pcm.data() + sp.start, sp.end - sp.start) == sve::LangChoice::Gemma)
                return piece("", std::nan(""), true);
            const long lang_ms = ms_since(t_lang);
            WhisperWatch watch{cb, user, i, n, false};
            std::string text;
            double s_sum = 0;
            int s_n = 0;
            const int reused_before = whisper_n_encode_reused_from_state(ctx->asr_state);
            const auto t_dec = clk::now();
            if (!whisper_decode_piece(ctx, pcm.data() + sp.start, sp.end - sp.start, watch, text, s_sum, s_n)) {
                if (!watch.cancelled) sve_logf(2, "whisper failed on a piece of %s", wav_path);
                return nullptr;
            }
            // For the device checks: language check and decode timed apart, and whether the encoder run was reused.
            sve_logf(0, "whisper piece %.1f s: lang %ld ms, decode %ld ms, encoder %s",
                     (sp.end - sp.start) / (double)sve::kAudioRate, lang_ms, ms_since(t_dec),
                     whisper_n_encode_reused_from_state(ctx->asr_state) > reused_before ? "reused" : "run again");
            // A piece that is nothing but a repeated word or phrase (a ticking loop) is dropped here, and its tokens
            // (high-probability loop tokens) are left out of the group's conf.
            text = sve::collapse_repeats(text);
            if (text.empty()) continue;
            p_sum += s_sum;
            p_n += s_n;
            if (!joined.empty()) joined += ' ';
            joined += text;
        }
        // Again over the whole group, for a run that goes on across two pieces.
        joined = sve::collapse_repeats(joined);
        return piece(joined, p_n > 0 ? p_sum / p_n : std::nan(""), false);
    } catch (const std::exception &e) {
        sve_logf(2, "whisper transcribe failed: %s", e.what());
        return nullptr;
    } catch (...) {
        sve_logf(2, "whisper transcribe failed");
        return nullptr;
    }
}

char *sve_transcribe_wav(sve_ctx *ctx, const char *wav_path, const char *lang, sve_progress_cb cb, void *user) {
    if (!sve_asr_loaded(ctx) || !wav_path) return nullptr;
    std::vector<float> pcm;
    if (!read_wav16k(wav_path, pcm)) return nullptr;

    whisper_full_params wp = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    wp.n_threads = ctx->whisper_threads;
    wp.print_progress = false;
    wp.print_realtime = false;
    wp.print_timestamps = false;
    wp.print_special = false;
    wp.no_timestamps = true;
    wp.suppress_blank = true;
    wp.language = (lang && *lang) ? lang : "auto";
    wp.detect_language = false;
    // whisper reports progress in percent; we map it onto the chunk counters (done/100).
    struct Prog { sve_progress_cb cb; void *user; } prog{cb, user};
    if (cb) {
        wp.progress_callback_user_data = &prog;
        wp.progress_callback = [](whisper_context *, whisper_state *, int progress, void *u) {
            auto *p = static_cast<Prog *>(u);
            p->cb("chunk", progress, 100, p->user);
        };
        wp.abort_callback_user_data = &prog;
        wp.abort_callback = [](void *u) -> bool {
            auto *p = static_cast<Prog *>(u);
            return p->cb("chunk", -1, 100, p->user) != 0;
        };
    }
    if (whisper_full_with_state(ctx->whisper, ctx->asr_state, wp, pcm.data(), (int)pcm.size()) != 0) return nullptr;

    std::string out;
    int n = whisper_full_n_segments_from_state(ctx->asr_state);
    for (int i = 0; i < n; ++i) {
        const char *t = whisper_full_get_segment_text_from_state(ctx->asr_state, i);
        if (t) { out += t; out += "\n"; }
    }
    if (cb) cb("final", 1, 1, user);
    return dup_str(out);
}

#else  // no whisper in this build

int sve_load_asr(sve_ctx *, const char *, int, int) { return -1; }
void sve_unload_asr(sve_ctx *) {}
int sve_asr_loaded(sve_ctx *) { return 0; }
char *sve_whisper_next_piece(sve_ctx *, const char *, int64_t, int, sve_progress_cb, void *) { return nullptr; }
char *sve_transcribe_wav(sve_ctx *, const char *, const char *, sve_progress_cb, void *) { return nullptr; }

#endif
