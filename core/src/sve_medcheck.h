#pragma once
// Code-enforced medicine check: every field Gemma proposes must be found in
// the exact quoted words. Pure C++; no llama, mtmd or SQLite.
#include <string>
#include <vector>

namespace sve {

struct MedFields { std::string name, dose, when_text, frequency, duration; };

struct MedCheck {
    std::string status;               // "ok" | "check" | "drop"
    std::vector<std::string> fields;  // names of fields not found in the quote
};

MedCheck check_medicine(const MedFields &f, const std::string &quote);

// Normalized tokens joined by single spaces (ASCII lowercase, number words and
// Telugu digits mapped to ASCII digits, punctuation removed).
std::string normalize_for_match(const std::string &s);

// The tokenizer behind normalize_for_match and check_medicine, exposed for
// text matching. split = false keeps letters and digits of one word together.
std::vector<std::string> med_tokens(const std::string &s, bool split = true);

// True when `needle` occurs as a contiguous run of tokens in `hay`.
bool contains_run(const std::vector<std::string> &hay, const std::vector<std::string> &needle);

// The medicine-name match used by check_medicine, on the raw value and quote.
bool name_ok(const std::string &value, const std::string &quote);

}  // namespace sve
