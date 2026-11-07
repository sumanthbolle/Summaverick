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

// Section summary JSON: {"title","topic","points":[{"text","start_sec"}],"terms":[],"hints":[]}.
using SectionPieces = std::vector<std::pair<int64_t, std::string>>;  // (start_sec, text)

// Instruction asking for that shape in `lang` ("te" -> Telugu, else English); each piece is marked [t=SEC].
std::string section_prompt(const SectionPieces &pieces, const std::string &lang, const std::string &vocab_line);
// Strips fences/thinking, parses the first JSON object, clamps each start_sec to the nearest piece start,
// caps points at 6. False when there is no title or no point.
bool parse_section_summary(const std::string &model_out, const std::vector<int64_t> &piece_starts, nlohmann::json &out);
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
