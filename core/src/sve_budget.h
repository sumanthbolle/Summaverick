// Token budgets of the notes prompts (summary, detailed notes, revision questions). Pure arithmetic, no model:
// sve_generate, sve_generate_from_sections, reduce_to_fit and sve_transcript_fits all use these, so the
// "does the transcript fit one prompt" decision is the one sve_generate makes.
#pragma once
#include <algorithm>

namespace sve {

// Output tokens of one note output.
inline int notes_max_out(int n_ctx) { return std::max(160, std::min(700, n_ctx / 4)); }
// Room for the material in the final prompt: prompt scaffolding ≈ 250 tokens plus the system prompt.
inline int notes_budget(int n_ctx, int sys_tokens) { return n_ctx - notes_max_out(n_ctx) - 250 - sys_tokens; }
// Output tokens of one chunk summary (reduce_to_fit's per-chunk step).
inline int chunk_max_out(int n_ctx) { return std::max(120, std::min(400, n_ctx / 5)); }
// Room for one chunk: the system prompt, ~80 tokens of scaffolding and the chunk's own output.
inline int chunk_budget(int n_ctx, int sys_tokens) { return std::max(128, n_ctx - chunk_max_out(n_ctx) - sys_tokens - 80 - 32); }
// The budget reduce_to_fit holds material to: material within it is used as it is, larger is summarised.
inline int fit_budget(int n_ctx, int sys_tokens, int final_budget) {
    return std::max(128, std::min(final_budget, chunk_budget(n_ctx, sys_tokens)));
}
// True when a notes prompt takes material of material_tokens as it is (no reduce_to_fit chunking).
inline bool notes_material_fits(int material_tokens, int n_ctx, int sys_tokens) {
    return material_tokens <= fit_budget(n_ctx, sys_tokens, notes_budget(n_ctx, sys_tokens));
}

} // namespace sve
