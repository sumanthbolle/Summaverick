#pragma once
// The per-recording checking agent (owner, 2026-11-24: "retry until the parent workflow is confident"). After a
// recording or import is processed, the parent checks every output with measurable checks, has only the failing part
// written again with what failed as feedback (at most kMaxRetries times), keeps the attempt with the fewest failures
// and marks "Please check" when it still fails. The pattern of refine_actions (sve_suggest.h): code checks first, a
// yes/no model check (Skill::verify) only where code cannot decide (a line in another language than the transcript,
// a question with few words in common with it), and workers that change nothing when their reply does not parse.
//
// Checks:
//   language   summary, notes, questions are in the note's language (Telugu letters share; English terms allowed)
//   grounded   every line under the notes' decision and action headings is said in the transcript
//   owner      such a line that names a speaker names the one who said it (or was given it)
//   noise      no output line repeats (a loop), and none is written from noise: a transcript of fewer than 4 said
//              words, or a saved word that only occurs in pieces clean_piece drops (saved words written as speech)
//   answerable every revision question's answer is said in the transcript
//   quote      every memory and every action item has its quote in its recording (a worker copies the line again)
// Pure: no llama or SQLite; the model and the rewriting are passed in (sve_verify_note in sve_core.cpp).
#include <functional>
#include <string>
#include <vector>
#include "sve_suggest.h"

namespace sve {

constexpr int kMaxRetries = 2;
constexpr int kMaxJudgeCalls = 16;   // yes/no and copy-the-line calls of one run (each is a prefill on the phone)

// ---- the code checks (exported for the tests) ------------------------------------------------------------

// Share of Telugu letters among all letters (0 when there are none).
double telugu_share(const std::string &text);
// "te": at least 30% Telugu letters; any other language: at most 30% (bracketed terms in the other script are
// allowed). Text with fewer than 12 letters passes.
bool in_language(const std::string &text, const std::string &lang);

// The bullet lines under the notes' decision and action headings ("## Decisions and instructions", "## Action
// items", their demoted or Telugu forms), without the bullet, the checkbox and the "— who — when" fields that
// say "not stated". Lines that say "None" are left out.
std::vector<std::string> decision_action_lines(const std::string &notes);

// Content words of `line` (match_tokens, 4+ letters, stop words and the notes' own filler left out) found in the
// transcript: at least 75% of them, and every number. Fewer than two content words: true (nothing to measure).
// said_check: 1 said, 0 not enough words found (a yes/no check decides), -1 a number never said (fails in code).
int said_check(const std::string &line, const json &pieces);
bool said_in(const std::string &line, const json &pieces);

// A decision or action line naming one speaker of the transcript while the line it comes from (the most words in
// common, the names left out) was said by another, who did not give it to them: that speaker, else "".
std::string wrong_owner(const std::string &line, const json &pieces);

struct QA { std::string q, a; };
// The "Q1. … / A1. …" pairs of a questions output (labels as count_questions reads them).
std::vector<QA> parse_qa(const std::string &questions);

// How many words of the transcript remain after clean_piece (loops, repeats, saved words as speech dropped).
int said_word_count(const json &pieces, const std::vector<std::string> &vocab);
// Lines of `output` written from noise: a line (12+ bytes) that occurs twice, a line clean_piece's loop collapse
// shortens, and a line naming a saved word that the transcript says only in pieces clean_piece drops.
// The saved words the transcript says only in pieces clean_piece drops (written as speech on near-silence).
std::vector<std::string> noise_terms(const json &pieces, const std::vector<std::string> &vocab);
// The pieces as clean_piece keeps them (a piece it empties is left out): what a worker writes again from.
json clean_pieces(const json &pieces, const std::vector<std::string> &vocab);
std::vector<std::string> noise_lines(const std::string &output, const json &pieces, const std::vector<std::string> &vocab);

// The transcript lines a yes/no check reads for `item`: the whole transcript when it is under max_bytes, else the
// three pieces sharing the most words with it, each with the two pieces around it, in order.
std::string judge_context(const std::string &item, const json &pieces, size_t max_bytes = 6000);

// ---- the parent ------------------------------------------------------------------------------------------

struct VerifyEvent {
    std::string part;     // summary | notes | questions | memory | action
    std::string check;    // language | grounded | noise | answerable | quote | all
    std::string action;   // check | retry | kept | please_check | fixed
    int attempt = 0;      // 0: as written; 1..kMaxRetries: a retry
    int total = 0, failed = 0;
    int64_t id = 0;       // memory or suggestion id
};

struct Failure { std::string check, item; };

struct PartVerdict {
    std::string part;
    int retries = 0;
    int chosen = 0;               // the attempt kept (0: as written)
    bool please_check = false;
    std::vector<Failure> failures;  // of the attempt kept
};

struct VerifyInput {
    json pieces = json::array();
    std::string lang = "en";
    std::vector<std::string> vocab;
    std::string summary, notes, questions;   // "" = not written, not checked
    json memories = json::array();   // [{"id","text","quote","start_sec"}]
    json actions = json::array();    // [{"id","text","quote","start_sec"}]
};

struct VerifyResult {
    std::string summary, notes, questions;   // as kept
    std::vector<PartVerdict> parts;
    json memory_fixes = json::array();    // [{"id","quote","start_sec"}]: the line found again
    json memories_flagged = json::array();  // [id]
    json action_fixes = json::array();    // [{"id","quote","start_sec"}]
    json actions_flagged = json::array(); // [id]
    std::vector<VerifyEvent> events;
    int judge_calls = 0, unjudged = 0;    // unjudged: items left unmeasured once kMaxJudgeCalls was reached
};

// Writes `part` (summary | notes | questions) again with `feedback` added to its prompt; `failing` are the lines
// that failed (the notes worker rewrites only the parts that hold them). attempt: 1..kMaxRetries; base: the attempt
// kept so far (0: as written), the text the failing lines are in and the next attempt builds on. False: nothing.
using RewriteFn = std::function<bool(const std::string &part, int attempt, int base, const std::string &feedback,
                                     const std::vector<std::string> &failing, std::string &out)>;
using EventFn = std::function<void(const VerifyEvent &)>;

// The failures of one output as the parent measures them (judge may be empty: code checks only).
std::vector<Failure> check_output(const std::string &part, const std::string &text, const VerifyInput &in,
                                  const ModelFn &judge, int &judge_calls, int &unjudged);
// The feedback a worker gets for these failures, in plain words (the lines quoted).
std::string feedback_for(const std::string &part, const std::vector<Failure> &failures, const std::string &lang_name,
                         const std::vector<std::string> &not_said = {});

VerifyResult verify_outputs(const VerifyInput &in, const ModelFn &judge, const RewriteFn &rewrite, const EventFn &on_event);

}  // namespace sve
