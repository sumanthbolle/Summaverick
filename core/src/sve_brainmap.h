#pragma once
// The mind map's data: one centre (you, or one recording) with branches of people, topics, promises,
// decisions and questions. Pure C++ over JSON; no SQLite. Tested in core/tests/test_brainmap.cpp.
#include <cstdint>
#include "sve_sections.h"  // json

namespace sve {

using nlohmann::json;

// memories: memory rows as Store::memory_items gives them, each also with "topics":[{"id"}].
// people/topics: [{id,kind,name}]. meaning_pairs: [{a,b,cos}] of memory ids.
// Returns {"center":{"kind":"you"|"recording","note_id"},"list":bool,"branches":[{"kind","items":[{"ref":"m12"|"t3",
// "label","importance","new","low_confidence","done","count","waiting"}],"more":N}],"links":[{"a","b","kind"}]}.
// Only memories memory_on_map allows. "waiting": a memory still in the review queue, or a person/topic whose
// shown memories all are; within a branch, importance first, then checked before waiting. Counts, "more" and
// "list" include waiting ones. Empty branches are left out. Facts and ideas are not branches: they are counted
// under their topic.
json build_brain_map(const json &memories, const json &people, const json &topics, const json &meaning_pairs,
                     int64_t note_id, int per_branch = 6);

}  // namespace sve
