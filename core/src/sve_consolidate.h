#pragma once
// Consolidation of the knowledge graph across recordings: the same person under Telugu and English
// spellings, and the same, an updated or a contradicting memory. People are never merged silently: a
// name that only may be someone already known becomes a new person plus a question for the person.
// The pure rules are tested in core/tests/test_consolidate.cpp; the Store helpers below them write.
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>
#include "sve_store.h"  // Store, json

namespace sve {

// Telugu letters to Latin: vowels, consonants and vowel signs, with the inherent "a" unless a sign or the
// virama follows; ం -> "m". Other characters pass unchanged. The output is normalised like the Latin side
// (person_norm), so long vowels come out short.
std::string transliterate_te(const std::string &s);

// One name word in the shared Latin form: lower case, aa->a, ee->i, oo->u, doubled letters collapsed,
// w->v, plus the folds both scripts need to meet (see the .cpp).
std::string person_norm(const std::string &latin_word);

// nfc_clean, then the honorifics (dr, dr., mr, mrs, ms, sir, madam, garu, గారు, అన్న, అక్క) and the last
// Telugu word's case ending dropped, then transliterate_te and person_norm per word, joined by spaces. Only a clear
// ending is dropped (see match_person): an ending that may be the name's own stays in the key (రజని, వాసుకి).
std::string person_key(const std::string &name);

enum class PersonMatch { same, maybe, different };
// same: the normalised names are equal, or their keys are, with the same honorifics, where removing a case ending
// counts only for clear endings (కు, తో, లో, గారు ..., and కి after ి ీ ె ే). Removing ని, ను, ది, దే, కే or another
// కి (రజని is not Raja, జానకి is not Jana), a name
// of honorifics only, a word subset or one letter apart (keys of 4+ letters): maybe. Else different.
PersonMatch match_person(const std::string &a, const std::string &b);

// The model's same-person check, answered with parse_yes_no.
std::string same_person_prompt(const std::string &a_name, const std::string &a_context, const std::string &b_name,
                               const std::string &b_context);
extern const char *kSamePersonSystem;

enum class Pair { unrelated, same, update, contradiction };
// Rows in the memory_items shape (type, text, checked, said_date, people:[{id}], topics:[{id}]).
// cosine < 0 when unknown (no embeddings yet).
Pair classify_pair(const json &newer, const json &older, double cosine);

// {"was","now","said"} from the first checked number or date field whose canonical facts differ, in the
// words stored; null when none differs.
json change_line(const json &older, const json &newer);

// `row` plus "change" (change_line against history[0], the row it replaced directly) when it has history.
json with_change(json row);

// The current row a new memory matches, if any. `current`: rows of the same type, newest first. A row said
// later than the new one is matched the other way round: the new one is then the older version
// (newer_is_older), and it is stored already superseded.
struct Placement {
    Pair pair = Pair::unrelated;
    int64_t older = 0;
    bool newer_is_older = false;
};
Placement place_memory(const json &newer, const json &current, double cosine = -1);

// After the new row is stored as new_id: update supersedes the older one (or the new one, newer_is_older);
// contradiction queues both. A "same" placement adds the source, and the new mention's people and topic
// links, to the older row instead (merge_same_memory) and stores no new row.
bool apply_placement(Store &st, const Placement &p, int64_t new_id);
bool merge_same_memory(Store &st, int64_t older, int64_t note_id, int64_t start_sec, const std::string &quote,
                       const json &conf /* number or null */, int64_t now,
                       const std::vector<std::pair<int64_t, std::string>> &links = {} /* (topic id, role) */);

// Model answers for names that may be someone known: (name, existing topic id) -> 1 yes, 0 no, -1 unclear.
// A pair with no answer (no model) counts as unclear.
using PersonAnswers = std::map<std::pair<std::string, int64_t>, int>;

// The (name, topic) pairs that need a same-person check: names no known person matches as "same", against
// every known person they match as "maybe". person_topics: Store::topics("person").
std::vector<std::pair<std::string, json>> person_checks(const std::vector<std::string> &names, const json &person_topics);

// The person topic for a spoken name, linked to the note: a known person who is the "same", else a new
// topic, plus a pending person_questions row against every known "maybe" the answers do not rule out.
// 0 for a name that names nobody (empty, too long, a pronoun, the person themselves), -1 on error.
int64_t resolve_person(Store &st, int64_t note_id, const std::string &name, const std::string &self_lower,
                       const std::string &context, const PersonAnswers &answers);

}  // namespace sve
