// Learns preferences from repeated edits to Gemma suggestions. Pure: Store only.
#pragma once
#include <string>
#include "sve_store.h"

namespace sve {

// Logs one decision per changed field (text, times, when_text, due_date, repeat). When the same
// (kind, field, from -> to) has been logged at least twice, adds a pending "fact" suggestion
// proposing a preference, unless an equivalent suggestion or memory item already exists.
void record_edit(Store &store, const std::string &kind, const json &before, const json &after);

} // namespace sve
