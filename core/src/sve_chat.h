// Ask as a chat (N9), pure parts: the language of a question, the earlier turns that go with a follow-up,
// and the visible answer text as it is written (thinking, citation labels and medicine sentences held back).
// sve_assistant_chat (sve_assistant.cpp) wires them to the store and the model.
#pragma once
#include <functional>
#include <string>
#include <vector>
#include "sve_store.h"  // json

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
class AnswerStream {
public:
    AnswerStream(std::vector<std::string> medicine_names, std::function<bool(const std::string &)> sink);
    // False once the sink has asked to cancel.
    bool feed(const std::string &raw);
    // Releases what is left (unless held back). False when cancelled.
    bool finish();
    const std::string &shown() const { return shown_; }
    bool cancelled() const { return cancelled_; }
    bool held() const { return held_; }

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
    bool cancelled_ = false, held_ = false;
};

}  // namespace sve
