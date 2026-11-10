// Internal: llama.cpp wrapper shared by the core's .cpp files. Not part of the public API.
#pragma once
#include "sve_core.h"

// On iOS llama ships as llama.xcframework (framework-style include); elsewhere as a plain header.
#if __has_include(<llama/llama.h>)
#include <llama/llama.h>
#else
#include "llama.h"
#endif

#include <functional>
#include <string>
#include <vector>

namespace sve {

// Each generated piece of text as it is sampled (raw: thinking blocks included). Returns false to cancel.
using PieceFn = std::function<bool(const std::string &piece)>;

struct Llm {
    llama_model *model = nullptr;
    llama_context *ctx = nullptr;
    const llama_vocab *vocab = nullptr;
    int n_ctx = 4096;
    int n_threads = 4;
    int n_threads_batch = 4;   // prefill and the audio encoder (load sets it)
    int n_gpu_layers = 0;   // 0 = CPU only; the audio projector follows (sve_load_audio)
    std::string tmpl; // chat template (jinja source) from the GGUF, or empty
    // A model run failed on the GPU backend (iOS refuses GPU work in the background): the Metal state stays broken and
    // every later run fails at once, even in front, until the model is loaded again (sve_llm_needs_reload).
    bool decode_failed = false;
    bool last_cut = false;  // the last generate stopped at max_new_tokens, before an end-of-generation token

    ~Llm();
    bool embeddings = false;  // loaded as an embedding model (meaning search): embed() only, never complete()

    // embeddings: an embedding model (EmbeddingGemma), CPU only whatever n_gpu_layers says (no GPU device, no op
    // offload), the model's own pooling, one sequence per batch (n_batch = n_ubatch = n_ctx).
    bool load(const char *path, int n_ctx_req, int n_threads_req, int n_gpu_layers, bool embeddings = false);
    // The pooled vector of text (its first n_ctx tokens), as the model gives it (not normalised). False on error.
    bool embed(const std::string &text, std::vector<float> &out);
    std::vector<llama_token> tokenize(const std::string &text, bool add_special, bool parse_special) const;
    int count_tokens(const std::string &text) const;
    // Wrap (system,user) in the model's chat template. Falls back to a ChatML-style layout.
    std::string apply_template(const std::string &system, const std::string &user) const;
    // Prompt -> completion. Clears KV memory first, so every call is independent. False on cancel or error.
    // greedy = deterministic sampling (assistant JSON and answers); notes keep the sampled default.
    // on_piece (optional): each token's text as it is sampled (Ask streams its answer); false cancels.
    bool complete(const std::string &prompt, int max_new_tokens, std::string &out,
                  sve_progress_cb cb, void *user, const char *phase, int done, int total, bool greedy = false,
                  const PieceFn &on_piece = nullptr);
    // Samples up to max_new_tokens after a prefill. greedy = deterministic (used for transcription).
    // mean_prob (optional): mean softmax probability of the chosen non-EOG tokens under the raw logits
    // (before penalties); NaN when no token was generated. Read after sampling, never changes the choice.
    // n_out (optional): how many tokens were sampled, the end-of-generation token included.
    // second_hearing: a transcription piece heard again (N7): the transcription's light penalty, then
    // top-p 0.9 / min-p 0.05 / temperature 0.3 with a fixed seed, even under g_sve_force_greedy (a greedy repeat
    // of the same input gives the same text). Overrides greedy.
    // on_piece: as for complete.
    bool generate(int max_new_tokens, bool greedy, std::string &out,
                  sve_progress_cb cb, void *user, const char *phase, int done, int total,
                  double *mean_prob = nullptr, int *n_out = nullptr, bool second_hearing = false,
                  const PieceFn &on_piece = nullptr);
};

// Remove thinking blocks and surrounding whitespace.
std::string strip_thinking(std::string s);
// Split text into pieces of ~target_chars, cutting at paragraph/sentence boundaries.
std::vector<std::string> split_text(const std::string &text, size_t target_chars);

// Reduce long material to bullet points that fit final_budget tokens (hierarchical, per-chunk
// calls with `system` as the system prompt). from_bullets is set when any reduction happened.
// False on cancel/error.
bool reduce_to_fit(Llm &llm, const std::string &system, std::string material, int final_budget,
                   std::string &out, bool &from_bullets, sve_progress_cb cb, void *user);

} // namespace sve
