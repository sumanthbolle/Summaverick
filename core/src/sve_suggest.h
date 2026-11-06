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

std::string trim_copy(const std::string &s);  // without surrounding whitespace

bool valid_hhmm(const std::string &s);   // "HH:MM", 00..23 / 00..59
bool valid_ymd(const std::string &s);    // "YYYY-MM-DD"
// "Monday".."Sunday" for a valid YYYY-MM-DD, else "".
std::string weekday_name(const std::string &ymd);
// False only when the quote names exactly one weekday (English or Telugu) and the date falls on another.
bool date_matches_weekday(const std::string &ymd, const std::string &quote);
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
// is checked with check_medicine (drop -> item discarded); a date on a medicine reminder is
// flagged unless it is today_ymd or the quote names its weekday. Profile items without a quote
// are dropped (a profile change must be stated in the recording). Topics/people are not included.
// Reminder and profile-medicine text is built in code from the checked medicine fields (never the
// model's words); other text that mentions a medicine gets "text" in check_fields. Dates naming a
// weekday must be its nearest upcoming occurrence from today_ymd unless the quote says "next"/"వచ్చే".
// Medicine reminders and profile medicines without a time from the routine get "times"; those whose
// dose differs from the saved medicine of that name (saved_medicines) get "dose".
std::vector<json> build_suggestions(const Extracted &e, const json &pieces, int64_t note_id,
                                    const json &routine, const std::string &today_ymd = "",
                                    const std::string &lang = "en",
                                    const std::vector<std::string> &known_names = {},
                                    const json &saved_medicines = json::array());

// True when the quote says something a date can come from: a date, a weekday, today/tomorrow/tonight,
// "<number> day(s)/week(s)/month(s)", or the Telugu equivalents (ఈరోజు, రేపు, రోజు, వారం, నెల, ...).
bool quote_supports_date(const std::string &quote);
// True when the quote contains the hour of "HH:MM" ("9 pm", "21:00", "9:30", "9:30 pm", "తొమ్మిది"); a
// stated am/pm, also after the minutes, must match.
bool quote_has_hour(const std::string &quote, const std::string &hhmm);

// "Take Amlodipine 5 mg" / "Amlodipine 5 mg వేసుకోండి".
std::string reminder_text(const std::string &name, const std::string &dose, const std::string &lang);
// "Add medicine: Amlodipine 5 mg" etc. for add_medicine / update_medicine / stop_medicine.
std::string profile_text(const std::string &change, const std::string &name, const std::string &dose,
                         const std::string &lang);

// Valid routine slots (wake, breakfast, lunch, dinner, bed -> "HH:MM") from a JSON object.
json clean_routine(const json &routine);

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

// Search hits (with "text") that contain one of the names as a whole token run.
json hits_naming(const json &hits, const std::vector<std::string> &names);

// For a medicine question that names no medicine: the saved list under "Your saved medicines:",
// followed by "Your recordings don't name a medicine for this question." Empty when no medicines.
std::string medicine_list_answer(const json &medicines, const std::string &lang);

// Removes summary sentences, key terms and quiz entries that mention a medicine. True if any removed.
bool filter_topic_summary(json &summary, const std::vector<std::string> &names);

// {"summary":str,"key_terms":[{"term","meaning"}],"quiz":[{"q","a"}]} with only valid entries.
json sanitize_topic_summary(const json &j);

// True when text mentions a dose unit (mg, ml, ...), a medicine word (tablet, medicine, pill, dose,
// మందు, మాత్ర, డోస్, ...) or one of the given medicine names.
bool mentions_medicine(const std::string &text, const std::vector<std::string> &names);

}  // namespace sve
