// Notes, summary and question prompts (sve_prompts.h; no model).
#include "check.h"
#include "sve_prompts.h"
#include "sve_sections.h"

using namespace sve;

static bool has(const std::string &s, const std::string &part) { return s.find(part) != std::string::npos; }

// N7 (owner audit of note 19): the notes step inverted statements, swapped words and numbers, turned a host's
// question into the guest's claim, and made an action item out of garbled speech.
static void check_faithful_rules(const std::string &p) {
    CHECK(has(p, "[unclear]"));                     // marked pieces (conf < 0.80) are not stated as fact
    CHECK(has(p, "do not state anything from it as fact"));
    CHECK(has(p, "garbled"));                       // skip instead of guessing
    CHECK(has(p, "do not guess"));
    CHECK(has(p, "Never expand an abbreviation the speaker did not expand"));  // "CAC (Civil Action Committee)"
    CHECK(has(p, "advertisements and sponsor"));
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

TEST(summary_and_chunk_prompts_know_unclear) {
    for (bool fb : {false, true}) CHECK(has(short_summary_prompt("TEXT", fb), "[unclear]"));
    CHECK(has(chunk_prompt("TEXT", 0, 2), "[unclear]"));
    CHECK(short_summary_prompt("TEXT", false).find("[unclear]") < short_summary_prompt("TEXT", false).find("TEXT"));
}

// N7 rev 1: an output cut at the token cap ends on its last whole sentence or bullet (note 20's notes stopped at
// "The speaker advises").
TEST(trim_cut_reply_ends_cleanly) {
    // A bullet cut mid-sentence is dropped; the whole bullets before it stay.
    CHECK_EQ(trim_cut_reply("## Topic\nExams\n\n## Key points\n- Prelims: 33% to pass\n- The speaker advises"),
             std::string("## Topic\nExams\n\n## Key points\n- Prelims: 33% to pass"));
    // A line with a finished sentence before the cut keeps that sentence.
    CHECK_EQ(trim_cut_reply("- Nine papers in mains. Each paper is worth 250 marks and the"),
             std::string("- Nine papers in mains."));
    // Ends cleanly already: unchanged (trailing space trimmed).
    CHECK_EQ(trim_cut_reply("- One.\n- Two.\n"), std::string("- One.\n- Two."));
    CHECK_EQ(trim_cut_reply("ఇది పూర్తి వాక్యం।"), std::string("ఇది పూర్తి వాక్యం।"));
    CHECK_EQ(trim_cut_reply("- ఒకటి।\n- రెండు కాని"), std::string("- ఒకటి।"));
    // A heading left with nothing under it goes too.
    CHECK_EQ(trim_cut_reply("## Key points\n- A done.\n\n## Decisions and instructions\n- Decided to"),
             std::string("## Key points\n- A done."));
    // A question cut before its answer goes with its half answer.
    CHECK_EQ(trim_cut_reply("Q1. What is CSAT?\nA1. A qualifying paper.\n\nQ2. Who was\n"),
             std::string("Q1. What is CSAT?\nA1. A qualifying paper."));
    CHECK_EQ(trim_cut_reply("Q1. What is CSAT?\nA1. A qualifying paper.\n\nQ2. Why is CSAT hard?\nA2. Because the"),
             std::string("Q1. What is CSAT?\nA1. A qualifying paper."));
    // A list number or "Q3." is not a finished sentence.
    CHECK_EQ(trim_cut_reply("- A.\n1. Second item is"), std::string("- A."));
    // Nothing whole to keep: the text as it is (better than nothing).
    CHECK_EQ(trim_cut_reply("The speaker advises"), std::string("The speaker advises"));
    CHECK_EQ(trim_cut_reply(""), std::string());
}

// Every call is the core plus one skill (2026-11-22); the one-word checkers and the transcriber take theirs alone.
TEST(skills_core_on_writing_skills_only) {
    using namespace sve::skills;
    for (Skill s : {Skill::notes, Skill::memories, Skill::insight, Skill::ask, Skill::compose, Skill::summary, Skill::general})
        CHECK(has(system(s), "Read the material to its end"));
    for (Skill s : {Skill::check_quote, Skill::same_person, Skill::classify, Skill::transcribe, Skill::todos})
        CHECK(!has(system(s), "Read the material to its end"));
    CHECK(has(system(Skill::todos), "\"We could\" is a suggestion"));
    CHECK(has(system(Skill::compose), "ready to send"));
    CHECK(has(sve::system_prompt("en"), "Skill: notes"));
    CHECK(has(sve::memory_system_prompt("te"), "Skill: things to remember"));
    CHECK(has(sve::memory_system_prompt("te"), "Write in Telugu"));
}
