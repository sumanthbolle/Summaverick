// Token budgets of the notes prompts (summary, detailed notes, revision questions). Pure arithmetic, no model:
// sve_generate, sve_generate_from_sections, reduce_to_fit and sve_transcript_fits all use these, so the
// "does the transcript fit one prompt" decision is the one sve_generate makes.
#pragma once
#include <algorithm>

namespace sve {

// Output tokens of one note output. N7 rev 1: 1400 at n_ctx 8192 (was 700, which cut note 20's detailed notes in
// the middle of Key points, before Decisions, Action items and Dates); a quarter of the context below 5600 (unchanged
// below 2800).
inline int notes_max_out(int n_ctx) { return std::max(160, std::min(1400, n_ctx / 4)); }
// Tokens of a notes prompt around the material: the instructions (the longest, detailed notes with its rules, is
// about 1250 characters of English, ~300 tokens) and the chat template. Was 250 before the N7 rules.
constexpr int kNotesScaffold = 400;
// Room for the material in the final prompt: the scaffolding plus the system prompt, and the answer.
inline int notes_budget(int n_ctx, int sys_tokens) { return n_ctx - notes_max_out(n_ctx) - kNotesScaffold - sys_tokens; }
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

// One-pass notes (title, summary, notes and questions in one reply): output tokens, and the prompt around the
// material (the three output prompts' instructions and rules plus the title's, about 650 tokens, and the template).
// The output cap is above one stepped output's (notes_max_out, 1400 at n_ctx 8192): the reply holds all four
// outputs, and Telugu notes alone can use most of 1400 (was 1100, which cut Telugu notes before the questions).
constexpr int kAllMaxOut = 2400;
constexpr int kAllScaffold = 800;
// True when a one-pass prompt takes material of material_tokens as it is: the same room rule as notes_material_fits,
// with the one-pass output and scaffolding.
inline bool all_material_fits(int material_tokens, int n_ctx, int sys_tokens) {
    const int final_budget = n_ctx - kAllMaxOut - kAllScaffold - sys_tokens;
    return material_tokens <= fit_budget(n_ctx, sys_tokens, final_budget);
}

} // namespace sve
