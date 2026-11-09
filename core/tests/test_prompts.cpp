// Notes, summary and question prompts (sve_prompts.h; no model).
#include "check.h"
#include "sve_prompts.h"

using namespace sve;

static bool has(const std::string &s, const std::string &part) { return s.find(part) != std::string::npos; }

// N7 (owner audit of note 19): the notes step inverted statements, swapped words and numbers, turned a host's
// question into the guest's claim, and made an action item out of garbled speech.
static void check_faithful_rules(const std::string &p) {
    CHECK(has(p, "unclear or garbled"));            // skip instead of guessing
    CHECK(has(p, "do not guess"));
    CHECK(has(p, "Keep who said what"));            // host versus guest
    CHECK(has(p, "host"));
    CHECK(has(p, "Never reverse a statement"));
    CHECK(has(p, "negation"));
    CHECK(has(p, "numbers, names and terms exactly as in the transcript"));
}

TEST(detailed_notes_prompt_has_faithful_rules) {
    for (bool fb : {false, true}) {
        const std::string p = detailed_notes_prompt("TEXT", fb);
        check_faithful_rules(p);
        CHECK(has(p, "only when a speaker asks for it or commits to it"));   // action items
        CHECK(has(p, "## Action items\n"));
        CHECK(p.size() > 8 && p.compare(p.size() - 8, 8, "## Topic") == 0);  // still ends on the first heading
        CHECK(has(p, "\"\"\"\nTEXT\n\"\"\""));
    }
}

TEST(revision_questions_prompt_has_faithful_rules) {
    for (bool fb : {false, true}) {
        const std::string p = revision_questions_prompt("TEXT", fb);
        check_faithful_rules(p);
        CHECK(has(p, "Q1. <question about the text>"));
        CHECK(has(p, "\"\"\"\nTEXT\n\"\"\""));
    }
}

TEST(faithful_rules_come_before_the_material) {
    const std::string p = detailed_notes_prompt("TEXT", false);
    CHECK(p.find("Never reverse") < p.find("TEXT"));
    const std::string q = revision_questions_prompt("TEXT", false);
    CHECK(q.find("Never reverse") < q.find("TEXT"));
}

TEST(output_language_rules_stay) {
    CHECK(has(system_prompt("te"), "Write in Telugu; keep important terms from other languages in brackets"));
    CHECK(has(system_prompt("en"), "Write in English;"));
    CHECK(has(system_prompt("en"), "/no_think"));
}
