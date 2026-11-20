// Saved words fix what the transcriber misheard (Shruti never sees them): close spellings, a word split in two,
// sound-alikes; never a common word into a different one, never a medicine (the medicine check reads what was said).
#include "check.h"
#include "sve_context.h"

using namespace sve;

namespace {
const std::vector<std::string> kNone;
std::string fix(const std::string &t, const std::vector<std::string> &terms, const std::vector<std::string> &keep = {}) {
    return vocab_correct(t, terms, keep);
}
}  // namespace

TEST(vocab_fixes_close_multiword_term) {
    CHECK_EQ(fix("we had a cement management call", {"incident management"}), std::string("we had a incident management call"));
}

TEST(vocab_joins_split_word_and_keeps_its_case) {
    CHECK_EQ(fix("check the charge bee invoices", {"ChargeBee"}), std::string("check the ChargeBee invoices"));
    CHECK_EQ(fix("chargebee is down", {"ChargeBee"}), std::string("ChargeBee is down"));
}

TEST(vocab_fixes_a_name_spelled_close) {
    CHECK_EQ(fix("call ramesh kumarr tomorrow", {"Ramesh Kumar"}), std::string("call Ramesh Kumar tomorrow"));
}

TEST(vocab_keeps_punctuation_around_the_span) {
    CHECK_EQ(fix("it was cement management.", {"incident management"}), std::string("it was incident management."));
    CHECK_EQ(fix("(charge bee), then", {"ChargeBee"}), std::string("(ChargeBee), then"));
}

TEST(vocab_fixes_telugu_spelling) {
    CHECK_EQ(fix("మేము హైదరబాద్ వెళ్ళాం", {"హైదరాబాద్"}), std::string("మేము హైదరాబాద్ వెళ్ళాం"));
}

TEST(vocab_leaves_short_words_unless_exact) {
    CHECK_EQ(fix("the cat sat", {"cart"}), std::string("the cat sat"));
    CHECK_EQ(fix("use aws now", {"AWS"}), std::string("use AWS now"));  // exact but for case
}

TEST(vocab_never_turns_a_common_word_into_another) {
    CHECK_EQ(fix("the meeting went well", {"Meetings"}), std::string("the meeting went well"));
    CHECK_EQ(fix("management said no", {"incident management"}), std::string("management said no"));
}

TEST(vocab_never_touches_medicines_or_doses) {
    // A saved medicine name is never written in: the medicine check must see what was said.
    CHECK_EQ(fix("take amlodepine 5 mg", {"Amlodipine"}, {"Amlodipine"}), std::string("take amlodepine 5 mg"));
    // Nor a word right before a dose, whatever the term.
    CHECK_EQ(fix("take telmisarten 40 mg daily", {"Telmisartan"}), std::string("take telmisarten 40 mg daily"));
}

TEST(vocab_no_terms_no_change) {
    CHECK_EQ(fix("nothing to do here", kNone), std::string("nothing to do here"));
    CHECK_EQ(fix("", {"ChargeBee"}), std::string(""));
}

TEST(vocab_already_right_is_unchanged) {
    CHECK_EQ(fix("incident management is ours", {"incident management"}), std::string("incident management is ours"));
}

// From the owner's notes (2026-11-20): the word before a term was eaten.
TEST(vocab_never_eats_the_word_before_a_term) {
    CHECK_EQ(fix("but perplexity said", {"Perplexity"}), std::string("but Perplexity said"));
    CHECK_EQ(fix("అది perplexity", {"Perplexity"}), std::string("అది Perplexity"));
}

