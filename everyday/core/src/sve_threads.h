// Internal: the CPU thread rules for the model (load time and sve_set_threads). Pure, so the tests check them.
#pragma once
#include <algorithm>

namespace sve {

// Decoding is memory-bound: the count asked for, else up to 4 cores (at least 1).
inline int decode_threads(int hw, int requested) {
    return requested > 0 ? requested : std::max(1, std::min(hw, 4));
}

// Prefill (and the audio encoder) are compute-bound bursts: up to 8 cores, never fewer than decoding.
// Mac CPU: prefill 11.8 s -> 7.2 s per 28 s piece, transcripts identical. A 6-core iPhone gets 6.
inline int batch_threads(int hw, int n_threads) { return std::max(n_threads, std::min(hw, 8)); }

struct ThreadPair { int decode; int batch; };

// The embedder (sve_load_embedder): a background CPU model next to Gemma, so its prefill uses no more cores than
// asked for: batch threads = decode threads (n 0 => up to 4).
inline ThreadPair embed_threads(int hw, int n) {
    int d = decode_threads(hw, n);
    return {d, d};
}

// sve_set_threads: n_decode 0 => the load-time default; n_batch 0 => batch_threads for that decode count.
inline ThreadPair resolve_threads(int hw, int n_decode, int n_batch) {
    int d = decode_threads(hw, n_decode);
    return {d, n_batch > 0 ? n_batch : batch_threads(hw, d)};
}

// sve_set_audio_threads: the audio encoder follows the prefill's threads. n 0 (or less) => the model's batch
// threads now; never 0.
inline int audio_threads(int requested, int batch) { return std::max(1, requested > 0 ? requested : batch); }

// mtmd has no runtime thread setter: the encoder is loaded again only when the count changes.
inline bool audio_threads_need_reload(int loaded, int wanted) { return loaded != wanted; }

} // namespace sve
