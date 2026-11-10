#pragma once
// Detailed notes and revision questions written one section at a time (processing engine v2), and put together.
// Pure: no llama, no SQLite. Tested in core/tests/test_assemble.cpp. The model call and the store writes are
// sve_section_output in sve_core.cpp.
#include <string>
#include "sve_extract.h"  // json

namespace sve {

// One part's output kind. The values are sve_output_kind's (SVE_OUT_DETAILED_NOTES, SVE_OUT_REVISION_QUESTIONS),
// as stored in section_outputs.kind.
enum class SectionOutputKind { Notes = 1, Questions = 2 };

// Output tokens of one part: notes 360, questions 200.
int section_output_max_out(SectionOutputKind kind);

// The user prompt of one part: "This is part <part> of <total> of the recording.", the part's section summary
// (guide, as sections_text renders it) as a guide, then detailed_notes_prompt / revision_questions_prompt over the
// part's own material (its pieces, unclear ones marked; from_bullets when it had to be reduced first). Notes end with "## Topic", as sve_generate's do.
std::string section_output_prompt(SectionOutputKind kind, const std::string &material, const std::string &guide,
                                  int part, int total, bool from_bullets = false);

// The detailed notes of a recording from its parts. sections: as sve_note_sections returns them (in order);
// outputs: the parts' notes, one string per section, same order.
// - One "## <section title>" per section ("Part N" when it has no title), in order. A part that is empty, or
//   whose every heading only says there is nothing (says_no_tasks: "None", "N/A"…), gets no heading.
// - Under it, the part's own headings one level down ("## Key points" -> "### Key points"); a heading that only
//   says there is nothing is left out.
// - Every part's "## Action items" lines go into one final "## Action items" (deduplicated on match_tokens); it
//   says "None" only when there is no task and a part said so; with no action items heading in any part it is left out.
// - Then [unclear] tags are removed and none_action_items runs over the whole (each part already went through
//   the notes checks, against its own material, when it was written).
std::string assemble_notes(const nlohmann::json &sections, const nlohmann::json &outputs);

// The revision questions of a recording from its parts (one string per section, "Q1. …\nA1. …" each).
// Questions are taken round-robin across the parts (every part is represented when capped), a near-duplicate of
// one already taken (Jaccard >= 0.7 on match_tokens of the question) is dropped, a question with no answer too,
// at most `max`. Numbered Q1…Qn / A1…An, a blank line between them.
std::string assemble_questions(const nlohmann::json &outputs, int max = 12);

}  // namespace sve
