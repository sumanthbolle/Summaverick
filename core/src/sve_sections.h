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

// A section keeps 3 to 6 points, drawn from the beginning, middle and end of the part. (N7 tried up to 10, one per
// line: on garbled audio that made more nonsense points, and the points still stopped early; rev 1 went back.)
constexpr size_t kSectionMaxPoints = 6;

// N7 rev 1: a piece heard with a conf (mean token probability) under 0.80 is marked "[unclear] " in the section and
// notes material, and the prompts say not to state it as fact. The text the person sees is never marked.
constexpr double kUnclearConf = 0.80;
extern const char *const kUnclearTag;  // "[unclear] "
// conf < 0.80. NaN (no conf: an older recording, or no token) is not unclear.
bool piece_unclear(double conf);
// text with kUnclearTag in front when piece_unclear(conf) and the text is not blank; else text unchanged.
std::string unclear_marked(const std::string &text, double conf);
// A piece's "conf" when it is a number, else NaN.
double piece_conf(const nlohmann::json &piece);
// The notes material of a note's pieces ([{"text","conf"?}], in order): the texts joined by "\n", each unclear
// one marked, the whole trimmed. With nothing unclear it is the transcript the app stores (pieces joined, trimmed).
std::string notes_material(const nlohmann::json &pieces);

// Saved vocabulary terms in a text (N7 rev 1: another note's "AI bubble" was listed as a term of note 20, so a saved
// term reaches a section, memory or notes prompt only when that note says it). Case-insensitive (ASCII); a Latin-
// script term (no code point past U+024F) must stand as whole words, so "IPO" is not said in "IPOs" (a Telugu
// ending such as "IPOలో" still counts); any other term (Telugu) is a substring match. The term is trimmed; "" is
// never said.
bool term_said(const std::string &term, const std::string &text);
// The vocab entries term_said in text, in their order.
std::vector<std::string> terms_said(const std::vector<std::string> &vocab, const std::string &text);
// After a section summary is parsed: drops every term, hint and point that mentions (term_said) a vocab entry
// that `said` (the section's own transcript text) does not. Returns how many were dropped.
int drop_unsaid_vocab(nlohmann::json &summary, const std::vector<std::string> &vocab, const std::string &said);

// A section's pieces as sve_section_summarize reads them ([{"start_sec","text","conf"?}]; blank and non-text
// pieces skipped): plain for the fallback and the vocabulary check, marked (unclear_marked) for the prompt.
struct SectionInput {
    SectionPieces plain, marked;
    std::vector<int64_t> starts;
    std::string said;  // the plain texts joined by "\n"
};
SectionInput section_input(const nlohmann::json &pieces);
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
// Every stored section rendered as in section_groups, joined: the text the notes from sections are written from.
std::string sections_text(const nlohmann::json &sections);

// N7 rev 1: an output cut at its token cap ends cleanly. The last line, unless it already ends a sentence (. ! ? ।
// or a closing bracket/quote after one), is cut after its last finished sentence when at least three words come
// before it, else dropped. Then trailing headings with nothing under them, a question without its answer, and a
// question whose answer was dropped go too. When nothing whole is left, the text is returned trimmed as it is.
std::string trim_cut_reply(const std::string &reply);

}  // namespace sve
