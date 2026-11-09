// Ask as a chat (N9): the question's language, the history window, and the streamed answer text.
#include "check.h"
#include "sve_chat.h"
#include "sve_suggest.h"

using namespace sve;

namespace {
// One token per 4 bytes, at least 1: a stand-in for the model's tokenizer.
int fake_tokens(const std::string &s) { return (int)s.size() / 4 + 1; }

// Feeds raw in pieces of n bytes; returns what the sink received.
std::string stream_in(const std::string &raw, size_t n, const std::vector<std::string> &meds = {},
                      std::vector<std::string> *pieces = nullptr) {
    std::string got;
    AnswerStream s(meds, [&](const std::string &p) {
        got += p;
        if (pieces) pieces->push_back(p);
        return true;
    });
    for (size_t i = 0; i < raw.size(); i += n) s.feed(raw.substr(i, n));
    s.finish();
    CHECK_EQ(got, s.shown());
    return got;
}
}  // namespace

TEST(question_lang_by_script) {
    CHECK_EQ(question_lang("Highlight this week key discussions to remember"), std::string("en"));
    CHECK_EQ(question_lang("ఈ వారం ముఖ్యమైన చర్చలు ఏమిటి?"), std::string("te"));
    // mixed: the dominant script, at least 30% Telugu letters gives Telugu
    CHECK_EQ(question_lang("నా meeting ఎప్పుడు?"), std::string("te"));
    CHECK_EQ(question_lang("What did Ravi say about the budget meeting నిన్న"), std::string("en"));
    CHECK_EQ(question_lang("123 ?!"), std::string("en"));
    CHECK_EQ(question_lang(""), std::string("en"));
    // digits and punctuation are not letters: "క 12345" is all Telugu letters
    CHECK_EQ(question_lang("క 12345 !!!"), std::string("te"));
}

TEST(chat_lang_question_first) {
    CHECK_EQ(chat_lang("What is due?", "te", "te"), std::string("en"));   // never the profile's
    CHECK_EQ(chat_lang("ఏమిటి?", "en", "en"), std::string("te"));
    CHECK_EQ(chat_lang("123", "te", "en"), std::string("te"));            // no letters: the hint
    CHECK_EQ(chat_lang("123", "", "te"), std::string("te"));              // no hint: the fallback
    CHECK_EQ(chat_lang("123", "xx", "en"), std::string("en"));
}

TEST(chat_turns_parse_and_sources_as_text) {
    json m = json::parse(R"([
        {"role":"user","text":"What did Ravi promise?"},
        {"role":"assistant","text":"He will send the quotation.","sources":[{"title":"Plumber visit","start_sec":95}]},
        {"role":"system","text":"ignored"},
        {"role":"user","text":"   "},
        {"role":"assistant","text":"Take Dolo 650 twice.","medicine":true},
        {"role":"user","text":"When?"}])");
    std::vector<ChatTurn> t = chat_turns(m);
    CHECK_EQ(t.size(), (size_t)4);
    if (t.size() != 4) return;
    CHECK_EQ(t[0].role, std::string("user"));
    CHECK_EQ(t[1].text, std::string("He will send the quotation.\n(Sources: Plumber visit at 1:35)"));
    // a medicine answer is never handed to the model to reword
    CHECK(t[2].text.find("Dolo") == std::string::npos);
    CHECK_EQ(t[3].text, std::string("When?"));
    CHECK(chat_turns(json::parse("{}")).empty());
    CHECK(chat_turns(json()).empty());
}

TEST(history_window_keeps_newest_within_budget) {
    std::vector<ChatTurn> earlier;
    for (int i = 0; i < 40; ++i)
        earlier.push_back({i % 2 ? "assistant" : "user", "turn " + std::to_string(i) + " " + std::string(200, 'x')});
    std::vector<ChatTurn> w = history_window(earlier, kChatHistoryTokens, fake_tokens);
    CHECK(!w.empty());
    CHECK(w.size() < earlier.size());
    int used = 0;
    for (const auto &t : w) used += fake_tokens(t.text);
    CHECK(used <= kChatHistoryTokens);
    // oldest first, ending with the newest earlier turn, contiguous
    CHECK_EQ(w.back().text, earlier.back().text);
    size_t first = earlier.size() - w.size();
    for (size_t i = 0; i < w.size(); ++i) CHECK_EQ(w[i].text, earlier[first + i].text);
    // one more turn would not have fitted
    CHECK(used + fake_tokens(earlier[first - 1].text) > kChatHistoryTokens);

    // everything fits: all of it
    std::vector<ChatTurn> few = {{"user", "a"}, {"assistant", "b"}};
    CHECK_EQ(history_window(few, kChatHistoryTokens, fake_tokens).size(), (size_t)2);
    CHECK(history_window({}, kChatHistoryTokens, fake_tokens).empty());

    // the newest turn alone is too long: it is cut to fit, and nothing older comes before it
    std::vector<ChatTurn> big = {{"user", "old"}, {"assistant", std::string(20000, 'y')}};
    std::vector<ChatTurn> wb = history_window(big, kChatHistoryTokens, fake_tokens);
    CHECK_EQ(wb.size(), (size_t)1);
    if (!wb.empty()) CHECK(fake_tokens(wb[0].text) <= kChatHistoryTokens && wb[0].text.size() > 1000);
}

TEST(history_block_labels_turns) {
    CHECK_EQ(history_block({}), std::string(""));
    CHECK_EQ(history_block({{"user", "Hi"}, {"assistant", "Hello"}}),
             std::string("Conversation so far (oldest first):\nPerson: Hi\nSummaverick: Hello\n"));
}

TEST(chat_search_query_follow_up_uses_previous_question) {
    std::vector<ChatTurn> earlier = {{"user", "What did Ravi say about the quotation?"}, {"assistant", "He will send it."}};
    CHECK_EQ(chat_search_query("When is the plumber coming back?", earlier), std::string("plumber coming back"));
    // a short follow-up carries the earlier question's words
    CHECK_EQ(chat_search_query("And when?", earlier), std::string("ravi quotation"));
    CHECK_EQ(chat_search_query("Why?", {}), std::string("Why?"));
}

TEST(think_stripper_across_piece_edges) {
    const std::string raw = "<|channel>thought\nplanning here<channel|>Hello <think>hidden</think>world.";
    for (size_t n = 1; n <= raw.size(); ++n) {
        ThinkStripper t;
        std::string out;
        for (size_t i = 0; i < raw.size(); i += n) out += t.feed(raw.substr(i, n));
        out += t.finish();
        CHECK_EQ(out, std::string("Hello world."));
    }
    // an unclosed block hides everything after it; a "<" that opens no tag is shown
    ThinkStripper u;
    std::string o = u.feed("a <b> c <thi") + u.feed("nk>never shown");
    o += u.finish();
    CHECK_EQ(o, std::string("a <b> c "));
}

TEST(answer_stream_concatenates_to_the_cited_answer) {
    const std::string raw =
        "<|channel>thought\nThe material says Friday.<channel|>\n"
        "Ravi will send the quotation by Friday evening [S1]. The kitchen pipe must be replaced soon [S2, S1].\n"
        "ఈ వారం పని ముగుస్తుంది [S2].\n";
    json labelled = json::parse(R"([{"label":"S1","note_id":1,"start_sec":30,"title":"A"},
                                    {"label":"S2","note_id":1,"start_sec":0,"title":"A"}])");
    // the stripped text, cited the way sve_assistant_ask cites it
    const std::string visible = raw.substr(raw.find("<channel|>") + 10);
    Cited c = apply_citations(visible, labelled);
    CHECK(!c.not_found);
    for (size_t n : {1, 2, 3, 5, 7, 16, 1000}) {
        std::vector<std::string> pieces;
        std::string got = stream_in(raw, n, {}, &pieces);
        CHECK_EQ(got, c.answer);
        for (const auto &p : pieces) {
            CHECK(p.find("[S") == std::string::npos);
            CHECK(p.find("thought") == std::string::npos);
            // every piece is whole UTF-8 (Telugu split by bytes is never sent half)
            unsigned char last = p.empty() ? 0 : (unsigned char)p.back();
            CHECK(last < 0x80 || (last & 0xC0) != 0xC0);
        }
        if (n < 100) CHECK(pieces.size() >= 3);   // released as it is written, not all at the end
    }
}

TEST(answer_stream_holds_not_found_and_medicine) {
    CHECK_EQ(stream_in("NOT_FOUND", 2), std::string(""));
    CHECK_EQ(stream_in("<think>x</think>  NOT_FOUND\n", 3), std::string(""));
    // a sentence that mentions a medicine is never shown; nothing after it either
    std::string got = stream_in("The doctor visit was on Monday [S1]. Take Dolo 650 twice [S1]. Then rest [S1].", 4, {"Dolo 650"});
    CHECK_EQ(got, std::string("The doctor visit was on Monday."));
    CHECK_EQ(stream_in("You need 5 mg daily [S1].", 3), std::string(""));
}

TEST(answer_stream_cancel) {
    int calls = 0;
    AnswerStream s({}, [&](const std::string &) { return ++calls < 2; });
    bool ok = true;
    const std::string raw = "One. Two. Three. Four. Five.";
    for (size_t i = 0; i < raw.size() && ok; ++i) ok = s.feed(raw.substr(i, 1));
    CHECK(!ok);
    CHECK_EQ(calls, 2);
    CHECK(s.cancelled());
    CHECK(!s.finish());   // stays cancelled
}
