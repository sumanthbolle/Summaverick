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
#include <memory>

// Logging: routes to the host callback (sve_set_log) or stderr.
void sve_logf(int level, const char *fmt, ...);

// Test hook: output token cap of sve_section_summarize (smoke sets a tiny value to force the fallback).
extern int g_sve_section_max_out;
// Test hooks (tools/model/compare_outputs.sh): every generation greedy; token ids written to a file
// ("i <id>" prompt, "o <id>" generated). Off by default; the app never sets them.
extern bool g_sve_force_greedy;
extern FILE *g_sve_token_log;
void sve_log_tokens(char tag, const int32_t *ids, size_t n);

struct sve_ctx {
    std::unique_ptr<sve::Store> store;
    std::unique_ptr<sve::Llm> llm;
#ifdef SVE_WITH_MTMD
    mtmd_context *mtmd = nullptr; // audio projector; belongs to llm->model, freed before it
#endif
#ifdef SVE_WITH_WHISPER
    whisper_context *whisper = nullptr;
    int whisper_threads = 4;
#endif
};
