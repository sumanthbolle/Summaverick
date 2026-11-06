// Internal: llama.cpp wrapper shared by the core's .cpp files. Not part of the public API.
#pragma once
#include "sve_core.h"

// On iOS llama ships as llama.xcframework (framework-style include); elsewhere as a plain header.
#if __has_include(<llama/llama.h>)
#include <llama/llama.h>
#else
#include "llama.h"
#endif

#include <string>
#include <vector>

namespace sve {

struct Llm {
    llama_model *model = nullptr;
    llama_context *ctx = nullptr;
    const llama_vocab *vocab = nullptr;
    int n_ctx = 4096;
    int n_threads = 4;
    std::string tmpl; // chat template (jinja source) from the GGUF, or empty

    ~Llm();
    bool load(const char *path, int n_ctx_req, int n_threads_req, int n_gpu_layers);
    std::vector<llama_token> tokenize(const std::string &text, bool add_special, bool parse_special) const;
    int count_tokens(const std::string &text) const;
    // Wrap (system,user) in the model's chat template. Falls back to a ChatML-style layout.
    std::string apply_template(const std::string &system, const std::string &user) const;
    // Prompt -> completion. Clears KV memory first, so every call is independent. False on cancel or error.
    bool complete(const std::string &prompt, int max_new_tokens, std::string &out,
                  sve_progress_cb cb, void *user, const char *phase, int done, int total);
    // Samples up to max_new_tokens after a prefill. greedy = deterministic (used for transcription).
    bool generate(int max_new_tokens, bool greedy, std::string &out,
                  sve_progress_cb cb, void *user, const char *phase, int done, int total);
};

// Remove thinking blocks and surrounding whitespace.
std::string strip_thinking(std::string s);
// Split text into pieces of ~target_chars, cutting at paragraph/sentence boundaries.
std::vector<std::string> split_text(const std::string &text, size_t target_chars);

} // namespace sve
