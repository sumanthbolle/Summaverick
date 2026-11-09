#pragma once
// Plans a recording's transcript pieces into ~5 minute sections (cut only at piece boundaries).
// Pure: no llama, no SQLite. Tested in core/tests/test_sections.cpp.
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>
#include "sve_extract.h"  // json, first_json_object

namespace sve {

struct PieceRef {
    int64_t start_sec;
    int64_t end_sec;  // the next piece's start, or the note duration for the last piece
};

// Half-open piece index ranges [first, last) covering the pieces in order. A range closes at the first
// boundary at or past `target` seconds, or at or past `min_len` when the next piece would pass `max_len`.
// A single piece longer than `max_len` is its own range. When !is_final, the trailing pieces that never
// reached a closing boundary are held back (more audio may still arrive), so a 280 s tail waits too.
std::vector<std::pair<size_t, size_t>> plan_sections(const std::vector<PieceRef> &pieces, bool is_final,
                                                     int target = 300, int min_len = 240, int max_len = 360);

// plan_sections over pieces JSON ([{"start_sec":0,"text":"…"}], in transcript order), as the host has them.
// Made sane first: start_sec is a number >= 0 and never below the previous piece's; a piece ends where the next
// starts, the last at end_sec_of_last (never before its own start). Returns "[[first,last),…]" as JSON text,
// e.g. "[[0,11],[11,22]]"; "[]" for no pieces or input that is not an array.
std::string plan_sections_json(const std::string &pieces_json, int64_t end_sec_of_last, bool is_final);

// Section summary JSON: {"title","topic","points":[{"text","start_sec"}],"terms":[],"hints":[]}.
using SectionPieces = std::vector<std::pair<int64_t, std::string>>;  // (start_sec, text)

// A section keeps up to 10 points and the prompt asks to cover every part (N7: 3 to 6 points skipped whole
// stretches of a 12-piece section).
constexpr size_t kSectionMaxPoints = 10;
// Instruction asking for that shape in `lang` ("te" -> Telugu, else English); each piece is marked [t=SEC].
std::string section_prompt(const SectionPieces &pieces, const std::string &lang, const std::string &vocab_line);
// Strips fences/thinking, parses the first JSON object, clamps each start_sec to the nearest piece start,
// caps points at kSectionMaxPoints. False when there is no title or no point.
bool parse_section_summary(const std::string &model_out, const std::vector<int64_t> &piece_starts, nlohmann::json &out);
// Memory candidates for one section, asked on their own (the section summary prompt above is unchanged).
// kMemoryShape is the reply's shape line: {"memories":[{"type","text","who","to_whom","due_text","quote",
// "start_sec","names":[{"as_said","as_written"}],"numbers","dates","times"}]}.
extern const char *kMemoryShape;
// The memory rule lines ("- …\n" each): at most 5, text in `lang`, quote word for word, checked words, no medicines.
std::string memory_rules(const std::string &lang);
// Same transcript layout as section_prompt, reply in kMemoryShape.
std::string memory_prompt(const SectionPieces &pieces, const std::string &lang, const std::string &vocab_line);
// The raw items of the first "memories" array that are whole objects, in order, even when the reply is cut off
// inside the array (the cut item is left out). [] when there is no such array. Items still need
// parse_memory_candidates and grounding.
nlohmann::json complete_memory_items(const std::string &model_out);

// No-model summary: title = first 8 words, points = first sentence of up to 3 pieces, "fallback":true.
nlohmann::json fallback_section_summary(const SectionPieces &pieces);

// Stored sections ([{"start_sec","summary":{...}}], as sve_note_sections returns) to chapters
// [{"title","start_sec"}]: consecutive sections with the same normalised topic (trimmed, whitespace collapsed,
// lower-cased for Latin/Greek/Cyrillic; Telugu has no case) merge, the title comes from the run's first section.
nlohmann::json merge_chapters(const nlohmann::json &sections);

// Section summaries rendered as text blocks ("[m:ss] title" and "- point" lines), consecutive blocks joined into
// groups of at most budget_tokens by count_tokens. A single block over the budget is cut to fit.
std::vector<std::string> section_groups(const nlohmann::json &sections, int budget_tokens,
                                        const std::function<int(const std::string &)> &count_tokens);

}  // namespace sve
