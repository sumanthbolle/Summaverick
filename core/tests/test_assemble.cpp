#include "check.h"
#include <set>
#include "sve_assemble.h"
#include "sve_store.h"
#include <cstdio>
#include <sqlite3.h>

using namespace sve;
using nlohmann::json;

static json sec(int idx, int first, int last, const char *title) {
    return {{"idx", idx}, {"first_piece", first}, {"last_piece", last}, {"start_sec", idx * 300}, {"end_sec", idx * 300 + 300},
            {"summary", {{"title", title}, {"points", json::array()}}}};
}

static size_t count_of(const std::string &s, const std::string &sub) {
    size_t n = 0;
    for (size_t p = s.find(sub); p != std::string::npos; p = s.find(sub, p + sub.size())) ++n;
    return n;
}

static const char *kPart1 =
    "## Topic\nThe Collector's role in the district\n\n"
    "## Key points\n- The Collector heads the district administration\n\n"
    "## Decisions and instructions\nNone\n\n"
    "## Action items\n- [ ] Read Article 243\n\n"
    "## Open questions\nNone";
static const char *kPart2 =
    "## Topic\nState Election Commission\n\n"
    "## Key points\n- Local body elections are run by the State Election Commission\n\n"
    "## Action items\n- [ ] No action items were stated\n\n"
    "## Dates and numbers mentioned\n- Article 243K";

TEST(assemble_notes_heads_each_part_and_merges_action_items) {
    json secs = json::array({sec(0, 0, 10, "Collector's role"), sec(1, 10, 20, "Elections")});
    const std::string out = assemble_notes(secs, json::array({kPart1, kPart2}));
    const size_t a = out.find("## Collector's role"), b = out.find("## Elections");
    CHECK(a != std::string::npos);
    CHECK(b != std::string::npos);
    CHECK(a < b);
    CHECK_EQ(count_of(out, "## Action items"), (size_t)1);
    const size_t ai = out.find("## Action items");
    CHECK(ai > b);  // the merged action items come last
    CHECK(out.find("Read Article 243", ai) != std::string::npos);
    CHECK(out.find("No action items") == std::string::npos);
    CHECK(out.find("Local body elections") > b);
    CHECK(out.find("Article 243K") > b);
    // A sub-heading that only says None is left out.
    CHECK(out.find("Open questions") == std::string::npos);
    CHECK(out.find("Decisions and instructions") == std::string::npos);
    std::fprintf(stderr, "---- assembled (2 sections) ----\n%s\n----\n", out.c_str());
}

TEST(assemble_notes_all_none_gives_None) {
    json secs = json::array({sec(0, 0, 10, "A"), sec(1, 10, 20, "B")});
    const std::string out = assemble_notes(
        secs, json::array({"## Key points\n- one\n\n## Action items\n- [ ] None stated",
                           "## Key points\n- two\n\n## Action items\nNo action items were stated."}));
    CHECK_EQ(count_of(out, "## Action items"), (size_t)1);
    const size_t ai = out.find("## Action items");
    CHECK(ai != std::string::npos && out.substr(ai) == "## Action items\nNone");
}

TEST(assemble_notes_skips_empty_part) {
    json secs = json::array({sec(0, 0, 10, "Collector's role"), sec(1, 10, 20, "Music")});
    const std::string out = assemble_notes(secs, json::array({kPart1, ""}));
    CHECK(out.find("## Collector's role") != std::string::npos);
    CHECK(out.find("Music") == std::string::npos);
    CHECK(out.find("Read Article 243") != std::string::npos);
    // A part whose every heading says None gets no heading either (no "None" heading for silence).
    const std::string out2 = assemble_notes(
        secs, json::array({kPart1, "## Topic\nNone\n\n## Key points\nNone\n\n## Action items\nNone"}));
    CHECK(out2.find("Music") == std::string::npos);
    CHECK(out2.find("Read Article 243") != std::string::npos);
    // Every part empty: nothing at all.
    CHECK_EQ(assemble_notes(secs, json::array({"", "  "})), std::string());
}

TEST(assemble_notes_dedupes_action_items) {
    json secs = json::array({sec(0, 0, 10, "A"), sec(1, 10, 20, "B")});
    const std::string out = assemble_notes(
        secs, json::array({"## Key points\n- one\n\n## Action items\n- [ ] Read Article 243 — students",
                           "## Key points\n- two\n\n## Action items\n- [ ] read article 243 — Students"}));
    CHECK_EQ(count_of(out, "rticle 243"), (size_t)1);
}

// Questions of part p: "Explain <four words of their own>?" (no word shared with another question but "explain").
static std::string qtext(int p, int i) {
    std::string q = "Explain";
    for (int j = 0; j < 4; ++j) q += std::string(" k") + (char)('a' + p) + (char)('a' + i) + "w" + (char)('a' + j);
    return q + "?";
}
TEST(assemble_notes_no_None_when_a_part_has_no_action_heading) {
    // Part 1 was cut at the cap before its Action items; part 2 says none: nobody knows part 1 had none.
    json secs = json::array({sec(0, 0, 10, "A"), sec(1, 10, 20, "B")});
    const std::string out = assemble_notes(
        secs, json::array({"## Key points\n- one", "## Key points\n- two\n\n## Action items\nNone"}));
    CHECK(out.find("None") == std::string::npos);
    CHECK(out.find("## Action items") == std::string::npos);
    // With a real task somewhere, only the real items are listed.
    const std::string out2 = assemble_notes(
        secs, json::array({"## Key points\n- one", "## Key points\n- two\n\n## Action items\n- [ ] Read Article 243"}));
    CHECK(out2.find("## Action items\n- [ ] Read Article 243") != std::string::npos);
    CHECK(out2.find("None") == std::string::npos);
    // An empty part (no speech) does not block "None": only non-empty parts are asked.
    json three = json::array({sec(0, 0, 10, "A"), sec(1, 10, 20, "B"), sec(2, 20, 30, "C")});
    const std::string out3 = assemble_notes(
        three, json::array({"## Key points\n- one\n\n## Action items\nNone", "", "## Key points\n- two\n\n## Action items\n- None stated"}));
    CHECK(out3.find("## Action items\nNone") != std::string::npos);
}

TEST(assemble_notes_keeps_topic_text_on_the_heading_line) {
    json secs = json::array({sec(0, 0, 10, "A")});
    const std::string out = assemble_notes(secs, json::array({"## Topic The Collector's role\n\n## Key points\n- one"}));
    CHECK(out.find("### Topic\nThe Collector's role") != std::string::npos);
    const std::string out2 = assemble_notes(secs, json::array({"## Topic: Elections\n## Key points\n- one"}));
    CHECK(out2.find("### Topic\nElections") != std::string::npos);
    // "## Topic" alone keeps the next line as its content.
    const std::string out3 = assemble_notes(secs, json::array({"## Topic\nElections\n## Key points\n- one"}));
    CHECK(out3.find("### Topic\nElections") != std::string::npos);
}

TEST(questions_labels_in_other_forms) {
    CHECK_EQ(count_questions("**Q1.** What is X?\n**A1.** X is a thing.\n\n**Q2. What is Y?**\n**A2.** Y."), 2);
    CHECK_EQ(count_questions("Q 1. What is X?\nA 1. X.\n\nQ 2: What is Y?\nA 2: Y."), 2);
    CHECK_EQ(count_questions("### Q1. What is X?\nA1. X."), 1);
    CHECK_EQ(count_questions("1. What is X?\nAnswer: X.\n\n2. What is Y?\nA. Y."), 2);
    // With Q labels, a numbered line inside an answer is part of the answer.
    CHECK_EQ(count_questions("Q1. Name two steps.\nA1. They are:\n1. first\n2. second"), 1);
    CHECK_EQ(count_questions("The recording has no lesson in it."), 0);
    const std::string out = assemble_questions(json::array({"**Q1.** What is X?\n**A1.** X is a thing."}));
    CHECK_EQ(out, std::string("Q1. What is X?\nA1. X is a thing."));
}

static std::string part_questions(int p, int n, const std::string &dup = "") {
    std::string s;
    for (int i = 1; i <= n; ++i) {
        const std::string q = (i == 3 && !dup.empty()) ? dup : qtext(p, i);
        s += "Q" + std::to_string(i) + ". " + q + "\nA" + std::to_string(i) + ". Answer " + std::to_string(p) + "." +
             std::to_string(i) + ".\n\n";
    }
    return s;
}

TEST(assemble_questions_dedupes_and_round_robins) {
    const std::string dup = "Who conducts the local body elections in a state?";
    json outs = json::array({part_questions(0, 6, dup), part_questions(1, 6), part_questions(2, 6, "Who conducts local body elections in the state?")});
    const std::string out = assemble_questions(outs);
    // 12 numbered questions, each with its answer.
    for (int i = 1; i <= 12; ++i) {
        CHECK(out.find("Q" + std::to_string(i) + ". ") != std::string::npos);
        CHECK(out.find("A" + std::to_string(i) + ". ") != std::string::npos);
    }
    CHECK(out.find("Q13.") == std::string::npos);
    CHECK_EQ(count_of(out, "local body elections"), (size_t)1);
    CHECK_EQ(count_of(out, "\nA"), (size_t)12);  // 12 answers, each on its own line
    for (int p = 0; p < 3; ++p) CHECK(out.find("Answer " + std::to_string(p) + ".") != std::string::npos);
    // Round robin: the first three are the first question of each part.
    CHECK(out.find("Q1. " + qtext(0, 1)) != std::string::npos);
    CHECK(out.find("Q2. " + qtext(1, 1)) != std::string::npos);
    CHECK(out.find("Q3. " + qtext(2, 1)) != std::string::npos);
    // Skipped empty parts; under the cap everything is kept.
    const std::string few = assemble_questions(json::array({"", part_questions(1, 2)}));
    CHECK(few.find("Q2. ") != std::string::npos && few.find("Q3.") == std::string::npos);
}

TEST(section_output_prompt_says_part_and_guide) {
    const std::string n = section_output_prompt(SectionOutputKind::Notes, "the words", "[0:00] Elections\n- point", 2, 3);
    CHECK(n.find("This is part 2 of 3 of the recording.") != std::string::npos);
    CHECK(n.find("[0:00] Elections") != std::string::npos);
    CHECK(n.find("the words") != std::string::npos);
    CHECK(n.size() > 8 && n.compare(n.size() - 8, 8, "## Topic") == 0);
    const std::string q = section_output_prompt(SectionOutputKind::Questions, "the words", "guide", 1, 1);
    CHECK(q.find("This is part 1 of 1 of the recording.") != std::string::npos);
    CHECK(q.find("Q1.") != std::string::npos);
    CHECK_EQ(section_output_max_out(SectionOutputKind::Notes), 440);
    CHECK_EQ(section_output_max_out(SectionOutputKind::Questions), 300);
}

// ---- store ----

static json state_row(const json &state, int idx, int kind) {
    for (const auto &r : state)
        if (r["idx"] == idx && r["kind"] == kind) return r;
    return nullptr;
}

TEST(section_outputs_done_needs_matching_range_and_lang) {
    Store st(":memory:");
    int64_t id = st.create("t", "te", nullptr, "x", 600);
    CHECK(st.set_sections(id, json::array({sec(0, 0, 10, "A")})));
    CHECK(st.bump_step_attempt(id, 0, 1, "te"));
    CHECK(st.set_section_output(id, 0, 1, "te", 0, 10, "## Key points\n- one"));
    json s = st.section_outputs_state(id, "te");
    CHECK_EQ(s.size(), (size_t)2);  // notes and questions of the one section
    CHECK(state_row(s, 0, 1)["done"] == true);
    CHECK(state_row(s, 0, 1)["attempts"] == 1);
    CHECK(state_row(s, 0, 2)["done"] == false);
    CHECK(state_row(s, 0, 2)["attempts"] == 0);
    CHECK(st.current_section_outputs(id, 1, "te") == json::array({"## Key points\n- one"}));
    CHECK(st.current_section_outputs(id, 2, "te").is_null());
    // The other language is not done.
    CHECK(state_row(st.section_outputs_state(id, "en"), 0, 1)["done"] == false);
    CHECK(st.current_section_outputs(id, 1, "en").is_null());
    // A write for a range that is not the current one is refused.
    CHECK(!st.set_section_output(id, 0, 2, "te", 0, 9, "Q1. x\nA1. y"));
    // Re-planned to [0,12): the old part is not done and its row is gone.
    CHECK(st.set_sections(id, json::array({sec(0, 0, 12, "A")})));
    CHECK(state_row(st.section_outputs_state(id, "te"), 0, 1)["done"] == false);
    CHECK(state_row(st.section_outputs_state(id, "te"), 0, 1)["attempts"] == 0);
    CHECK(st.section_outputs(id).empty());
    // Sections replaced with the same range keep their outputs (a live recording adds a section).
    CHECK(st.set_section_output(id, 0, 1, "te", 0, 12, "## Key points\n- one"));
    CHECK(st.set_sections(id, json::array({sec(0, 0, 12, "A"), sec(1, 12, 20, "B")})));
    CHECK(state_row(st.section_outputs_state(id, "te"), 0, 1)["done"] == true);
    CHECK(state_row(st.section_outputs_state(id, "te"), 1, 1)["done"] == false);
    CHECK(st.current_section_outputs(id, 1, "te").is_null());  // part 2 is missing
    // An empty part (no speech) counts as done.
    CHECK(st.set_section_output(id, 1, 1, "te", 12, 20, ""));
    CHECK(st.current_section_outputs(id, 1, "te") == json::array({"## Key points\n- one", ""}));
    // Clearing the transcript clears them.
    CHECK(st.clear_transcript(id));
    CHECK(st.section_outputs(id).empty());
    CHECK(st.step_attempts(id).empty());
}

TEST(section_output_rewritten_in_another_lang_overwrites) {
    Store st(":memory:");
    int64_t id = st.create("t", "te", nullptr, "x", 600);
    CHECK(st.set_sections(id, json::array({sec(0, 0, 10, "A")})));
    CHECK(st.set_section_output(id, 0, 1, "te", 0, 10, "telugu notes"));
    CHECK(st.set_section_output(id, 0, 1, "en", 0, 10, "english notes", true));
    json rows = st.section_outputs(id);
    CHECK_EQ(rows.size(), (size_t)1);
    CHECK(rows[0]["lang"] == "en" && rows[0]["text"] == "english notes" && rows[0]["cut"] == true);
    CHECK(state_row(st.section_outputs_state(id, "te"), 0, 1)["done"] == false);
    CHECK(state_row(st.section_outputs_state(id, "en"), 0, 1)["done"] == true);
    CHECK(st.current_section_outputs(id, 1, "en") == json::array({"english notes"}));
}

TEST(step_attempts_start_again_in_another_lang) {
    Store st(":memory:");
    int64_t id = st.create("t", "te", nullptr, "x", 600);
    CHECK(st.set_sections(id, json::array({sec(0, 0, 10, "A")})));
    CHECK(st.bump_step_attempt(id, 0, 1, "te"));
    CHECK(st.bump_step_attempt(id, 0, 1, "te"));
    CHECK(st.set_step_error(id, 0, 1, "empty reply"));
    CHECK(state_row(st.section_outputs_state(id, "te"), 0, 1)["attempts"] == 2);
    CHECK(state_row(st.section_outputs_state(id, "en"), 0, 1)["attempts"] == 0);  // te tries are not en tries
    CHECK(st.bump_step_attempt(id, 0, 1, "en"));
    json a = st.step_attempts(id);
    CHECK(a[0]["attempts"] == 1 && a[0]["lang"] == "en" && a[0]["last_error"].is_null());
}

TEST(section_outputs_columns_added_to_an_older_file) {
    // A file whose section_outputs / step_attempts were made before "cut" and "lang" existed gets them at open.
    const std::string path = "/tmp/sve_test_assemble_old.db";
    std::remove(path.c_str());
    std::remove((path + "-wal").c_str());
    std::remove((path + "-shm").c_str());
    {
        Store st(path);
        CHECK(st.ok());
    }
    {
        sqlite3 *db = nullptr;
        CHECK(sqlite3_open(path.c_str(), &db) == SQLITE_OK);
        CHECK(sqlite3_exec(db, "DROP TABLE section_outputs; DROP TABLE step_attempts; "
                               "CREATE TABLE section_outputs (note_id INTEGER NOT NULL REFERENCES notes(id) ON DELETE CASCADE, "
                               "idx INTEGER NOT NULL, kind INTEGER NOT NULL, lang TEXT NOT NULL, first_piece INTEGER NOT NULL, "
                               "last_piece INTEGER NOT NULL, text TEXT NOT NULL, created_at INTEGER NOT NULL, "
                               "PRIMARY KEY(note_id, idx, kind)); "
                               "CREATE TABLE step_attempts (note_id INTEGER NOT NULL REFERENCES notes(id) ON DELETE CASCADE, "
                               "idx INTEGER NOT NULL, kind INTEGER NOT NULL, attempts INTEGER NOT NULL DEFAULT 0, last_error TEXT, "
                               "PRIMARY KEY(note_id, idx, kind));",
                           nullptr, nullptr, nullptr) == SQLITE_OK);
        sqlite3_close(db);
    }
    Store st(path);
    CHECK(st.ok());
    int64_t id = st.create("t", "te", nullptr, "x", 600);
    CHECK(st.set_sections(id, json::array({sec(0, 0, 10, "A")})));
    CHECK(st.bump_step_attempt(id, 0, 1, "te"));
    CHECK(st.set_section_output(id, 0, 1, "te", 0, 10, "notes", true));
    CHECK(st.section_outputs(id)[0]["cut"] == true);
    std::remove(path.c_str());
    std::remove((path + "-wal").c_str());
    std::remove((path + "-shm").c_str());
}

TEST(step_attempts_count_and_error) {
    Store st(":memory:");
    int64_t id = st.create("t", "en", nullptr, "x", 600);
    CHECK(st.set_sections(id, json::array({sec(0, 0, 10, "A")})));
    CHECK(st.bump_step_attempt(id, 0, 2, "en"));
    CHECK(st.bump_step_attempt(id, 0, 2, "en"));
    CHECK(st.set_step_error(id, 0, 2, "no reply"));
    json a = st.step_attempts(id);
    CHECK_EQ(a.size(), (size_t)1);
    CHECK(a[0]["attempts"] == 2 && a[0]["last_error"] == "no reply");
    CHECK(st.undo_step_attempt(id, 0, 2));  // a cancel does not count
    CHECK(st.step_attempts(id)[0]["attempts"] == 1);
}

TEST(section_outputs_deleted_with_note) {
    Store st(":memory:");
    int64_t id = st.create("t", "en", nullptr, "x", 600);
    CHECK(st.set_sections(id, json::array({sec(0, 0, 10, "A")})));
    CHECK(st.bump_step_attempt(id, 0, 1, "en"));
    CHECK(st.set_section_output(id, 0, 1, "en", 0, 10, "notes"));
    CHECK(st.remove(id));
    CHECK(st.section_outputs(id).empty());
    CHECK(st.step_attempts(id).empty());
}

TEST(section_output_refused_for_deleted_note) {
    Store st(":memory:");
    int64_t id = st.create("t", "en", nullptr, "x", 600);
    CHECK(st.set_sections(id, json::array({sec(0, 0, 10, "A")})));
    CHECK(st.remove(id));
    CHECK(!st.set_section_output(id, 0, 1, "en", 0, 10, "notes"));
    CHECK(!st.bump_step_attempt(id, 0, 1, "en"));
    CHECK(st.section_outputs(id).empty());
    CHECK(st.step_attempts(id).empty());
    CHECK(st.section_outputs_state(id, "en").empty());
}

TEST(wipe_assistant_clears_section_outputs) {
    Store st(":memory:");
    int64_t id = st.create("t", "en", nullptr, "x", 600);
    CHECK(st.set_sections(id, json::array({sec(0, 0, 10, "A")})));
    CHECK(st.bump_step_attempt(id, 0, 1, "en"));
    CHECK(st.set_section_output(id, 0, 1, "en", 0, 10, "notes"));
    CHECK(st.wipe_assistant());
    CHECK(st.section_outputs(id).empty());
    CHECK(st.step_attempts(id).empty());
}
