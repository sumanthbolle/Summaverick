#include "sve_llm.h"
#include "sve_internal.h"
#include "sve_prompts.h"
#include "sve_budget.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <thread>

namespace sve {


Llm::~Llm() {
    if (ctx) llama_free(ctx);
    if (model) llama_model_free(model);
}

bool Llm::load(const char *path, int n_ctx_req, int n_threads_req, int n_gpu_layers) {
    llama_model_params mp = llama_model_default_params();
    this->n_gpu_layers = n_gpu_layers;
    mp.n_gpu_layers = n_gpu_layers; // default load mode is mmap, which is what we want on every host
    model = llama_model_load_from_file(path, mp);
    if (!model) { sve_logf(2, "failed to load model %s", path); return false; }
    vocab = llama_model_get_vocab(model);

    n_ctx = n_ctx_req > 0 ? n_ctx_req : 4096;
    int train = llama_model_n_ctx_train(model);
    if (train > 0 && n_ctx > train) n_ctx = train;

    unsigned hw = std::thread::hardware_concurrency();
    n_threads = n_threads_req > 0 ? n_threads_req : (int)std::max(1u, std::min(hw, 4u));
    // Prefill (and the audio encoder) are compute-bound bursts: up to 8 cores, never fewer than decoding.
    // Mac CPU: prefill 11.8 s -> 7.2 s per 28 s piece, transcripts identical. A 6-core iPhone gets 6
    // (not yet measured there). Decoding is memory-bound and stays on n_threads.
    n_threads_batch = std::max(n_threads, (int)std::min(hw, 8u));

    llama_context_params cp = llama_context_default_params();
    cp.n_ctx = n_ctx;
    cp.n_batch = 512;
    cp.n_ubatch = 512;
    cp.n_threads = n_threads;
    cp.n_threads_batch = n_threads_batch;
    cp.no_perf = true;
    ctx = llama_init_from_model(model, cp);
    if (!ctx) { sve_logf(2, "failed to create context"); return false; }

    const char *t = llama_model_chat_template(model, nullptr);
    if (t) tmpl = t;
    char desc[128];
    llama_model_desc(model, desc, sizeof desc);
    sve_logf(0, "loaded %s (n_ctx=%d, threads=%d, batch threads=%d, gpu_layers=%d)", desc, n_ctx, n_threads,
             n_threads_batch, n_gpu_layers);
    return true;
}

std::vector<llama_token> Llm::tokenize(const std::string &text, bool add_special, bool parse_special) const {
    int n = -llama_tokenize(vocab, text.c_str(), (int)text.size(), nullptr, 0, add_special, parse_special);
    std::vector<llama_token> out(std::max(n, 0));
    if (n > 0) llama_tokenize(vocab, text.c_str(), (int)text.size(), out.data(), n, add_special, parse_special);
    sve_log_tokens('i', out.data(), out.size());
    return out;
}

int Llm::count_tokens(const std::string &text) const {
    return -llama_tokenize(vocab, text.c_str(), (int)text.size(), nullptr, 0, false, false);
}

// Wrap (system,user) in the model's chat template. Falls back to a ChatML‑style layout.
std::string Llm::apply_template(const std::string &system, const std::string &user) const {
    // Gemma 4's template is not one llama_chat_apply_template knows; write it out directly.
    // No <|think|> in the system turn, so the model answers without a thinking block.
    if (tmpl.find("<|turn>") != std::string::npos) {
        return "<|turn>system\n" + system + "<turn|>\n<|turn>user\n" + user + "<turn|>\n<|turn>model\n";
    }
    llama_chat_message msgs[2] = {{"system", system.c_str()}, {"user", user.c_str()}};
    if (!tmpl.empty()) {
        std::vector<char> buf((system.size() + user.size()) * 2 + 1024);
        int n = llama_chat_apply_template(tmpl.c_str(), msgs, 2, true, buf.data(), (int)buf.size());
        if (n > (int)buf.size()) {
            buf.resize(n + 1);
            n = llama_chat_apply_template(tmpl.c_str(), msgs, 2, true, buf.data(), (int)buf.size());
        }
        if (n > 0) return std::string(buf.data(), n);
    }
    return "<|im_start|>system\n" + system + "<|im_end|>\n<|im_start|>user\n" + user +
           "<|im_end|>\n<|im_start|>assistant\n";
}

// Prompt → completion. Clears KV memory first, so every call is independent.
// Returns false on cancel or error.
bool Llm::complete(const std::string &prompt, int max_new_tokens, std::string &out,
              sve_progress_cb cb, void *user, const char *phase, int done, int total, bool greedy) {
    out.clear();
    llama_memory_clear(llama_get_memory(ctx), true);

    std::vector<llama_token> toks = tokenize(prompt, true, true);
    if ((int)toks.size() + 16 >= n_ctx) {
        sve_logf(2, "prompt too long (%zu tokens for n_ctx %d)", toks.size(), n_ctx);
        return false;
    }
    int room = n_ctx - (int)toks.size() - 4;
    if (max_new_tokens > room) max_new_tokens = room;

    // prefill in n_batch‑sized pieces
    const int nb = (int)llama_n_batch(ctx);
    for (size_t i = 0; i < toks.size(); i += nb) {
        int n = (int)std::min<size_t>(nb, toks.size() - i);
        llama_batch batch = llama_batch_get_one(toks.data() + i, n);
        if (llama_decode(ctx, batch) != 0) { sve_logf(2, "decode failed during prefill"); return false; }
        if (cb && cb(phase, done, total, user)) return false;
    }
    return generate(max_new_tokens, greedy, out, cb, user, phase, done, total);
}

// Samples up to max_new_tokens after a prefill. greedy = deterministic (used for transcription).
bool Llm::generate(int max_new_tokens, bool greedy, std::string &out,
              sve_progress_cb cb, void *user, const char *phase, int done, int total, double *mean_prob,
              int *n_out) {
    llama_sampler_chain_params sp = llama_sampler_chain_default_params();
    sp.no_perf = true;
    llama_sampler *smpl = llama_sampler_chain_init(sp);
    double prob_sum = 0;
    int prob_n = 0;
    if (greedy || g_sve_force_greedy) {
        // light penalty only: real speech repeats words, but a stuck model repeats whole phrases
        llama_sampler_chain_add(smpl, llama_sampler_init_penalties(llama_vocab_n_tokens(vocab), 64, 1.05f, 0.0f, 0.0f));
        llama_sampler_chain_add(smpl, llama_sampler_init_greedy());
    } else {
        llama_sampler_chain_add(smpl, llama_sampler_init_penalties(llama_vocab_n_tokens(vocab), 64, 1.15f, 0.0f, 0.0f));
        llama_sampler_chain_add(smpl, llama_sampler_init_top_p(0.9f, 1));
        llama_sampler_chain_add(smpl, llama_sampler_init_min_p(0.05f, 1));
        llama_sampler_chain_add(smpl, llama_sampler_init_temp(0.3f));
        llama_sampler_chain_add(smpl, llama_sampler_init_dist(42));
    }

    bool ok = true;
    char piece[256];
    if (n_out) *n_out = 0;
    for (int i = 0; i < max_new_tokens; ++i) {
        llama_token id = llama_sampler_sample(smpl, ctx, -1);
        if (n_out) ++*n_out;
        sve_log_tokens('o', &id, 1);
        if (llama_vocab_is_eog(vocab, id)) break;
        if (mean_prob) {
            const float *lg = llama_get_logits_ith(ctx, -1);
            const int nv = llama_vocab_n_tokens(vocab);
            if (lg && id >= 0 && id < nv) {
                float mx = lg[0];
                for (int k = 1; k < nv; ++k) mx = std::max(mx, lg[k]);
                double z = 0;
                for (int k = 0; k < nv; ++k) z += std::exp((double)lg[k] - mx);
                prob_sum += std::exp((double)lg[id] - mx) / z;
                ++prob_n;
            }
        }
        llama_sampler_accept(smpl, id);
        int n = llama_token_to_piece(vocab, id, piece, sizeof piece, 0, false);
        if (n > 0) out.append(piece, n);
        llama_batch batch = llama_batch_get_one(&id, 1);
        if (llama_decode(ctx, batch) != 0) { sve_logf(2, "decode failed"); ok = false; break; }
        if ((i & 15) == 0 && cb && cb(phase, done, total, user)) { ok = false; break; }
    }
    llama_sampler_free(smpl);
    if (mean_prob) *mean_prob = prob_n ? prob_sum / prob_n : std::nan("");
    return ok;
}

// Remove thinking blocks (Qwen3 "<think>…</think>", Gemma 4 "<|channel>…<channel|>") and surrounding whitespace.
std::string strip_thinking(std::string s) {
    for (auto tags : {std::make_pair(std::string("<think>"), std::string("</think>")),
                      std::make_pair(std::string("<|channel>"), std::string("<channel|>"))}) {
        for (;;) {
            size_t a = s.find(tags.first);
            if (a == std::string::npos) break;
            size_t b = s.find(tags.second, a);
            if (b == std::string::npos) { s.erase(a); break; }
            s.erase(a, b + tags.second.size() - a);
        }
    }
    size_t st = s.find_first_not_of(" \t\r\n");
    if (st == std::string::npos) return "";
    size_t en = s.find_last_not_of(" \t\r\n");
    return s.substr(st, en - st + 1);
}

// Split text into pieces of ~target_chars, cutting at paragraph/sentence boundaries.
std::vector<std::string> split_text(const std::string &text, size_t target_chars) {
    std::vector<std::string> parts;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = std::min(text.size(), pos + target_chars);
        if (end < text.size()) {
            // search back for a good boundary, but not more than 40% of the chunk
            size_t floor = pos + target_chars * 6 / 10;
            size_t cut = std::string::npos;
            for (const char *sep : {"\n\n", "\n", ". ", "। ", "? ", "! ", "; ", ", ", " "}) {
                size_t c = text.rfind(sep, end);
                if (c != std::string::npos && c > floor) { cut = c + strlen(sep); break; }
            }
            if (cut != std::string::npos) end = cut;
        }
        // never cut inside a UTF‑8 sequence
        while (end < text.size() && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) ++end;
        parts.push_back(text.substr(pos, end - pos));
        pos = end;
    }
    return parts;
}

bool reduce_to_fit(Llm &llm, const std::string &sys, std::string material, int final_budget,
                   std::string &out, bool &from_bullets, sve_progress_cb cb, void *user) {
    from_bullets = false;
    // Per-chunk extraction needs room for the chunk, the system prompt, ~80 tokens of scaffolding
    // and its own output.
    const int sys_tokens = llm.count_tokens(sys);
    const int chunk_out = sve::chunk_max_out(llm.n_ctx);
    const int chunk_budget = sve::chunk_budget(llm.n_ctx, sys_tokens);
    final_budget = sve::fit_budget(llm.n_ctx, sys_tokens, final_budget);

    for (int round = 0; round < 4; ++round) {
        int ntok = llm.count_tokens(material);
        if (ntok <= final_budget) { out = material; return true; }

        double chars_per_tok = (double)material.size() / std::max(1, ntok);
        // Split by chunk_budget (as large as the context allows) so we make as few model calls as possible.
        size_t target_chars = (size_t)std::max(200.0, chunk_budget * chars_per_tok * 0.9);
        std::vector<std::string> parts = split_text(material, target_chars);

        std::string bullets;
        int total = (int)parts.size();
        for (int i = 0; i < total; ++i) {
            std::string prompt = llm.apply_template(sys, chunk_prompt(parts[i], i, total));
            std::string piece;
            if (!llm.complete(prompt, chunk_out, piece, cb, user, "chunk", i, total)) return false;
            bullets += strip_thinking(piece);
            bullets += "\n";
        }
        material = bullets;
        from_bullets = true;
    }
    out = material;
    return true;
}

} // namespace sve
