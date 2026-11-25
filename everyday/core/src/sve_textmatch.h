#pragma once
// Telugu and English text matching for memories: canonical forms for numbers,
// weekdays, months and relative days, name matching with Telugu case endings,
// and spoken-date resolution against the date a recording was made.
// Pure C++; no llama, mtmd or SQLite.
#include <string>
#include <vector>

namespace sve {

// Telugu canonical composition (U+0C46 U+0C56 -> U+0C48), canonical order of
// Telugu combining marks, and removal of ZWJ/ZWNJ. Other scripts pass unchanged.
std::string nfc_clean(const std::string &s);

// nfc_clean, then the medicine tokenizer, then canonical forms: numbers as
// digits, weekdays wd1..wd7 (Monday = 1), months mo1..mo12, relative words
// rd0/rd1/rd2/rd-1/rd-2, rw0/rw1 (weeks), rm1 (next month).
std::vector<std::string> match_tokens(const std::string &s);

// The match_tokens that are numbers or wd/mo/rd/rw/rm tokens, in order.
std::vector<std::string> canonical_facts(const std::string &text);

extern const std::vector<std::string> kTeluguCaseEndings;

// True when the medicine name match accepts `name` in `text`, or when a word of
// the text is the name's last word plus a Telugu case ending (the name's other
// words directly before it).
bool name_in(const std::string &name, const std::string &text);

// "YYYY-MM-DD" for a spoken date counted from the day it was said, or "" when
// the words hold no single date this can resolve.
std::string resolve_spoken_date(const std::string &words, const std::string &said_ymd);

}  // namespace sve
