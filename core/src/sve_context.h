// Pure context assembler: builds the "always" and "relevant" blocks fed to Gemma.
#pragma once
#include <string>
#include <vector>
#include "sve_store.h"

namespace sve {

// Cut to at most max_bytes without splitting a UTF-8 code point.
std::string truncate_utf8(const std::string &s, size_t max_bytes);

// Profile, routine, medicines, preferences, vocab. Drops oldest vocab, then oldest
// preferences, when over budget; truncate_utf8 as the last resort. Empty if nothing known.
// medicines = false leaves out the Medicines section (notes, titles, summaries: free text that
// is shown without a medicine check must not be handed the person's doses; spec §4).
// said (N7 rev 1): when given, only the vocabulary terms that text says (terms_said) are listed: the section,
// memory, notes and title prompts pass the material they are written from, so another note's words never reach them.
std::string always_block(Store &store, size_t max_bytes = 2000, bool medicines = true, const std::string *said = nullptr);

// `system` preceded by the always block as background (unchanged when nothing is known).
std::string context_system(Store &store, const std::string &system, bool medicines = true,
                           const std::string *said = nullptr);

// The saved vocabulary terms, oldest first.
std::vector<std::string> saved_vocab(Store &store);

// "Spell these words this way: a, b" from saved vocabulary, newest first, whole terms within
// max_bytes (UTF-8 safe). Vocabulary only: never medicines or doses, so the transcriber is not
// nudged to hear a dose that was not said (spec §7.4). Empty when there is no vocabulary.
// The transcription prompt's line (hearing help); other prompts use spelling_vocab_line.
std::string transcription_vocab_line(Store &store, size_t max_bytes = 400);

// Said next to the saved words wherever they are not hearing help (sections, memories, notes, the always
// block): they are spellings, not content. Another note's vocabulary ("S&P 500 index", "IPO") was listed
// as terms and hints of an unrelated recording (N7); rev 1 also leaves out, in code, every word the note does not say.
extern const char *const kVocabOnlyIfSaid;

// The same words as transcription_vocab_line, with kVocabOnlyIfSaid: "Spell these words this way (<rule>): a, b".
// For the section and memory prompts. said: only the words that text says (terms_said; N7 rev 1: the instruction
// alone did not stop "AI bubble" being listed for a note that never said it). Empty when no word is left.
std::string spelling_vocab_line(Store &store, size_t max_bytes = 400, const std::string *said = nullptr);

struct Relevant {
    std::string block;
    json sources;  // [{"label":"S1","note_id":..,"start_sec":..,"title":..}], exactly the labels in block
};

// Memories (current, not wrong) and transcript pieces matching `query`, within max_bytes. Whole items only
// (pieces first, then facts); an oversized first piece is included truncated.
// to != 0: only pieces of notes created in [from, to), and no remembered facts (they have no period).
Relevant relevant(Store &store, const std::string &query, size_t max_bytes, int64_t from = 0, int64_t to = 0);

std::string relevant_block(Store &store, const std::string &query, size_t max_bytes);
json relevant_sources(Store &store, const std::string &query, size_t max_bytes);

} // namespace sve
