// One-pass notes: the title, summary, detailed notes and revision questions written in one reply under four
// headers; this splits that reply. Pure, no model.
#pragma once
#include <optional>
#include <string>

namespace sve {

struct AllParts { std::string title, summary, notes, questions; };

// text holds the header lines "### TITLE", "### SUMMARY", "### NOTES", "### QUESTIONS" in that order, each followed
// by its part. Text before the first header is ignored; parts are trimmed. A header missing or out of order, or
// followed by an empty part, gives nullopt.
std::optional<AllParts> split_all(const std::string &text);

} // namespace sve
