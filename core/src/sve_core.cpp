// Summaverick Everyday — portable core implementation.
// Hosts: iOS (today), Android (next), the Summaverick Everyday device (Linux/ARM).
#include "sve_core.h"
#include "sve_internal.h"
#include "sve_prompts.h"
#include "sve_context.h"

#include <algorithm>
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

static bool g_backend_init = false;

const char *sve_version(void) { return SVE_VERSION; }

sve_ctx *sve_init(const char *db_path) {
    if (!g_backend_init) {
        llama_log_set(llama_log_bridge, nullptr);
        llama_backend_init();
        g_backend_init = true;
    }
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
    if (!l->load(gguf_path, n_ctx, n_threads, n_gpu_layers)) return -2;
    ctx->llm = std::move(l);
    return 0;
}

void sve_unload_llm(sve_ctx *ctx) { if (ctx) { sve_unload_audio(ctx); ctx->llm.reset(); } }
int sve_llm_loaded(const sve_ctx *ctx) { return ctx && ctx->llm ? 1 : 0; }


char *sve_generate(sve_ctx *ctx, const char *transcript, sve_output_kind kind, const char *lang_c,
                   sve_progress_cb cb, void *user) {
    if (!ctx || !ctx->llm || !transcript) return nullptr;
    sve::Llm &llm = *ctx->llm;
    std::string lang = lang_c ? lang_c : "en";
    std::string text(transcript);
    if (strip_thinking(text).empty()) return dup_str("");

    // The always block (profile, vocabulary) goes in front of every system prompt (spec §7.3), here
    // without the medicines: notes are free text and must not pick up the profile's doses (spec §4).
    const std::string sys = sve::context_system(*ctx->store, sve::system_prompt(lang), false);
    const int max_out = std::max(160, std::min(700, llm.n_ctx / 4));
    // prompt scaffolding ≈ 250 tokens plus the system prompt (reduce_to_fit floors this)
    const int budget = llm.n_ctx - max_out - 250 - llm.count_tokens(sys);

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
    out = strip_thinking(out);
    // The prompt ended with the first heading/label; put it back if the model continued from it.
    if (!prefix.empty() && out.rfind(prefix, 0) != 0) out = prefix + " " + out;
    return dup_str(out);
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
    std::string prompt = ctx->llm->apply_template(sve::context_system(*ctx->store, sve::system_prompt(lang_c ? lang_c : "en"), false),
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

namespace {
// Minimal WAV reader: 16‑bit PCM, mono or stereo, any sample rate (we require 16 kHz).
bool read_wav16k(const std::string &path, std::vector<float> &pcm) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::vector<char> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (data.size() < 44 || memcmp(data.data(), "RIFF", 4) || memcmp(data.data() + 8, "WAVE", 4)) return false;
    size_t p = 12;
    int channels = 1, rate = 0, bits = 16;
    while (p + 8 <= data.size()) {
        uint32_t sz; memcpy(&sz, data.data() + p + 4, 4);
        if (!memcmp(data.data() + p, "fmt ", 4)) {
            uint16_t ch; memcpy(&ch, data.data() + p + 10, 2); channels = ch;
            uint32_t r; memcpy(&r, data.data() + p + 12, 4); rate = r;
            uint16_t b; memcpy(&b, data.data() + p + 22, 2); bits = b;
        } else if (!memcmp(data.data() + p, "data", 4)) {
            if (bits != 16 || rate != 16000) { sve_logf(2, "wav must be 16 kHz 16‑bit (got %d Hz, %d bit)", rate, bits); return false; }
            const int16_t *s = reinterpret_cast<const int16_t *>(data.data() + p + 8);
            size_t n = std::min<size_t>(sz, data.size() - p - 8) / 2;
            pcm.reserve(n / channels);
            for (size_t i = 0; i + channels <= n; i += channels) {
                float v = 0;
                for (int c = 0; c < channels; ++c) v += s[i + c];
                pcm.push_back(v / (channels * 32768.0f));
            }
            return true;
        }
        p += 8 + sz + (sz & 1);
    }
    return false;
}
} // namespace

// ---------------------------------------------------------------- audio understanding (Gemma 4 via mtmd)
#ifdef SVE_WITH_MTMD
int sve_load_audio(sve_ctx *ctx, const char *mmproj_path) {
    if (!ctx || !ctx->llm || !mmproj_path) return -1;
    sve_unload_audio(ctx);
    mtmd_context_params mp = mtmd_context_params_default();
    mp.use_gpu = true;
    mp.print_timings = false;
    mp.n_threads = ctx->llm->n_threads;
    mp.warmup = false;
    ctx->mtmd = mtmd_init_from_file(mmproj_path, ctx->llm->model, mp);
    if (!ctx->mtmd) { sve_logf(2, "failed to load audio projector %s", mmproj_path); return -2; }
    if (!mtmd_support_audio(ctx->mtmd)) {
        sve_logf(2, "projector %s has no audio encoder", mmproj_path);
        sve_unload_audio(ctx);
        return -3;
    }
    return 0;
}

void sve_unload_audio(sve_ctx *ctx) {
    if (ctx && ctx->mtmd) { mtmd_free(ctx->mtmd); ctx->mtmd = nullptr; }
}

int sve_audio_loaded(const sve_ctx *ctx) { return ctx && ctx->mtmd ? 1 : 0; }

// Transcribes in ~28 s pieces; each result is (piece start in whole seconds, text). False on failure/cancel.
static bool transcribe_pieces(sve_ctx *ctx, const char *wav_path, sve_progress_cb cb, void *user,
                              std::vector<std::pair<int64_t, std::string>> &result) {
    if (!ctx || !ctx->llm || !ctx->mtmd || !wav_path) return false;
    sve::Llm &llm = *ctx->llm;
    std::vector<float> pcm;
    if (!read_wav16k(wav_path, pcm)) { sve_logf(2, "could not read %s", wav_path); return false; }
    const int rate = mtmd_get_audio_sample_rate(ctx->mtmd);
    if (rate > 0 && rate != 16000) { sve_logf(2, "projector wants %d Hz audio", rate); return false; }

    // Gemma 4 takes at most ~30 s per clip. Cut at the quietest point near 28 s so words are not split.
    const size_t target = 28 * 16000, search = 3 * 16000, win = 1600;
    std::vector<std::pair<size_t, size_t>> pieces;
    for (size_t pos = 0; pos < pcm.size();) {
        size_t end = std::min(pcm.size(), pos + target);
        if (end < pcm.size()) {
            size_t best = end; float best_e = 1e30f;
            for (size_t c = end - search; c + win <= end; c += win / 2) {
                float e = 0;
                for (size_t k = c; k < c + win; ++k) e += pcm[k] * pcm[k];
                if (e < best_e) { best_e = e; best = c + win / 2; }
            }
            end = best;
        }
        if (end - pos >= 8000) pieces.push_back({pos, end}); // skip slivers under 0.5 s
        pos = end;
    }

    const std::string sys =
        "You are a precise transcriber. Speakers often mix languages, for example Telugu with English words.";
    // Words the person fixed earlier (spec §7.4), bounded so the audio prompt stays small.
    const std::string vocab = sve::transcription_vocab_line(*ctx->store, 400);
    std::string prev;
    const int total = (int)pieces.size();
    for (int i = 0; i < total; ++i) {
        if (cb && cb("chunk", i, total, user)) return false;
        std::string instr = std::string(mtmd_get_marker(ctx->mtmd)) +
            "\nTranscribe this audio exactly as spoken. Write each word in the script of the language it was spoken in: "
            "Telugu words in Telugu script, Hindi in Devanagari, English words in English. Do not translate. "
            "Output only the transcript.";
        if (!vocab.empty()) instr += "\n" + vocab + ".";
        if (!prev.empty()) {
            // The tail of the previous piece keeps names and spellings consistent across the 28 s cuts.
            std::string tail = prev.size() > 400 ? prev.substr(prev.size() - 400) : prev;
            while (!tail.empty() && (static_cast<unsigned char>(tail.front()) & 0xC0) == 0x80) tail.erase(0, 1);
            instr += "\nFor context only, the previous part ended with: \"" + tail + "\"";
        }
        std::string prompt = llm.apply_template(sys, instr);

        llama_memory_clear(llama_get_memory(llm.ctx), true);
        mtmd_bitmap *bmp = mtmd_bitmap_init_from_audio(pieces[i].second - pieces[i].first, pcm.data() + pieces[i].first);
        mtmd_input_chunks *chunks = mtmd_input_chunks_init();
        mtmd_input_text text{};
        text.text = prompt.c_str();
        text.text_len = prompt.size();
        text.add_special = true;
        text.parse_special = true;
        const mtmd_bitmap *bmps[1] = {bmp};
        int rc = mtmd_tokenize(ctx->mtmd, chunks, &text, bmps, 1);
        llama_pos n_past = 0;
        if (rc == 0) rc = mtmd_helper_eval_chunks(ctx->mtmd, llm.ctx, chunks, 0, 0, (int32_t)llama_n_batch(llm.ctx), true, &n_past);
        mtmd_input_chunks_free(chunks);
        mtmd_bitmap_free(bmp);
        if (rc != 0) { sve_logf(2, "audio piece %d/%d failed to evaluate (%d)", i + 1, total, rc); return false; }

        std::string piece;
        int room = std::min(768, llm.n_ctx - (int)n_past - 4);
        if (!llm.generate(room, true, piece, cb, user, "chunk", i, total)) return false;
        piece = strip_thinking(piece);
        if (!piece.empty()) { result.push_back({(int64_t)(pieces[i].first / 16000), piece}); prev = piece; }
    }
    if (cb) cb("final", total, total, user);
    return true;
}

char *sve_transcribe_audio(sve_ctx *ctx, const char *wav_path, sve_progress_cb cb, void *user) {
    try {
        std::vector<std::pair<int64_t, std::string>> pieces;
        if (!transcribe_pieces(ctx, wav_path, cb, user, pieces)) return nullptr;
        std::string out;
        for (const auto &p : pieces) { out += p.second; out += "\n"; }
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
        std::vector<std::pair<int64_t, std::string>> pieces;
        if (!transcribe_pieces(ctx, wav_path, cb, user, pieces)) return nullptr;
        sve::json arr = sve::json::array();
        for (const auto &p : pieces) arr.push_back({{"start_sec", p.first}, {"text", p.second}});
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
char *sve_transcribe_audio(sve_ctx *, const char *, sve_progress_cb, void *) { return nullptr; }
char *sve_transcribe_audio_pieces(sve_ctx *, const char *, sve_progress_cb, void *) { return nullptr; }
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
int sve_note_clear_transcript(sve_ctx *ctx, int64_t id) { return ctx && ctx->store->set_text_column(id, "transcript", nullptr) ? 0 : -1; }

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

// ---------------------------------------------------------------- whisper (device build)
#ifdef SVE_WITH_WHISPER

int sve_load_asr(sve_ctx *ctx, const char *path, int n_threads) {
    if (!ctx || !path) return -1;
    sve_unload_asr(ctx);
    whisper_context_params wp = whisper_context_default_params();
    wp.use_gpu = false;
    ctx->whisper = whisper_init_from_file_with_params(path, wp);
    if (!ctx->whisper) return -2;
    unsigned hw = std::thread::hardware_concurrency();
    ctx->whisper_threads = n_threads > 0 ? n_threads : (int)std::max(1u, std::min(hw, 4u));
    return 0;
}

void sve_unload_asr(sve_ctx *ctx) {
    if (ctx && ctx->whisper) { whisper_free(ctx->whisper); ctx->whisper = nullptr; }
}

char *sve_transcribe_wav(sve_ctx *ctx, const char *wav_path, const char *lang, sve_progress_cb cb, void *user) {
    if (!ctx || !ctx->whisper || !wav_path) return nullptr;
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
    if (whisper_full(ctx->whisper, wp, pcm.data(), (int)pcm.size()) != 0) return nullptr;

    std::string out;
    int n = whisper_full_n_segments(ctx->whisper);
    for (int i = 0; i < n; ++i) {
        const char *t = whisper_full_get_segment_text(ctx->whisper, i);
        if (t) { out += t; out += "\n"; }
    }
    if (cb) cb("final", 1, 1, user);
    return dup_str(out);
}
#endif
