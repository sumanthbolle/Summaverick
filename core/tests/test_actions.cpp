// The action-item workflow (2026-11-22): a finder proposes, a coverage check re-asks lines with a commitment and no
// action, an updater reads each action's later lines; the parent applies what they say. A stand-in model here.
#include <string>
#include <vector>
#include "check.h"
#include "sve_suggest.h"

using namespace sve;

namespace {
const json kMeeting = json::parse(R"([
  {"start_sec":0,"text":"Ravi: సరే అందరూ join అయ్యారా? Okay, let's start."},
  {"start_sec":24,"text":"Priya: Login page లో bug ఇంకా ఉంది. నేను Wednesday కల్లా ఆ login bug fix చేస్తాను."},
  {"start_sec":52,"text":"Ravi: Good. నేను Friday లోపు API document అందరికీ పంపిస్తాను."},
  {"start_sec":108,"text":"Kiran: Budget ఎవరు approve చేస్తారు? అది ఇంకా clear గా లేదు."},
  {"start_sec":186,"text":"Kiran will order the test phones by Monday."},
  {"start_sec":240,"text":"Priya: Sure. Coffee ఎవరైనా? సరే bye."}
])");
const json kProposal = json::parse(R"([
  {"start_sec":0,"text":"Sumanth: I'll send the proposal on Friday."},
  {"start_sec":12,"text":"Anil: The pricing in the proposal is still provisional."},
  {"start_sec":24,"text":"Sumanth: Good point. I'll wait for your pricing confirmation before I send it."}
])");

struct Fake {
    std::vector<std::string> phases, prompts;
    std::string check_reply = R"({"tasks":[]})", update_reply = R"({"status":"stands"})";
    ModelFn fn() {
        return [this](const std::string &phase, const std::string &, const std::string &prompt, int, std::string &out) {
            phases.push_back(phase);
            prompts.push_back(prompt);
            out = phase == "actions_check" ? check_reply : update_reply;
            return true;
        };
    }
};
}  // namespace

TEST(commitment_lines_are_found_in_english_and_telugu) {
    auto lines = commitment_lines(kMeeting);
    // Priya (చేస్తాను), Ravi (పంపిస్తాను), Kiran will order; not the question (చేస్తారు?) nor the small talk.
    CHECK_EQ((int)lines.size(), 3);
    CHECK_EQ(lines[0], (size_t)1);
    CHECK_EQ(lines[1], (size_t)2);
    CHECK_EQ(lines[2], (size_t)4);
}

TEST(coverage_check_asks_again_only_for_lines_without_an_action) {
    Extracted e = parse_extraction(R"({"tasks":[{"text":"API document అందరికీ పంపిస్తాను","who":"Ravi","due_text":"Friday లోపు"},
                                               {"text":"order the test phones","who":"Kiran","due_text":"by Monday"}]})");
    Fake f;
    f.check_reply = R"({"tasks":[{"text":"login bug fix చేస్తాను","who":"Priya","due_text":"Wednesday కల్లా"}]})";
    ActionStats st;
    Extracted out = refine_actions(e, kMeeting, f.fn(), "English", st);
    CHECK_EQ((int)out.tasks.size(), 3);
    CHECK_EQ(st.found, 2);
    CHECK_EQ(st.added, 1);
    int checks = 0;
    for (size_t i = 0; i < f.phases.size(); ++i)
        if (f.phases[i] == "actions_check") {
            checks++;
            CHECK(f.prompts[i].find("Priya") != std::string::npos);   // only Priya's line went back
        }
    CHECK_EQ(checks, 1);
}

TEST(updater_keeps_the_condition_and_drops_the_day_put_on_hold) {
    Extracted e = parse_extraction(R"({"tasks":[{"text":"Send the proposal","who":"Sumanth","due_text":"on Friday","due_date":"2026-11-27","quote":"I'll send the proposal on Friday."}]})");
    Fake f;
    f.update_reply = R"({"status":"changed","text":"Send the proposal after Anil confirms the pricing","due_text":""})";
    ActionStats st;
    Extracted out = refine_actions(e, kProposal, f.fn(), "English", st);
    CHECK_EQ((int)out.tasks.size(), 1);
    CHECK_EQ(out.tasks[0]["text"].get<std::string>(), std::string("Send the proposal after Anil confirms the pricing"));
    CHECK(out.tasks[0]["due_date"].is_null());
    CHECK_EQ(out.tasks[0]["due_text"].get<std::string>(), std::string(""));
    CHECK_EQ(st.updated, 1);
    bool saw_later = false;
    for (size_t i = 0; i < f.phases.size(); ++i)
        if (f.phases[i] == "actions_update") saw_later = f.prompts[i].find("pricing confirmation") != std::string::npos;
    CHECK(saw_later);   // the updater read the lines after the action
}

TEST(updater_drops_a_cancelled_action_and_skips_the_last_line) {
    const json p = json::parse(R"([
      {"start_sec":0,"text":"Ravi: I'll book the hall for Saturday."},
      {"start_sec":10,"text":"Priya: We don't need it any more, the event moved online."},
      {"start_sec":20,"text":"Ravi: Okay, I won't book it. I'll send the online link instead."}
    ])");
    Extracted e = parse_extraction(R"({"tasks":[{"text":"Book the hall","who":"Ravi","due_text":"for Saturday","quote":"I'll book the hall for Saturday."},
                                               {"text":"Send the online link","who":"Ravi","quote":"I'll send the online link instead."}]})");
    Fake f;
    f.update_reply = R"({"status":"cancelled"})";
    ActionStats st;
    Extracted out = refine_actions(e, p, f.fn(), "English", st);
    CHECK_EQ((int)out.tasks.size(), 1);   // the hall is cancelled; the link was said last, nothing after it
    CHECK_EQ(out.tasks[0]["text"].get<std::string>(), std::string("Send the online link"));
    CHECK_EQ(st.cancelled, 1);
    int updates = 0;
    for (const auto &ph : f.phases) updates += ph == "actions_update";
    CHECK_EQ(updates, 1);
}

TEST(workers_failing_leave_the_finder_s_actions) {
    Extracted e = parse_extraction(R"({"tasks":[{"text":"Send the proposal","who":"Sumanth","quote":"I'll send the proposal on Friday."}]})");
    ModelFn broken = [](const std::string &, const std::string &, const std::string &, int, std::string &out) { out = "not json"; return true; };
    ActionStats st;
    Extracted out = refine_actions(e, kProposal, broken, "English", st);
    CHECK_EQ((int)out.tasks.size(), 1);
    CHECK_EQ(out.tasks[0]["text"].get<std::string>(), std::string("Send the proposal"));
}

// From the Gemma run (2026-11-22): the coverage check re-asked the line the updater had used, and added the same
// action twice; the day stayed although it was put on hold.
TEST(lines_the_updater_read_are_not_asked_again) {
    Extracted e = parse_extraction(R"({"tasks":[{"text":"Send the proposal","who":"Sumanth","due_text":"on Friday","quote":"I'll send the proposal on Friday."}]})");
    Fake f;
    f.update_reply = R"({"status":"changed","text":"Send the proposal after Anil confirms the pricing","due_text":"Friday","day_stands":false})";
    f.check_reply = R"({"tasks":[{"text":"wait for pricing confirmation","who":"me"}]})";
    ActionStats st;
    Extracted out = refine_actions(e, kProposal, f.fn(), "English", st);
    CHECK_EQ((int)out.tasks.size(), 1);
    CHECK_EQ(st.lines_checked, 0);
    CHECK(out.tasks[0]["due_date"].is_null());
    CHECK_EQ(out.tasks[0]["due_text"].get<std::string>(), std::string(""));   // day_stands false wins over due_text
}

TEST(an_action_that_stands_takes_the_updater_s_wording) {
    Extracted e = parse_extraction(R"({"tasks":[{"text":"proposalను పంపడం","who":"Sumanth","due_text":"on Friday","quote":"I'll send the proposal on Friday."}]})");
    Fake f;
    f.update_reply = R"({"status":"stands","text":"Send the proposal on Friday","due_text":"on Friday","day_stands":true})";
    ActionStats st;
    Extracted out = refine_actions(e, kProposal, f.fn(), "English", st);
    CHECK_EQ(out.tasks[0]["text"].get<std::string>(), std::string("Send the proposal on Friday"));
    CHECK_EQ(out.tasks[0]["due_text"].get<std::string>(), std::string("on Friday"));
}

// From the second Gemma run: the held day stayed (day_stands ignored), one line gave two actions, and a reworded
// action lost its line.
TEST(parent_drops_a_day_the_condition_replaced) {
    Extracted e = parse_extraction(R"({"tasks":[{"text":"Send the proposal","who":"Sumanth","due_text":"on Friday","quote":"I'll send the proposal on Friday."}]})");
    Fake f;
    f.update_reply = R"({"status":"changed","text":"Send the proposal after receiving pricing confirmation from Anil.","due_text":"Friday","day_stands":true})";
    ActionStats st;
    Extracted out = refine_actions(e, kProposal, f.fn(), "English", st);
    CHECK_EQ(out.tasks[0]["due_text"].get<std::string>(), std::string(""));
    CHECK(out.tasks[0]["due_date"].is_null());
    // A condition that keeps the day ("on Friday after ...") keeps it.
    Fake g;
    g.update_reply = R"({"status":"changed","text":"Send the proposal on Friday after the pricing is confirmed","due_text":"on Friday"})";
    Extracted out2 = refine_actions(e, kProposal, g.fn(), "English", st);
    CHECK_EQ(out2.tasks[0]["due_text"].get<std::string>(), std::string("on Friday"));
}

TEST(parent_keeps_one_action_per_person_and_line_and_its_line) {
    Extracted e = parse_extraction(R"J({"tasks":[{"text":"Proposal పంపిస్తాను (after Anil confirms the pricing)","who":"Sumanth","quote":"I'll send the proposal on Friday."},
                                               {"text":"send Friday proposal","who":"Sumanth","due_text":"Friday","quote":"I'll send the proposal on Friday."}]})J");
    Fake f;
    f.update_reply = R"({"status":"stands"})";
    ActionStats st;
    Extracted out = refine_actions(e, kProposal, f.fn(), "English", st);
    CHECK_EQ((int)out.tasks.size(), 1);
    CHECK(out.tasks[0]["text"].get<std::string>().find("after Anil") != std::string::npos);  // the one with the condition
    Fake g;
    g.update_reply = R"({"status":"changed","text":"Sumanth will send the proposal after Anil confirms","due_text":""})";
    Extracted one = parse_extraction(R"({"tasks":[{"text":"send the proposal","who":"Sumanth"}]})");
    Extracted out2 = refine_actions(one, kProposal, g.fn(), "English", st);
    CHECK_EQ(out2.tasks[0]["start_sec"].get<int64_t>(), (int64_t)0);   // reworded, still at its line
    CHECK(out2.tasks[0]["quote"].get<std::string>().find("send the proposal") != std::string::npos);
}

// Third Gemma run: the cleared day came back from the quote, and a mixed-language action was not matched to its line.
TEST(a_day_the_workflow_cleared_stays_cleared) {
    auto v = build_suggestions(parse_extraction(R"({"tasks":[{"text":"Send the proposal after Anil confirms","who":"Sumanth","due_text":"","due_date":null,"quote":"I'll send the proposal on Friday.","start_sec":0}]})"),
                               kProposal, 7, json::object(), "2026-11-22");
    CHECK(v[0]["payload"]["due_date"].is_null());
}

TEST(a_mixed_language_action_finds_its_line_by_shared_words) {
    const json p = json::parse(R"([
      {"start_sec":0,"text":"Sumanth: నేను Friday proposal పంపిస్తాను."},
      {"start_sec":12,"text":"Anil: Proposal లో pricing ఇంకా final కాలేదు."},
      {"start_sec":24,"text":"Sumanth: నువ్వు pricing confirm చేసిన తర్వాతే నేను పంపిస్తాను."}
    ])");
    Extracted e = parse_extraction(R"J({"tasks":[{"text":"Proposal పంపిస్తాను (after Anil confirms the pricing)","who":"Sumanth"}]})J");
    Fake f;
    f.check_reply = R"({"tasks":[{"text":"send Friday proposal","who":"Sumanth","due_text":"Friday"}]})";
    ActionStats st;
    Extracted out = refine_actions(e, p, f.fn(), "English", st);
    CHECK_EQ((int)out.tasks.size(), 1);
    CHECK_EQ(st.lines_checked, 0);
}
