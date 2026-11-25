#pragma once
// Extraction prompt and tolerant parser for the on-device assistant.
// Pure C++; no llama, mtmd or SQLite.
#include <string>
#include "third_party/nlohmann/json.hpp"

namespace sve {

// Builds the prompt that asks the model for one JSON object of proposals.
std::string extraction_prompt(const std::string &context_block,
                              const std::string &transcript_with_markers,
                              const std::string &today_iso);

struct Extracted {
    nlohmann::json reminders = nlohmann::json::array();
    nlohmann::json tasks = nlohmann::json::array();
    nlohmann::json profile = nlohmann::json::array();
    nlohmann::json vocab = nlohmann::json::array();
    nlohmann::json facts = nlohmann::json::array();
    nlohmann::json topics = nlohmann::json::array();
    nlohmann::json people = nlohmann::json::array();
    // The next meeting the recording sets ({"text","when_text","quote","start_sec"}), or null.
    nlohmann::json next_meeting = nullptr;
    bool ok = false;
};

// Never throws. On any failure ok=false and every array is empty.
Extracted parse_extraction(const std::string &model_output);

// First balanced {...} in text that parses as a JSON object (fences and prose around it ignored).
// Never throws.
bool first_json_object(const std::string &text, nlohmann::json &out);

// pieces: [{"start_sec":int,"text":str,...}, ...] -> "[t=<sec>] <text>\n" per piece.
std::string pieces_with_markers(const nlohmann::json &pieces);

// A recording's "people" are individual people: a name of at most four words, no digits, not a company, product,
// organisation, acronym or description ("one of the managers ...", "my manager"). The model also returned Google,
// Perplexity and "l three harris technologies" as people (2026-11-21). Those go in as topics instead.
bool looks_like_person_name(const std::string &name);
// A description of someone ("one of the managers ...", "my manager"), not a name of anyone or anything: dropped.
bool looks_like_description(const std::string &name);

}  // namespace sve
