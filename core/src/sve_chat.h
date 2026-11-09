// Ask as a chat (N9), pure parts: the language of a question, the earlier turns that go with a follow-up,
// and the visible answer text as it is written (thinking, citation labels and medicine sentences held back).
// sve_assistant_chat (sve_assistant.cpp) wires them to the store and the model.
#pragma once
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include "sve_context.h"  // Relevant
#include "sve_store.h"    // json

namespace sve {

// "te" when Telugu-script letters are at least 30% of the letters, else "en" (also with no letters at all).
std::string question_lang(const std::string &text);
// The answer language: the question's when it has letters; otherwise hint ("te"/"en"), otherwise fallback.
// Never the profile's when the question says otherwise.
std::string chat_lang(const std::string &question, const std::string &hint, const std::string &fallback);

struct ChatTurn {
    std::string role;  // "user" | "assistant"
    std::string text;
};

// messages_json ([{"role":"user"|"assistant","text":…}], newest last) as turns: other roles and empty texts are
// dropped. An assistant turn's "sources" ([{"title","start_sec"}]) are kept as text after it:
// "\n(Sources: Title at m:ss; …)". An assistant turn with "medicine":true is replaced by a placeholder, so a
// medicine answer (built in code, quoted word for word) is never handed to the model to reword.
std::vector<ChatTurn> chat_turns(const json &messages);

constexpr int kChatHistoryTokens = 1500;
// The earlier turns that fit budget_tokens, taken newest first and returned oldest first (contiguous, whole turns).
// When the newest one alone is over budget, it is cut (UTF-8 safe) to fit and is the only one.
std::vector<ChatTurn> history_window(const std::vector<ChatTurn> &earlier, int budget_tokens,
                                     const std::function<int(const std::string &)> &count_tokens);
// "Conversation so far (oldest first):\nPerson: …\nSummaverick: …\n", or "" for no turns.
std::string history_block(const std::vector<ChatTurn> &window);

// Words to search the recordings with: the question's keywords (keyword_query); a short follow-up (fewer than
// two keywords) adds the previous question's. The question itself when there are none.
std::string chat_search_query(const std::string &question, const std::vector<ChatTurn> &earlier);

// The user prompt of a recordings answer: the conversation (may be ""), the material (relevant().block), the
// question, in lang_name. Asks for [S<n>] labels and NOT_FOUND as sve_assistant_ask does.
std::string chat_recordings_prompt(const std::string &history, const std::string &material, const std::string &question,
                                   const std::string &lang_name);

// ---------------------------------------------------------------- N10: the right source for each question

// The kind of question, by a code rule first (in this order):
// - a medicine name or medicine word (is_medicine_question): "medicine", the code-built path; never anything else;
// - quiz words ("quiz", "test me", "next question"; క్విజ్, పరీక్ష, తర్వాతి ప్రశ్న): "quiz";
// - due words ("due", "remind…", "to-do", "what do I have", "deadline"; చేయాల్సి, గుర్తు చేయ, గడువు): "due";
// - period words (today, yesterday, this week, last week, this month; ఈ రోజు/ఈరోజు/నేడు, నిన్న, ఈ వారం,
//   గత వారం/పోయిన వారం, ఈ నెల): "summary" with [from, to) in local time, the week starting on Monday;
// - summary words with no period (key discussions, highlights, important points, summarise, recap; సారాంశం,
//   ముఖ్యాంశాలు, ముఖ్యమైన): "summary" over the last 7 days, today included.
// Anything else: kind "recordings" with by_rule false (retrieval, then the model's recordings/general call).
struct Route {
    std::string kind;
    int64_t from = 0, to = 0;  // summary only: epoch seconds, from included, to excluded
    bool by_rule = false;
};
Route route_question(const std::string &text, int64_t now, const std::vector<std::string> &medicine_names = {});

// The model's one-letter recordings/general reply (thinking stripped): true for "G".
bool classify_general(const std::string &reply);
// The classification prompt: answer R (about the person's own recordings, life, people, plans) or G (general).
std::string classify_prompt(const std::string &question);

// "Q1. …\nA1. …" (revision_questions as stored; markdown "**", "#", "-" ignored, answers may run on several lines)
// as (question, answer) pairs; a question with no answer is dropped.
std::vector<std::pair<std::string, std::string>> parse_revision_questions(const std::string &text);

struct QuizItem {
    std::string id;  // "<note id>:<n>", n counting from 1 in the note's questions
    std::string question, answer, title;
    int64_t note_id = 0;
};
// notes: [{"id","title","revision_questions"}] newest first. The first question not in `asked`, never one whose
// question or answer mentions a medicine. nullopt when none is left.
std::optional<QuizItem> pick_quiz(const json &notes, const std::vector<std::string> &asked,
                                  const std::vector<std::string> &medicine_names);
// How many questions pick_quiz could give with nothing asked.
size_t quiz_count(const json &notes, const std::vector<std::string> &medicine_names);

// "YYYY-MM-DD" plus days (negative too).
std::string ymd_add_days(const std::string &ymd, int days);

// What is due in the 7 days from today (today .. today+6), listed in code: open tasks with a due date in that window
// or before it (overdue, marked), then active reminders that ring in it (daily, twice daily and weekly ones still
// running; once ones on a day in it). sources gets {note_id,start_sec,title:""} of the rows from a recording.
// The "nothing due" sentence when empty. lang "te" -> Telugu.
std::string due_answer(const json &reminders, const json &tasks, const std::string &today, const std::string &lang,
                       json &sources);

// Text without "[unclear]" tags (sections stored by an earlier build may have them).
std::string strip_unclear(const std::string &text);
// The material of a period summary: per note "### Title (YYYY-MM-DD)" then its stored short summary, or else its
// section titles and points. notes: [{"title","created_at","short_summary","sections"}], oldest first.
std::string summary_material(const json &notes);
// The user prompt of a period summary (period: "2026-11-09 to 2026-11-15").
std::string summary_prompt(const std::string &history, const std::string &material, const std::string &period,
                           const std::string &question, const std::string &lang_name);
// The user prompt of a general-knowledge answer.
std::string general_prompt(const std::string &history, const std::string &question, const std::string &lang_name);

// How many of the keywords (keyword_query words) the text contains (case-insensitive for ASCII).
int keyword_hits(const std::string &keywords, const std::string &text);
// A passage scores when it has at least half of the keywords (rounded up), and at least one.
bool passage_scores(int hits, const std::string &keywords);

// A stored summary passage: a note's short summary (start 0) or a section's title and points.
struct Passage {
    int64_t note_id = 0, start_sec = 0;
    std::string title, text;
};
// Adds the scoring passages that mention no medicine (best first, at most max_n, within max_bytes of block) to rel
// as further [S<n>] labels under "Summaries of recordings:". Returns how many were added.
int add_passages(Relevant &rel, std::vector<Passage> candidates, const std::string &keywords,
                 const std::vector<std::string> &medicine_names, size_t max_bytes, size_t max_n = 4);

// Removes thinking blocks (Gemma 4 "<|channel>…<channel|>", "<think>…</think>") from text that arrives in pieces:
// text that could still be the start of a block is held until it cannot. An unclosed block hides the rest.
class ThinkStripper {
public:
    std::string feed(const std::string &piece);
    std::string finish();

private:
    std::string buf_;
    int open_ = -1;  // index of the open block's tags, -1 outside a block
};

// The visible answer, from the model's raw pieces: thinking removed, [S<n>] citation groups removed (with the
// spaces before them, as apply_citations does), outer whitespace trimmed. Released to the sink a sentence at a
// time (an end mark followed by a space, or a line end), so the pieces are whole UTF-8 and concatenate to
// apply_citations' answer. Stops for good, without releasing it, at a sentence that mentions a medicine
// (mentions_medicine) or says NOT_FOUND: that answer is then built in code. The sink returns false to cancel.
// SkipMedicine (summaries, general answers): a sentence that mentions a medicine is left out and the rest goes
// on; the answer is then shown() itself.
class AnswerStream {
public:
    enum Mode { HoldOnMedicine, SkipMedicine };
    AnswerStream(std::vector<std::string> medicine_names, std::function<bool(const std::string &)> sink,
                 Mode mode = HoldOnMedicine);
    // False once the sink has asked to cancel.
    bool feed(const std::string &raw);
    // Releases what is left (unless held back). False when cancelled.
    bool finish();
    const std::string &shown() const { return shown_; }
    bool cancelled() const { return cancelled_; }
    bool held() const { return held_; }
    // Nothing more is wanted: cancelled, or held (the answer is then built in code): generation can stop.
    bool done() const { return cancelled_ || held_; }

private:
    void cite(const std::string &visible);  // citation stage, into pending_
    void release(bool final);
    void flush_group();

    std::vector<std::string> names_;
    std::function<bool(const std::string &)> sink_;
    ThinkStripper think_;
    std::string group_;    // a "[S…" group being read
    size_t spaces_ = 0;    // spaces held before the next character
    bool started_ = false; // something visible has been seen
    std::string pending_;  // cited text not yet released
    std::string shown_;
    std::string carry_;    // SkipMedicine: whitespace after the last shown sentence, shown with the next one
    Mode mode_ = HoldOnMedicine;
    bool cancelled_ = false, held_ = false;
};

}  // namespace sve
