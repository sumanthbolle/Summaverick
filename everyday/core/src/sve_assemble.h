#pragma once
// Detailed notes and revision questions written one section at a time (processing engine v2), and put together.
// Pure: no llama, no SQLite. Tested in core/tests/test_assemble.cpp. The model call and the store writes are
// sve_section_output in sve_core.cpp.
#include <string>
#include <utility>
#include <vector>
#include "sve_extract.h"  // json

namespace sve {

// One part's output kind. The values are sve_output_kind's (SVE_OUT_DETAILED_NOTES, SVE_OUT_REVISION_QUESTIONS),
// as stored in section_outputs.kind.
enum class SectionOutputKind { Notes = 1, Questions = 2 };

// Output tokens of one part: notes 440, questions 300 (Telugu takes more tokens per word).
int section_output_max_out(SectionOutputKind kind);

// The user prompt of one part: section_prompt_shared, then section_prompt_suffix(kind). Everything kind-specific
// comes after the material, so the notes and the questions of a part can share one reading of it
// (sve_section_outputs_pair). Notes end with "## Topic", as sve_generate's do.
std::string section_output_prompt(SectionOutputKind kind, const std::string &material, const std::string &guide,
                                  int part, int total, bool from_bullets = false);
// The part of a part's prompt both kinds share: "This is part <part> of <total> of the recording." ("This is part
// <part> of the recording." when total is 0: written during the recording), the part's
// section summary (guide, as sections_text renders it) as a guide, then the part's own material (its pieces,
// unclear ones marked; from_bullets when it had to be reduced first) in triple quotes.
std::string section_prompt_shared(const std::string &material, const std::string &guide, int part, int total,
                                  bool from_bullets = false);
// What one kind asks of the material above: the detailed notes structure (ending "## Topic") or the 6 revision
// questions, each with its rules (faithful_rules, as detailed_notes_prompt / revision_questions_prompt).
std::string section_prompt_suffix(SectionOutputKind kind);

// Splits full (a chat-templated prompt of the user text shared_user + suffix_user) where shared_user ends: head is
// everything up to and including shared_user, tail the rest (suffix_user and the template's end of turn). The last
// copy of the user text is the one split. False when the template did not keep the user text as it is.
bool split_templated(const std::string &full, const std::string &shared_user, const std::string &suffix_user,
                     std::string &head, std::string &tail);

// Cells kept free next to a pair, as Llm::complete keeps for one prompt.
constexpr int kPairMargin = 16;
// True when a part's notes and questions can be written together: a context with n_seq_max >= 2 sequences, a
// non-empty shared prefix, and room for the prefix once plus both suffixes and both caps (440 + 300) in n_ctx
// cells (a unified KV memory: the copied prefix shares its cells), and for each sequence's prefix, suffix and cap
// in n_ctx_seq (the cells one sequence may use). Otherwise the part is written by two single calls.
bool section_pair_fits(int n_seq_max, int n_ctx, int n_ctx_seq, int shared_tokens, int notes_suffix_tokens,
                       int questions_suffix_tokens);

// The detailed notes of a recording from its parts. sections: as sve_note_sections returns them (in order);
// outputs: the parts' notes, one string per section, same order.
// - One "## <section title>" per section ("Part N" when it has no title), in order. A part that is empty, or
//   whose every heading only says there is nothing (says_no_tasks: "None", "N/A"…), gets no heading.
// - Under it, the part's own headings one level down ("## Key points" -> "### Key points"); a heading that only
//   says there is nothing is left out. "## Topic <text>" on one line becomes "### Topic" with <text> under it.
// - Every part's "## Action items" lines go into one final "## Action items" (deduplicated on match_tokens); it
//   says "None" only when there is no task and every non-empty part has an action items heading that says none
//   (a part without one, e.g. cut at its cap, may have had tasks); otherwise, with no task, it is left out.
// - Then [unclear] tags are removed and none_action_items runs over the whole (each part already went through
//   the notes checks, against its own material, when it was written).
// - placeholder (optional): a part whose output is null (not written yet) gets its "## <section title>" and the
//   placeholder under it, and the action items never say "None"; without one a null part is like an empty one.
std::string assemble_notes(const nlohmann::json &sections, const nlohmann::json &outputs,
                           const std::string *placeholder = nullptr);

// The revision questions of a recording from its parts (one string per section, "Q1. …\nA1. …" each).
// Questions are taken round-robin across the parts (every part is represented when capped), a near-duplicate of
// one already taken (Jaccard >= 0.7 on match_tokens of the question) is dropped, a question with no answer too,
// at most `max`. Numbered Q1…Qn / A1…An, a blank line between them. A null part (not written yet) gives none.
// How many whole questions (with an answer) assemble_questions finds in one part's reply. Labels may be "Q1.",
// "Q 1)", "**Q1.**", "### Q1:", or a bare "1." in a reply without Q labels; answers "A1.", "A.", "Answer:".
int count_questions(const std::string &reply);
// The whole questions (with an answer) of a reply, as count_questions counts them: {question, answer} without labels.
std::vector<std::pair<std::string, std::string>> question_pairs(const std::string &reply);
// True when a one-pass questions part has at least as many questions as the stepped questions prompt asks for
// (kRevisionQuestions): sve_generate_all uses the one-pass reply only then.
bool onepass_questions_enough(const std::string &questions);

std::string assemble_questions(const nlohmann::json &outputs, int max = 12);

}  // namespace sve
