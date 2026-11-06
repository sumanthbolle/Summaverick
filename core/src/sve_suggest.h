#pragma once
// Pure assistant logic used by the C API (sve_assistant.cpp): turning model output into
// checked suggestions, extraction windows, Ask citations and the code-built medicine answer.
// No llama, mtmd or SQLite here, so all of it is unit-tested in core/tests/test_suggest.cpp.
#include <functional>
#include <string>
#include <vector>
#include "sve_extract.h"

namespace sve {

using nlohmann::json;

// Type-checked getters for model-produced JSON. Never throw.
std::string jstr(const json &o, const char *key);                 // string, else ""
int64_t jint(const json &o, const char *key, int64_t dflt);       // integer (or whole number), else dflt
json jarr(const json &o, const char *key);                        // array, else []
json jobj(const json &o, const char *key);                        // object, else null

bool valid_hhmm(const std::string &s);   // "HH:MM", 00..23 / 00..59
bool valid_ymd(const std::string &s);    // "YYYY-MM-DD"
std::string sanitize_repeat(const std::string &s);  // daily|twice_daily|weekly|once, else ""
// Valid "HH:MM" strings from a JSON array (others dropped, duplicates removed).
std::vector<std::string> valid_times(const json &arr);

// Times implied by meal/day words in when_text ("after dinner" -> routine dinner), sorted.
std::vector<std::string> resolve_times(const std::string &when_text, const json &routine);

// Groups pieces into windows whose "[t=SEC] text" lines total at most budget_tokens
// (counted line by line with `count`). A piece larger than the budget gets its own window.
std::vector<json> group_windows(const json &pieces, int budget_tokens,
                                const std::function<int(const std::string &)> &count);

// Concatenates reminders/tasks/profile/facts; dedupes vocab/topics/people case-insensitively;
// at most 3 topics and 5 people overall. ok when any part was ok.
Extracted merge_extracted(const std::vector<Extracted> &parts);

struct QuoteHit { bool found = false; int64_t start_sec = -1; };
// Finds the normalized quote as a contiguous token run in the cited piece (by start_sec),
// else in any piece of the note, else across two adjacent pieces. Empty quote: not found.
QuoteHit locate_quote(const json &pieces, const std::string &quote, int64_t cited_start_sec);

// True when the dose is non-empty and its last normalized token is a number (no unit).
bool dose_missing_unit(const std::string &dose);

// Builds pending suggestion records (kind, payload, quote, note_id, start_sec, check_status,
// check_fields) from extraction output. Every quote is located in `pieces`; every medicine
// is checked with check_medicine (drop -> item discarded). Topics/people are not included.
std::vector<json> build_suggestions(const Extracted &e, const json &pieces, int64_t note_id,
                                    const json &routine);

// Question -> words for a search: lowercased ASCII, stopwords and 1-letter words dropped.
std::string keyword_query(const std::string &question);

// Names (from `names`) that occur in the question, as normalized token runs.
std::vector<std::string> names_in_question(const std::string &question, const std::vector<std::string> &names);
// Medicine route: a known medicine name, or a medicine word (tablet, medicine, dose, pill,
// మందు, మాత్ర, డోస్ and their inflections) in the question.
bool is_medicine_question(const std::string &question, const std::vector<std::string> &names);

struct Cited {
    std::string answer;      // model text without [S<n>] markers
    json sources = json::array();  // [{"note_id","start_sec","title"}] in order of first citation
    bool not_found = true;
};
// Maps [S<n>] labels in the model output to `labelled_sources` (from relevant()).
// NOT_FOUND anywhere, or no valid citation -> not_found (answer and sources empty).
Cited apply_citations(const std::string &model_out, const json &labelled_sources);

std::string not_found_text(const std::string &lang);  // "te" -> Telugu, else English

// Code-built medicine answer: profile medicines, then quoted transcript pieces with their
// recording and time. Empty when there is nothing to say. lang "te" -> Telugu templates.
std::string medicine_answer(const json &medicines, const json &hits, const std::string &lang);

// {"summary":str,"key_terms":[{"term","meaning"}],"quiz":[{"q","a"}]} with only valid entries.
json sanitize_topic_summary(const json &j);

// True when text mentions a dose unit (mg, ml, ...) or one of the given medicine names.
bool mentions_medicine(const std::string &text, const std::vector<std::string> &names);

}  // namespace sve
