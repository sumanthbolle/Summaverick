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
//   meaning    a summary sentence, a decision or action line, an answer, or a decision/question/promise memory (the
//              map's relationship) does not say the opposite of the transcript or state as settled what it leaves
//              open; only a judge "yes" clears it
//   who        a relationship's person is the one the transcript gives it to
//   binding    a line's person, action and date are said together (binding_conflict); only a judge "yes" clears it
//   consistency the outputs agree with each other: of two claims about the same thing, one negated or left open and
//              the other not, the one the transcript contradicts (by code) while it backs the other fails; any other
//              disagreement is left to each claim's own source check (no judge: measured false flags)
// Pure: no llama or SQLite; the model and the rewriting are passed in (sve_verify_note in sve_core.cpp).
#include <functional>
#include <string>
#include <vector>
#include "sve_suggest.h"

namespace sve {

constexpr int kMaxRetries = 2;
constexpr int kMaxJudgeCalls = 16;
constexpr size_t kMaxEvidence = 60;   // yes/no and copy-the-line calls of one run (each is a prefill on the phone)

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
// said_check: 1 said, 0 not enough words found (a yes/no check decides), -1 a number never said (fails in code),
// -2 said with the opposite meaning (only a judge "yes" clears it).
int said_check(const std::string &line, const json &pieces);
bool said_in(const std::string &line, const json &pieces);
// negated: the clause says "not", "never", "n't", "లేదు"... ; polarity_conflict: the closest transcript clause has the
// opposite polarity ("did not approve" against "approved"). said_check returns -2 for such a line.
bool negated(const std::string &clause);
bool polarity_conflict(const std::string &line, const json &pieces);
// undecided: the clause leaves the matter open (pending, proposed, asked, "?"). undecided_conflict: the line states as
// settled what a transcript clause sharing one of its words leaves open ("Priya approved the budget" against
// "Approval is still pending"). said_check returns -2 for it too.
bool undecided(const std::string &clause);
bool undecided_conflict(const std::string &line, const json &pieces);

// Person, action and date together: the passage that says the line's action (its content words, names and dates
// left aside: the best piece and the pieces either side) must also say each of the line's dates and days and name
// or be said by the speaker the line names. "" when it does, else what is missing ("Monday", "Priya"). A line with
// no date and no speaker named, or no action words, is not measured here. "Fix the login bug — Priya — Monday"
// fails when Monday belongs to another speaker's task.
std::string binding_conflict(const std::string &line, const json &pieces);
// A memory's or action item's quote says something of its text (a content word in common). An invented line with a
// real but unrelated quote does not.
bool quote_supports(const std::string &text, const std::string &quote);

// The summary's sentences that make a claim (2+ content words), headings and bullets left out.
std::vector<std::string> summary_claims(const std::string &summary);
// Evidence: the start_sec of the piece that says the most of the claim's content words (half of them, or 2), else -1.
int64_t evidence_sec(const std::string &claim, const json &pieces);

// One claim of one output, for the check that the outputs agree with each other. part: summary | notes | questions
// | memory; id: the memory id (0 for the others).
struct Claim {
    std::string part, text;
    int64_t id = 0;
};
// Pairs of claims from different outputs (or two memories) about the same thing (2+ shared content words covering
// 60% of the shorter) where one is negated or left open and the other is not.
std::vector<std::pair<Claim, Claim>> conflicting_claims(const std::vector<Claim> &claims);

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

struct Failure {
    std::string check, item;
    int64_t start_sec = -1;   // the evidence: where in the recording the claim is said (or should be), -1 unknown
};

struct PartVerdict {
    std::string part;
    int retries = 0;
    int chosen = 0;               // the attempt kept (0: as written)
    bool please_check = false;
    std::vector<Failure> failures;  // of the attempt kept
    std::vector<Failure> unchecked; // of the attempt kept: claims no check could decide (no judge, no budget, no
                                    // clear reply). Never "passed": the note says it was only partly checked.
};

struct VerifyInput {
    json pieces = json::array();
    std::string lang = "en";
    std::vector<std::string> vocab;
    std::string summary, notes, questions;   // "" = not written, not checked
    json memories = json::array();   // [{"id","text","quote","start_sec","type"?,"who"?}]: decisions, questions and
                                     // promises are the map's relationships (who decided, asked or promised what)
    json actions = json::array();    // [{"id","text","quote","start_sec"}]
};

struct VerifyResult {
    std::string summary, notes, questions;   // as kept
    std::vector<PartVerdict> parts;
    json memory_fixes = json::array();    // [{"id","quote","start_sec"}]: the line found again
    json memories_flagged = json::array();  // [id]
    json action_fixes = json::array();    // [{"id","quote","start_sec"}]
    json actions_flagged = json::array(); // [id]
    // The map's relationships, checked once on the memory they come from: [{"memory_id","check","start_sec"}] with
    // check meaning (the opposite of the transcript, or settled where it is open) | who (another person said or
    // decided it) | consistency (an output says otherwise). Their memories are in memories_flagged too.
    json relation_flags = json::array();
    // Every checked claim with its evidence: [{"part","item","start_sec","status","ok"}] (at most kMaxEvidence);
    // status passed | failed | unchecked, ok = status == passed.
    json evidence = json::array();
    // Claims, memories, action items and relationships no check could decide: [{"part","item","start_sec","id"?}].
    json unchecked = json::array();
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
// unchecked (optional): the claims a judge was needed for and could not decide (no judge, no budget, no clear reply).
std::vector<Failure> check_output(const std::string &part, const std::string &text, const VerifyInput &in,
                                  const ModelFn &judge, int &judge_calls, int &unjudged,
                                  std::vector<Failure> *unchecked = nullptr);
// The feedback a worker gets for these failures, in plain words (the lines quoted).
std::string feedback_for(const std::string &part, const std::vector<Failure> &failures, const std::string &lang_name,
                         const std::vector<std::string> &not_said = {});

VerifyResult verify_outputs(const VerifyInput &in, const ModelFn &judge, const RewriteFn &rewrite, const EventFn &on_event);

}  // namespace sve
