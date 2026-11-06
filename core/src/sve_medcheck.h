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

}  // namespace sve
