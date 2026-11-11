#include "sve_llm.h"
#include "sve_internal.h"
#include "sve_prompts.h"
#include "sve_budget.h"
#include "sve_threads.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <thread>

namespace sve {


Llm::~Llm() {
    if (ctx) llama_free(ctx);
    if (model) llama_model_free(model);
}

bool Llm::load(const char *path, int n_ctx_req, int n_threads_req, int n_gpu_layers, bool embeddings, int n_seq) {
    llama_model_params mp = llama_model_default_params();
    this->embeddings = embeddings;
    if (embeddings) n_gpu_layers = 0;
    this->n_gpu_layers = n_gpu_layers;
    mp.n_gpu_layers = n_gpu_layers; // default load mode is mmap, which is what we want on every host
    // The embedder runs in the background too, where iOS refuses GPU work: no GPU device at all.
    static ggml_backend_dev_t kNoDevices[] = {nullptr};
    if (embeddings) mp.devices = kNoDevices;
    model = llama_model_load_from_file(path, mp);
    if (!model) { sve_logf(2, "failed to load model %s", path); return false; }
    vocab = llama_model_get_vocab(model);

    n_ctx = n_ctx_req > 0 ? n_ctx_req : 4096;
    int train = llama_model_n_ctx_train(model);
    if (train > 0 && n_ctx > train) n_ctx = train;

    // Decoding (memory-bound) up to 4 cores; prefill and the audio encoder up to 8 (sve_threads.h).
    const int hw = (int)std::thread::hardware_concurrency();
    ThreadPair th = embeddings ? embed_threads(hw, n_threads_req) : resolve_threads(hw, n_threads_req, 0);
    n_threads = th.decode;
    n_threads_batch = th.batch;

    llama_context_params cp = llama_context_default_params();
    cp.n_ctx = n_ctx;
    cp.n_batch = 512;
    cp.n_ubatch = 512;
    cp.n_threads = n_threads;
    cp.n_threads_batch = n_threads_batch;
    cp.no_perf = true;
    // Two sequences (sve_section_outputs_pair: a part's notes and questions from one reading of it), in one unified
    // KV memory: the copied prefix shares its cells and each sequence may use the whole n_ctx.
    if (n_seq >= 2) {
        cp.n_seq_max = 2;
        cp.kv_unified = true;
    }
    if (embeddings) {
        // Pooling from the model. The attention is not causal, so a passage must fit one ubatch: embed() keeps the
        // first n_ubatch tokens (512, far more than a piece; it caps the compute buffer next to Gemma).
        cp.embeddings = true;
        cp.n_batch = (uint32_t)n_ctx;
        cp.n_ubatch = std::min<uint32_t>(512, (uint32_t)n_ctx);
        cp.n_seq_max = 1;
        cp.op_offload = false;
    }
    ctx = llama_init_from_model(model, cp);
    if (!ctx) { sve_logf(2, "failed to create context"); return false; }

    const char *t = llama_model_chat_template(model, nullptr);
    if (t) tmpl = t;
    char desc[128];
    llama_model_desc(model, desc, sizeof desc);
    sve_logf(0, "loaded %s (n_ctx=%d, threads=%d, batch threads=%d, gpu_layers=%d, sequences=%u)", desc, n_ctx, n_threads,
             n_threads_batch, n_gpu_layers, llama_n_seq_max(ctx));
    return true;
}

bool Llm::embed(const std::string &text, std::vector<float> &out) {
    out.clear();
    if (!embeddings || !ctx) return false;
    std::vector<llama_token> toks = tokenize(text, true, false);
    if (toks.empty()) return false;
    const size_t cap = std::min<size_t>((size_t)n_ctx, (size_t)llama_n_ubatch(ctx));
    if (toks.size() > cap) toks.resize(cap);
    if (llama_memory_t mem = llama_get_memory(ctx)) llama_memory_clear(mem, true);
    llama_batch batch = llama_batch_init((int32_t)toks.size(), 0, 1);
    for (size_t i = 0; i < toks.size(); i++) {
        batch.token[i] = toks[i];
        batch.pos[i] = (llama_pos)i;
        batch.n_seq_id[i] = 1;
        batch.seq_id[i][0] = 0;
        batch.logits[i] = 1;  // every token is output; the model pools them
    }
    batch.n_tokens = (int32_t)toks.size();
    bool ok = llama_decode(ctx, batch) == 0;
    if (!ok) sve_logf(2, "embedding decode failed (%zu tokens)", toks.size());
    const float *v = ok ? llama_get_embeddings_seq(ctx, 0) : nullptr;
    if (ok && !v) sve_logf(2, "the embedding model has no pooling");
    if (v) out.assign(v, v + llama_model_n_embd_out(model));  // the pooled vector's length
    llama_batch_free(batch);
    return v != nullptr;
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
              sve_progress_cb cb, void *user, const char *phase, int done, int total, bool greedy,
              const PieceFn &on_piece) {
    out.clear();
    last_cut = false;
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
        if (llama_decode(ctx, batch) != 0) { decode_failed = true; sve_logf(2, "decode failed during prefill"); return false; }
        if (cb && cb(phase, done, total, user)) return false;
    }
    return generate(max_new_tokens, greedy, out, cb, user, phase, done, total, nullptr, nullptr, false, on_piece);
}

// The sampler chain of generate (and of each sequence of complete_pair).
static llama_sampler *make_sampler(const llama_vocab *vocab, bool greedy, bool second_hearing) {
    llama_sampler_chain_params sp = llama_sampler_chain_default_params();
    sp.no_perf = true;
    llama_sampler *smpl = llama_sampler_chain_init(sp);
    if (second_hearing) {
        llama_sampler_chain_add(smpl, llama_sampler_init_penalties(llama_vocab_n_tokens(vocab), 64, 1.05f, 0.0f, 0.0f));
        llama_sampler_chain_add(smpl, llama_sampler_init_top_p(0.9f, 1));
        llama_sampler_chain_add(smpl, llama_sampler_init_min_p(0.05f, 1));
        llama_sampler_chain_add(smpl, llama_sampler_init_temp(0.3f));
        llama_sampler_chain_add(smpl, llama_sampler_init_dist(42));
    } else if (greedy || g_sve_force_greedy) {
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

    return smpl;
}

// Samples up to max_new_tokens after a prefill. greedy = deterministic (used for transcription).
bool Llm::generate(int max_new_tokens, bool greedy, std::string &out,
              sve_progress_cb cb, void *user, const char *phase, int done, int total, double *mean_prob,
              int *n_out, bool second_hearing, const PieceFn &on_piece) {
    llama_sampler *smpl = make_sampler(vocab, greedy, second_hearing);
    double prob_sum = 0;
    int prob_n = 0;
    bool ok = true, eog = false;
    char piece[256];
    if (n_out) *n_out = 0;
    last_cut = false;
    for (int i = 0; i < max_new_tokens; ++i) {
        llama_token id = llama_sampler_sample(smpl, ctx, -1);
        if (n_out) ++*n_out;
        sve_log_tokens('o', &id, 1);
        if (llama_vocab_is_eog(vocab, id)) { eog = true; break; }
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
        if (n > 0 && on_piece && !on_piece(std::string(piece, n))) { ok = false; break; }
        llama_batch batch = llama_batch_get_one(&id, 1);
        if (llama_decode(ctx, batch) != 0) { decode_failed = true; sve_logf(2, "decode failed"); ok = false; break; }
        if ((i & 15) == 0 && cb && cb(phase, done, total, user)) { ok = false; break; }
    }
    llama_sampler_free(smpl);
    last_cut = ok && !eog;
    if (mean_prob) *mean_prob = prob_n ? prob_sum / prob_n : std::nan("");
    return ok;
}

// Decodes toks into sequence seq from position pos, in n_batch-sized batches; only the last token gives logits.
// Its logits are those of the last llama_decode (index n-1 of that batch), so the caller samples before the next one.
static bool decode_into(llama_context *ctx, const std::vector<llama_token> &toks, llama_seq_id seq, llama_pos pos,
                        int &last_idx) {
    const int nb = (int)llama_n_batch(ctx);
    llama_batch b = llama_batch_init(nb, 0, 1);
    bool ok = true;
    for (size_t i = 0; i < toks.size() && ok; i += nb) {
        const int n = (int)std::min<size_t>(nb, toks.size() - i);
        for (int j = 0; j < n; ++j) {
            b.token[j] = toks[i + j];
            b.pos[j] = pos + (llama_pos)(i + j);
            b.n_seq_id[j] = 1;
            b.seq_id[j][0] = seq;
            b.logits[j] = i + j + 1 == toks.size();
        }
        b.n_tokens = n;
        ok = llama_decode(ctx, b) == 0;
        last_idx = n - 1;
    }
    llama_batch_free(b);
    return ok;
}

bool Llm::complete_pair(const std::string &head, const std::string (&tails)[2], const int (&max_new)[2],
                        const PairDoneFn &on_done, sve_progress_cb cb, void *user, const char *phase) {
    last_cut = false;
    last_pair_decode_failed = false;
    llama_memory_t mem = llama_get_memory(ctx);
    if (!mem || llama_n_seq_max(ctx) < 2) { sve_logf(2, "pair: the context has one sequence"); return false; }
    llama_memory_clear(mem, true);
    // Guards: every exit from here (a cancel, an error, an exception) frees the samplers and the batch and clears
    // the memory.
    struct Clear {
        llama_memory_t m;
        ~Clear() { llama_memory_clear(m, true); }
    } clear_after{mem};
    struct Seq {
        std::vector<llama_token> tail;
        llama_sampler *smpl = nullptr;
        std::string out;
        llama_pos pos = 0;
        int n = 0, idx = -1;
        bool done = false;
        ~Seq() { if (smpl) llama_sampler_free(smpl); }
    } q[2];
    struct Batch {
        llama_batch b;
        ~Batch() { llama_batch_free(b); }
    } rb{llama_batch_init(2, 0, 1)};
    llama_batch &b = rb.b;

    const std::vector<llama_token> h = tokenize(head, true, true);
    q[0].tail = tokenize(tails[0], false, true);
    q[1].tail = tokenize(tails[1], false, true);
    const long nh = (long)h.size(), n0 = (long)q[0].tail.size(), n1 = (long)q[1].tail.size();
    const long seq_cells = (long)llama_n_ctx_seq(ctx), need = nh + n0 + n1 + max_new[0] + max_new[1] + 16;
    if (h.empty() || need > n_ctx || nh + n0 + max_new[0] + 16 > seq_cells || nh + n1 + max_new[1] + 16 > seq_cells) {
        sve_logf(2, "pair: %ld cells needed for n_ctx %d", need, n_ctx);
        return false;
    }
    auto fail = [&](const char *what) {
        decode_failed = last_pair_decode_failed = true;
        sve_logf(2, "pair: decode failed (%s)", what);
        return false;
    };
    // The shared prefix once, in sequence 0, then a copy for sequence 1 (cells shared, nothing computed).
    for (size_t i = 0; i < h.size(); i += llama_n_batch(ctx)) {
        const int n = (int)std::min<size_t>(llama_n_batch(ctx), h.size() - i);
        if (llama_decode(ctx, llama_batch_get_one(const_cast<llama_token *>(h.data()) + i, n)) != 0) return fail("prefix");
        if (cb && cb(phase, 0, 2, user)) return false;
    }
    llama_memory_seq_cp(mem, 0, 1, -1, -1);
    char piece[256];
    // Samples sequence i's next token from logits index idx of the last decode; ends it on EOG or at its cap.
    auto sample = [&](int i, int idx) {
        Seq &s = q[i];
        const llama_token id = llama_sampler_sample(s.smpl, ctx, idx);
        ++s.n;
        sve_log_tokens('o', &id, 1);
        bool cut = false;
        if (llama_vocab_is_eog(vocab, id)) {
            s.done = true;
        } else {
            llama_sampler_accept(s.smpl, id);
            const int np = llama_token_to_piece(vocab, id, piece, sizeof piece, 0, false);
            if (np > 0) s.out.append(piece, np);
            s.tail.assign(1, id);  // decoded next round
            if (s.n >= max_new[i]) s.done = cut = true;
        }
        if (s.done && on_done) on_done(i, s.out, cut);
    };
    for (int i = 0; i < 2; ++i) {
        q[i].smpl = make_sampler(vocab, false, false);
        q[i].pos = (llama_pos)h.size();
        int idx = -1;
        if (!decode_into(ctx, q[i].tail, i, q[i].pos, idx)) return fail("suffix");
        q[i].pos += (llama_pos)q[i].tail.size();
        if (max_new[i] <= 0) {
            q[i].done = true;
            if (on_done) on_done(i, "", true);
            continue;
        }
        sample(i, idx);  // before the other sequence's suffix replaces these logits
        if (cb && cb(phase, 0, 2, user)) return false;
    }
    // One decode per round: the last token of every sequence still going.
    for (int round = 0; !(q[0].done && q[1].done); ++round) {
        b.n_tokens = 0;
        for (int i = 0; i < 2; ++i) {
            if (q[i].done) continue;
            const int j = b.n_tokens++;
            b.token[j] = q[i].tail[0];
            b.pos[j] = q[i].pos++;
            b.n_seq_id[j] = 1;
            b.seq_id[j][0] = i;
            b.logits[j] = 1;
            q[i].idx = j;
        }
        if (llama_decode(ctx, b) != 0) return fail("round");
        for (int i = 0; i < 2; ++i)
            if (!q[i].done) sample(i, q[i].idx);
        if ((round & 15) == 0 && cb && cb(phase, 0, 2, user)) return false;
    }
    return true;
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
