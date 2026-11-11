// Internal: shared by the core's .cpp files. Not part of the public API.
#pragma once
#include "sve_core.h"
#include "sve_store.h"
#include "sve_llm.h"

// Audio-capable models (Gemma 4) go through llama.cpp's multimodal library. It ships inside
// llama.xcframework on iOS; other hosts build without it unless they link mtmd.
#if __has_include(<llama/mtmd.h>)
#include <llama/mtmd.h>
#include <llama/mtmd-helper.h>
#define SVE_WITH_MTMD 1
#endif

#ifdef SVE_WITH_WHISPER
#include "whisper.h"
#endif

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

// Logging: routes to the host callback (sve_set_log) or stderr.
void sve_logf(int level, const char *fmt, ...);

// C API helpers (sve_assistant.cpp, sve_brain.cpp). Strings returned to the host are malloc'd (sve_free_string).
inline char *dup(const std::string &s) {
    char *p = (char *)malloc(s.size() + 1);
    if (!p) return nullptr;
    memcpy(p, s.data(), s.size());
    p[s.size()] = 0;
    return p;
}

inline char *dump(const sve::json &j) { return dup(j.dump()); }

// Runs f; any exception is logged and turned into the error value.
template <class R, class F>
inline R guard(const char *name, R err, F f) {
    try {
        return f();
    } catch (const std::exception &e) {
        sve_logf(2, "%s failed: %s", name, e.what());
    } catch (...) {
        sve_logf(2, "%s failed", name);
    }
    return err;
}

inline sve::json parse(const char *s) {
    if (!s) return sve::json();
    sve::json j = sve::json::parse(s, nullptr, false);
    return j.is_discarded() ? sve::json() : j;
}

namespace sve {
// Medicine names the person has or the model proposed from recordings (sve_assistant.cpp).
std::vector<std::string> known_med_names(Store &st);
}  // namespace sve

// Test hook (smoke mode "memories"): called for every memory candidate sve_memories_from_section drops,
// with the reason (sve::Grounded::reason, or "double_check_no") and the candidate JSON. NULL in the app.
extern void (*g_sve_memory_drop)(int64_t note_id, int section_idx, const char *reason, const char *cand_json);

// Test hook (smoke mode "api"): stands in for the model in sve_memories_from_section. Given the phase
// ("memories" or "memory_check") and the user prompt, returns the reply (NULL = the call failed). NULL in the app.
extern const char *(*g_sve_model_reply)(const char *phase, const char *prompt);

// Test hook: output token cap of sve_section_summarize (smoke sets a tiny value to force the fallback).
extern int g_sve_section_max_out;
// Test hooks (tools/model/compare_outputs.sh): every generation greedy; token ids written to a file
// ("i <id>" prompt, "o <id>" generated). Off by default; the app never sets them.
extern bool g_sve_force_greedy;
extern FILE *g_sve_token_log;
void sve_log_tokens(char tag, const int32_t *ids, size_t n);

struct sve_ctx {
    std::unique_ptr<sve::Store> store;
    // Per recording (WAV path), since this process started: pieces heard by sve_transcribe_next_piece and how many
    // of them were heard a second time; logged and cleared at its "end" (N7: the phone check measures the share).
    std::map<std::string, std::pair<int, int>> hearings;
    std::unique_ptr<sve::Llm> llm;
    // Notes and questions of a part written together (sve_set_pair_enabled): off, the model loads with one sequence
    // and every part is written by single calls.
    bool pair_enabled = true;
    // Meaning search (sve_load_embedder): a second, CPU-only model next to llm, and the tag its vectors are stored
    // under (file name and dimensions), so vectors of another model are never compared with its own.
    std::unique_ptr<sve::Llm> embed;
    std::string embed_model;
#ifdef SVE_WITH_MTMD
    mtmd_context *mtmd = nullptr; // audio projector; belongs to llm->model, freed before it
    std::string mmproj_path;      // the projector's file, to load it again with other threads
    int audio_threads = 0;        // the encoder's CPU threads since its load
#endif
#ifdef SVE_WITH_WHISPER
    whisper_context *whisper = nullptr;
    int whisper_threads = 4;
#endif
};
