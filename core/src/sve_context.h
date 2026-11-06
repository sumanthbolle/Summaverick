// Pure context assembler: builds the "always" and "relevant" blocks fed to Gemma.
#pragma once
#include <string>
#include "sve_store.h"

namespace sve {

// Cut to at most max_bytes without splitting a UTF-8 code point.
std::string truncate_utf8(const std::string &s, size_t max_bytes);

// Profile, routine, medicines, preferences, vocab. Drops oldest vocab, then oldest
// preferences, when over budget; truncate_utf8 as the last resort. Empty if nothing known.
std::string always_block(Store &store, size_t max_bytes = 2000);

struct Relevant {
    std::string block;
    json sources;  // [{"label":"S1","note_id":..,"start_sec":..,"title":..}], exactly the labels in block
};

// Memory facts and transcript pieces matching `query`, within max_bytes. Whole items only
// (pieces first, then facts); an oversized first piece is included truncated.
Relevant relevant(Store &store, const std::string &query, size_t max_bytes);

std::string relevant_block(Store &store, const std::string &query, size_t max_bytes);
json relevant_sources(Store &store, const std::string &query, size_t max_bytes);

} // namespace sve
