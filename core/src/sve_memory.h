#pragma once
// Memory candidates from section summaries: shape check, grounding against the section's own
// words, the yes/no double-check, the review-queue rule and importance.
// A wrong memory is worse than a missing one: every rule here drops or queues when in doubt.
// Pure C++; no llama, mtmd or SQLite. Tested in core/tests/test_memory.cpp.
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include "sve_sections.h"  // SectionPieces, json

namespace sve {

using nlohmann::json;

// Memories go straight to the map only after the accuracy gate passes (Task 8).
constexpr bool kDirectToMapDefault = false;
// Pieces with a transcript confidence below this mark a memory "low". 0.0 never flags (Task 8).
constexpr double kLowConfidence = 0.0;

// Up to 8 valid candidates: type is one of fact/decision/promise/question/person/idea
// ("open question" -> question), text non-empty and at most 300 bytes, quote a string.
// Output keys: type, text, quote, who, to_whom, due_text (strings, "" when missing),
// numbers, dates, times (arrays of non-empty strings), start_sec (>= 0, default 0), and
// names: [{"as_said": the quote's words, "as_written": the form the text uses}] (a plain string is both).
json parse_memory_candidates(const json &arr);

struct Grounded {
    bool ok = false;
    // Why it was dropped: medicine, quote_too_short, quote_not_found, field_not_in_quote:<kind>,
    // text_not_in_quote, text_name_not_in_quote.
    std::string reason;
    // type,text,who,to_whom,due_text,due_date,said_date,checked,confidence,health,sources:[{start_sec,quote,conf}].
    // checked: [{"kind","text"}], text in the quote's words; names also carry "as_written".
    // sources[0].quote is the transcript's own words that matched, not the model's copy.
    json row;
    // Names, numbers, dates, promises, and every memory that could skip the review queue.
    bool needs_double_check = false;
};

// Checks one candidate against the section's pieces (in order). piece_conf is per piece (nullopt = unknown,
// counted as high); said_ymd is the local date the recording was made, used for relative due dates.
Grounded ground_memory(const json &cand, const SectionPieces &pieces, const std::vector<std::optional<double>> &piece_conf,
                       const std::string &said_ymd, const std::vector<std::string> &med_names, double low_conf_threshold);

// "I", "we", "you", "he", "she", "they" and their Telugu forms: a who/to_whom that names nobody.
bool is_pronoun(const std::string &who);

// Asks the model whether the quote says the memory (its text, who, to whom and deadline), answered yes or no.
std::string double_check_prompt(const json &row);

// The double_check value for a memory whose yes/no check was not run.
constexpr int kNotDoubleChecked = -2;

// 1 for yes/అవును, 0 for no/కాదు/లేదు, -1 otherwise. Takes text with thinking already stripped.
// The caller drops a memory on 0.
int parse_yes_no(const std::string &model_out);

// True when the memory waits for the person: health, low confidence, a double-check that is not a
// clear yes (-1, or kNotDoubleChecked), a conflict, or direct-to-map not allowed. Only a "yes" shows
// a memory without review.
bool goes_to_queue(const json &row, int double_check, bool direct_allowed);

// Orders the map, recaps and practice; never hides or deletes. people_met_max: most notes any linked
// person appears in. Halves every 60 days since max(last_seen, last_used) (unix seconds).
double memory_importance(const json &row, int people_met_max, int practice_successes, int64_t now);

// The one rule for what the map and a person's or topic's list may show: current (not superseded), not wrong,
// not a medicine's, and either checked (not queued, review kept or corrected, or any review but wrong when
// kDirectToMapDefault) or waiting (memory_waiting) and neither health-related nor a conflict (those are
// decided in the review queue first). memory_map_review_sql is the same rule as an SQL fragment
// over alias m (starts with " AND ").
bool memory_on_map(const json &row);
const char *memory_map_review_sql();
// Waiting in the review queue, not checked yet (queue 1, review new): the map draws it lighter.
bool memory_waiting(const json &row);

}  // namespace sve
