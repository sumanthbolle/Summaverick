#include "check.h"
#include "sve_verify.h"

using namespace sve;

namespace {

json meeting() {
    return json::array({
        {{"start_sec", 0}, {"text", "Ravi: Okay, let's start. We talk about the release this week."}},
        {{"start_sec", 24}, {"text", "Priya: The login page bug is still there. I will fix the login bug by Wednesday."}},
        {{"start_sec", 52}, {"text", "Ravi: Good. I will send the API document to everyone before Friday."}},
        {{"start_sec", 80}, {"text", "Ravi: We decided the launch is not this month, it moves to the 15th of next month."}},
        {{"start_sec", 108}, {"text", "Kiran: Who approves the budget? That is not clear yet."}},
    });
}

const char *kNotes =
    "## Topic\nRelease planning\n\n## Key points\n- The login bug is still open\n\n"
    "## Decisions and instructions\n- Launch moves to the 15th of next month\n- The budget was approved by finance\n\n"
    "## Action items\n- [ ] Fix the login bug — Priya — Wednesday\n- [ ] Send the API document — Ravi — Friday\n\n"
    "## Open questions\n- Who approves the budget";

}  // namespace

TEST(verify_language_share) {
    CHECK(in_language("The launch moves to next month and the login bug is fixed.", "en"));
    CHECK(!in_language("లాంచ్ వచ్చే నెలకు మారింది, లాగిన్ బగ్ బుధవారం సరిచేస్తారు.", "en"));
    CHECK(in_language("లాంచ్ వచ్చే నెలకు మారింది (launch), లాగిన్ బగ్ బుధవారం సరిచేస్తారు.", "te"));
    CHECK(!in_language("The launch moves to next month and the login bug is fixed.", "te"));
    CHECK(in_language("ok", "te"));   // too short to measure
}

TEST(verify_decision_lines) {
    const auto l = decision_action_lines(kNotes);
    CHECK_EQ(l.size(), (size_t)4);
    if (l.size() == 4) {
        CHECK_EQ(l[0], "Launch moves to the 15th of next month");
        CHECK_EQ(l[2], "Fix the login bug Priya Wednesday");
    }
    CHECK(decision_action_lines("## Action items\n- None\n## Decisions\nNone.").empty());
    // demoted part headings of assembled notes
    CHECK_EQ(decision_action_lines("## Part 1\n### Decisions and instructions\n- Ship it\n### Key points\n- x").size(), (size_t)1);
}

TEST(verify_said_in) {
    const json p = meeting();
    CHECK(said_in("Launch moves to the 15th of next month", p));
    CHECK(said_in("Fix the login bug Priya Wednesday", p));
    CHECK(!said_in("The budget was approved by finance", p));
    CHECK(!said_in("Launch moves to the 20th of next month", p));   // a number never said
    CHECK(said_in("Ship", p));   // too few words to measure
}

TEST(verify_noise_lines) {
    const json p = json::array({
        {{"start_sec", 0}, {"text", "We will meet the vendor on Monday about the new contract terms."}},
        {{"start_sec", 30}, {"text", "ChargeBee Kubernetes"}},   // saved words written as speech on silence
    });
    const std::vector<std::string> vocab = {"ChargeBee", "Kubernetes"};
    auto n = noise_lines("- Meet the vendor on Monday\n- Meet the vendor on Monday\n- Move billing to ChargeBee", p, vocab);
    CHECK_EQ(n.size(), (size_t)2);
    CHECK(noise_lines("- Meet the vendor on Monday about the contract", p, vocab).empty());
    CHECK(said_word_count(p, vocab) >= 4);
    CHECK(said_word_count(json::array({{{"start_sec", 0}, {"text", "ChargeBee Kubernetes"}}}), vocab) < 4);
}

TEST(verify_parse_qa) {
    const auto q = parse_qa("Q1. When is the launch?\nA1. The 15th of next month.\n\nQ2. Who fixes the bug?\nA2. Priya.");
    CHECK_EQ(q.size(), (size_t)2);
    if (q.size() == 2) CHECK_EQ(q[1].a, "Priya.");
}

TEST(verify_retries_only_failing_part) {
    VerifyInput in;
    in.pieces = meeting();
    in.summary = "The team planned the release. The launch moves to the 15th of next month.";
    in.notes = kNotes;
    in.questions = "Q1. When is the launch?\nA1. The 15th of next month.";
    std::vector<std::string> rewritten;
    std::string feedback_seen;
    const RewriteFn rewrite = [&](const std::string &part, int, int, const std::string &fb, const std::vector<std::string> &failing,
                                  std::string &out) {
        rewritten.push_back(part);
        feedback_seen = fb;
        CHECK_EQ(failing.size(), (size_t)1);
        std::string n = kNotes;
        n.replace(n.find("- The budget was approved by finance\n"), 37, "");
        out = n;
        return true;
    };
    // The judge says no to every line it is asked about.
    int asked = 0;
    const ModelFn judge = [&](const std::string &, const std::string &, const std::string &, int, std::string &out) {
        ++asked;
        out = "no";
        return true;
    };
    std::vector<VerifyEvent> events;
    VerifyResult r = verify_outputs(in, judge, rewrite, [&](const VerifyEvent &e) { events.push_back(e); });
    CHECK_EQ(rewritten.size(), (size_t)1);
    if (!rewritten.empty()) CHECK_EQ(rewritten[0], "notes");
    CHECK(feedback_seen.find("The budget was approved by finance") != std::string::npos);
    CHECK(r.notes.find("budget was approved") == std::string::npos);
    CHECK_EQ(r.parts.size(), (size_t)3);
    for (const auto &v : r.parts) CHECK(!v.please_check);
    CHECK_EQ(asked, 1);   // only the one line code could not find
    bool logged_retry = false;
    for (const auto &e : events) logged_retry = logged_retry || (e.part == "notes" && e.action == "retry");
    CHECK(logged_retry);
}

TEST(verify_please_check_after_two_retries) {
    VerifyInput in;
    in.pieces = meeting();
    in.lang = "te";
    in.summary = "The team planned the release and the launch moves to the 15th of next month.";
    int calls = 0;
    const RewriteFn rewrite = [&](const std::string &, int, int, const std::string &fb, const std::vector<std::string> &, std::string &out) {
        ++calls;
        CHECK(fb.find("Telugu") != std::string::npos);
        out = "Still in English: the launch moves to the 15th of next month.";
        return true;
    };
    VerifyResult r = verify_outputs(in, nullptr, rewrite, nullptr);
    CHECK_EQ(calls, kMaxRetries);
    CHECK_EQ(r.parts.size(), (size_t)1);
    if (!r.parts.empty()) {
        CHECK(r.parts[0].please_check);
        CHECK_EQ(r.parts[0].chosen, 0);
    }
    CHECK_EQ(r.summary, in.summary);   // no better attempt: the first is kept
}

TEST(verify_noise_recording_no_retry) {
    VerifyInput in;
    in.pieces = json::array({{{"start_sec", 0}, {"text", "అది"}}});
    in.summary = "A meeting about the budget was held.";
    int calls = 0;
    const RewriteFn rewrite = [&](const std::string &, int, int, const std::string &, const std::vector<std::string> &, std::string &) {
        ++calls;
        return false;
    };
    VerifyResult r = verify_outputs(in, nullptr, rewrite, nullptr);
    CHECK_EQ(calls, 0);
    CHECK(!r.parts.empty() && r.parts[0].please_check);
}

TEST(verify_memory_quote_found_again) {
    VerifyInput in;
    in.pieces = meeting();
    in.memories = json::array({
        {{"id", 7}, {"text", "Priya fixes the login bug by Wednesday"}, {"quote", "I will fix the login bug by Wednesday"}, {"start_sec", 24}},
        {{"id", 8}, {"text", "Ravi sends the API document"}, {"quote", "Ravi mails the deck"}, {"start_sec", 52}},
        {{"id", 9}, {"text", "The office moves"}, {"quote", "we move offices"}, {"start_sec", 80}},
    });
    const ModelFn judge = [&](const std::string &, const std::string &, const std::string &prompt, int, std::string &out) {
        out = prompt.find("API document") != std::string::npos && prompt.find("Something taken") != std::string::npos &&
                      prompt.find("\"Ravi sends") != std::string::npos
                  ? "I will send the API document to everyone before Friday."
                  : "NONE";
        return true;
    };
    VerifyResult r = verify_outputs(in, judge, nullptr, nullptr);
    CHECK_EQ(r.memory_fixes.size(), (size_t)1);
    if (!r.memory_fixes.empty()) {
        CHECK_EQ(jint(r.memory_fixes[0], "id", 0), 8);
        CHECK_EQ(jint(r.memory_fixes[0], "start_sec", -1), 52);
    }
    CHECK_EQ(r.memories_flagged.size(), (size_t)1);
    if (!r.memories_flagged.empty()) CHECK_EQ(r.memories_flagged[0].get<int64_t>(), 9);
}

TEST(verify_judge_budget_never_fails) {
    VerifyInput in;
    in.pieces = meeting();
    std::string notes = "## Decisions\n";
    for (int i = 0; i < kMaxJudgeCalls + 4; ++i) notes += "- Unrelated decision number " + std::to_string(100 + i) + " about vendors\n";
    in.notes = notes;
    const ModelFn judge = [&](const std::string &, const std::string &, const std::string &, int, std::string &out) {
        out = "yes";
        return true;
    };
    VerifyResult r = verify_outputs(in, judge, nullptr, nullptr);
    // numbers never said fail in code without the judge
    CHECK(!r.parts.empty() && r.parts[0].please_check);
    CHECK_EQ(r.judge_calls, 0);
}

TEST(verify_wrong_owner) {
    const json p = json::array({
        {{"start_sec", 0}, {"text", "Ravi: I will send the API document before Friday."}},
        {{"start_sec", 20}, {"text", "Me: I will call the client and confirm the demo date tomorrow."}},
        {{"start_sec", 40}, {"text", "Ravi: Kiran, please order the test phones by Monday."}},
        {{"start_sec", 60}, {"text", "Kiran: Okay."}},
    });
    CHECK_EQ(wrong_owner("Ravi will call the client to confirm the demo date tomorrow", p), "Me");
    CHECK_EQ(wrong_owner("Ravi will send the API document before Friday", p), "");
    CHECK_EQ(wrong_owner("Kiran will order the test phones by Monday", p), "");   // given to him
    CHECK_EQ(wrong_owner("Call the client to confirm the demo date", p), "");       // names nobody
}

TEST(verify_language_ignores_headings) {
    // Telugu notes under the English template headings are Telugu.
    CHECK(in_language("## Topic\nProposal పంపడం మరియు Pricing\n\n## Key points\n- Proposal లోని pricing provisional మాత్రమే.\n\n"
                      "## Decisions and instructions\n- Sumanth, Anil pricing ను confirm చేసిన తర్వాత proposal ని పంపుతాడు.\n\n"
                      "## Action items\n- None\n\n## Open questions\n- None", "te"));
}

TEST(verify_telugu_filler_words) {
    const json p = json::array({{{"start_sec", 0}, {"text", "Sumanth: నేను Friday proposal పంపిస్తాను."}},
                                {{"start_sec", 12}, {"text", "Anil: Proposal లో pricing ఇంకా final కాలేదు."}}});
    CHECK(said_in("Sumanth మరియు Anil మధ్య జరిగింది.", p));
}

TEST(verify_judge_failure_is_unmeasured) {
    VerifyInput in;
    in.pieces = meeting();
    in.notes = kNotes;   // one line code cannot find
    in.memories = json::array({{{"id", 9}, {"text", "The office moves"}, {"quote", "we move offices"}, {"start_sec", 80}}});
    const ModelFn judge = [&](const std::string &, const std::string &, const std::string &, int, std::string &) { return false; };
    int rewrites = 0;
    const RewriteFn rewrite = [&](const std::string &, int, int, const std::string &, const std::vector<std::string> &, std::string &) {
        ++rewrites;
        return false;
    };
    VerifyResult r = verify_outputs(in, judge, rewrite, nullptr);
    CHECK_EQ(rewrites, 0);
    CHECK(!r.parts.empty() && !r.parts[0].please_check);
    CHECK(r.memories_flagged.empty());
    CHECK(r.unjudged >= 2);
    CHECK(!said_in("Release the model next month", in.pieces));   // "model" and "month" are words, not canonical forms
}

TEST(verify_worker_material_and_feedback) {
    const json p = json::array({
        {{"start_sec", 0}, {"text", "We will meet the vendor on Monday about the new contract terms."}},
        {{"start_sec", 30}, {"text", "ChargeBee Kubernetes"}},
    });
    const std::vector<std::string> vocab = {"ChargeBee", "Kubernetes"};
    CHECK_EQ(clean_pieces(p, vocab).size(), (size_t)1);
    CHECK_EQ(noise_terms(p, vocab).size(), (size_t)2);
    const std::string fb = feedback_for("summary", {{"noise", "Meet the vendor about ChargeBee"}}, "English", noise_terms(p, vocab));
    CHECK(fb.find("never said") != std::string::npos && fb.find("ChargeBee") != std::string::npos);
}

// Review 2026-11-25: a decision that reverses the recording passed on word overlap, and a bare number
// passed as an answer because short answers skipped the number check.
TEST(verify_reversed_decision_is_caught) {
    const json p = json::array({
        {{"start_sec", 0}, {"text", "Meera: The committee did not approve the budget increase."}},
        {{"start_sec", 20}, {"text", "Meera: We will ask again in March."}},
    });
    CHECK_EQ(said_check("The committee did approve the budget increase", p), -2);
    CHECK_EQ(said_check("The committee did not approve the budget increase", p), 1);
    CHECK_EQ(said_check("The committee didn't approve the budget increase", p), 1);
    // No judge: the reversed decision is flagged, not passed.
    VerifyInput in;
    in.lang = "en";
    in.pieces = p;
    int calls = 0, unjudged = 0;
    const std::string notes = "## Decisions\n- The committee did approve the budget increase\n";
    auto f = check_output("notes", notes, in, nullptr, calls, unjudged);
    CHECK_EQ(f.size(), (size_t)1);
    if (!f.empty()) CHECK_EQ(f[0].check, "meaning");
    // A judge that says no keeps it flagged; one that says yes clears it.
    const ModelFn no = [](const std::string &, const std::string &, const std::string &, int, std::string &o) { o = "no"; return true; };
    const ModelFn yes = [](const std::string &, const std::string &, const std::string &, int, std::string &o) { o = "yes"; return true; };
    calls = 0;
    CHECK_EQ(check_output("notes", notes, in, no, calls, unjudged).size(), (size_t)1);
    calls = 0;
    CHECK(check_output("notes", notes, in, yes, calls, unjudged).empty());
}

TEST(verify_negation_scope_is_the_clause) {
    // "not this month, it moves to the 15th": the decision matches the second clause, which is not negated.
    const json p = meeting();
    CHECK_EQ(said_check("Launch moves to the 15th of next month", p), 1);
    CHECK(!negated("The launch moves to the 15th"));
    CHECK(negated("We did not approve it"));
    CHECK(negated("ఆమోదం ఇంకా రాలేదు"));
    CHECK(!negated("Nothingness is a word"));
}

TEST(verify_bare_number_answer_needs_the_number) {
    const json p = json::array({
        {{"start_sec", 0}, {"text", "Asha: The budget increase was discussed but no figure was agreed."}},
    });
    CHECK_EQ(said_check("9000", p), -1);
    VerifyInput in;
    in.lang = "en";
    in.pieces = p;
    int calls = 0, unjudged = 0;
    const auto f = check_output("questions", "Q1. How much is the increase?\nA1. 9000", in, nullptr, calls, unjudged);
    CHECK_EQ(f.size(), (size_t)1);
    if (!f.empty()) CHECK_EQ(f[0].check, "answerable");
}

// Orchestrator review 2026-11-25: summary claims, relationships, agreement across outputs, evidence.
namespace {
json priya() {
    return json::array({
        {{"start_sec", 0}, {"text", "Ravi: Priya will review the budget on Friday. Approval is still pending."}},
        {{"start_sec", 30}, {"text", "Kiran: I will book the hall for the launch."}},
    });
}
const ModelFn kNo = [](const std::string &, const std::string &, const std::string &, int, std::string &o) { o = "no"; return true; };
}  // namespace

TEST(verify_undecided_conflict) {
    const json p = priya();
    CHECK(undecided("Approval is still pending"));
    CHECK(undecided("Should we move the launch?"));
    CHECK(!undecided("Priya will review the budget on Friday"));
    CHECK(undecided_conflict("Priya approved the budget", p));
    CHECK(!undecided_conflict("Budget approval is pending", p));     // says it is open: no conflict
    CHECK(!undecided_conflict("Priya will review the budget on Friday", p) || said_check("Priya will review the budget on Friday", p) == 1);
    CHECK_EQ(said_check("Priya approved the budget", p), -2);
}

TEST(verify_summary_claims_checked) {
    const auto c = summary_claims("## Summary\nBudget approval is pending. Priya will review it on Friday!\n- Ok.");
    CHECK_EQ(c.size(), (size_t)2);
    VerifyInput in;
    in.pieces = priya();
    int calls = 0, unjudged = 0;
    auto f = check_output("summary", "The team met. Priya approved the budget.", in, kNo, calls, unjudged);
    CHECK_EQ(f.size(), (size_t)1);
    if (!f.empty()) {
        CHECK_EQ(f[0].check, "meaning");
        CHECK_EQ(f[0].start_sec, (int64_t)0);   // the evidence: where the recording says it
    }
    calls = 0;
    CHECK(check_output("summary", "Budget approval is pending. Priya will review it on Friday.", in, kNo, calls, unjudged).empty());
}

TEST(verify_conflicting_claims_across_outputs) {
    std::vector<Claim> cl = {{"summary", "Budget approval is pending", 0},
                             {"memory", "Priya approved the budget approval", 7},
                             {"notes", "Book the hall for the launch Kiran", 0}};
    auto pairs = conflicting_claims(cl);
    CHECK_EQ(pairs.size(), (size_t)1);
    if (!pairs.empty()) CHECK((pairs[0].first.id == 7) != (pairs[0].second.id == 7));
    // One output's own lines never conflict with each other here.
    CHECK(conflicting_claims({{"notes", "The budget is approved", 0}, {"notes", "The budget is not approved", 0}}).empty());
}

TEST(verify_relationships_meaning_who_consistency) {
    VerifyInput in;
    in.pieces = priya();
    in.summary = "Budget approval is pending. Priya will review it on Friday.";
    in.memories = json::array({
        {{"id", 1}, {"text", "Priya approved the budget"}, {"quote", "Approval is still pending."}, {"start_sec", 0}, {"type", "decision"}, {"who", "Priya"}},
        {{"id", 2}, {"text", "Priya will review the budget on Friday"}, {"quote", "Priya will review the budget on Friday."}, {"start_sec", 0}, {"type", "promise"}, {"who", "Priya"}},
        {{"id", 3}, {"text", "Ravi will book the hall for the launch"}, {"quote", "I will book the hall for the launch."}, {"start_sec", 30}, {"type", "promise"}, {"who", "Ravi"}},
        {{"id", 4}, {"text", "The hall costs a lot"}, {"quote", "I will book the hall for the launch."}, {"start_sec", 30}, {"type", "fact"}},
    });
    VerifyResult r = verify_outputs(in, kNo, nullptr, nullptr);
    std::map<int64_t, std::string> got;
    for (const auto &f : r.relation_flags) got[jint(f, "memory_id", 0)] = jstr(f, "check");
    CHECK_EQ(got[1], "meaning");      // settled where the recording leaves it open
    CHECK(!got.count(2));             // reviewing is what was said
    CHECK_EQ(got[3], "who");          // Kiran said he would book the hall, not Ravi
    CHECK(!got.count(4));             // a fact is not a relationship
    CHECK(std::find(r.memories_flagged.begin(), r.memories_flagged.end(), json(1)) != r.memories_flagged.end());
    bool ev = false;
    for (const auto &e : r.evidence) ev = ev || (jstr(e, "part") == "relation" && jint(e, "id", 0) == 1 && !e.value("ok", true));
    CHECK(ev);
}

TEST(verify_output_disagreeing_with_another_is_retried) {
    // The summary says it is approved; the notes say it is pending. The summary disagrees with the transcript too:
    // it is the one written again, with the disagreement as feedback.
    VerifyInput in;
    in.pieces = priya();
    in.summary = "The budget approval was given.";
    in.notes = "## Decisions\n- Budget approval is still pending\n";
    std::vector<std::string> parts;
    std::string fb;
    const RewriteFn rewrite = [&](const std::string &part, int, int, const std::string &f, const std::vector<std::string> &, std::string &out) {
        parts.push_back(part);
        fb = f;
        out = "Budget approval is still pending.";
        return true;
    };
    VerifyResult r = verify_outputs(in, kNo, rewrite, nullptr);
    CHECK(!parts.empty() && parts[0] == "summary");
    CHECK(r.summary.find("pending") != std::string::npos);
    for (const auto &v : r.parts) CHECK(!v.please_check);
}

TEST(verify_provisional_is_open_not_settled) {
    // Measured 2026-11-26 (proposal_telugu): "the pricing is only provisional" says the matter is open, like the
    // transcript's "not final yet"; it is no claim of a settled decision.
    const json p = json::array({
        {{"start_sec", 12}, {"text", "Anil: Proposal లో pricing ఇంకా final కాలేదు, provisional మాత్రమే."}},
    });
    CHECK(undecided("Pricing provisional మాత్రమే అని Anil తెలియజేశాడు."));
    CHECK(!undecided_conflict("Pricing provisional మాత్రమే అని Anil తెలియజేశాడు.", p));
    CHECK(undecided_conflict("The pricing is final", p));
}

TEST(verify_side_clause_negation_is_not_the_claim) {
    // Measured 2026-11-26 (team_meeting): "decided for next month on the 15th, not this month" agrees with the
    // recording; its short side clause "not this month" must not make it read as the opposite.
    const json p = json::array({
        {{"start_sec", 80}, {"text", "Ravi: Launch గురించి మనం decide చేశాం: launch ఈ నెల కాదు, next month 15th కి."}},
    });
    CHECK(!polarity_conflict("The launch has been decided for next month on the 15th, not this month.", p));
    CHECK(polarity_conflict("The launch is not next month on the 15th", p));
}

// Review of 9b6c400 (2026-11-26): four reproductions that were accepted, and the unchecked state.
namespace {
json planning() {
    return json::array({
        {{"start_sec", 0}, {"text", "Ravi: We could approve the budget increase next quarter."}},
        {{"start_sec", 30}, {"text", "Priya: I will fix the login bug by Friday."}},
        {{"start_sec", 60}, {"text", "Ravi: Good. Let me check the calendar for the review."}},
        {{"start_sec", 90}, {"text", "Kiran: Testing needs another device."}},
        {{"start_sec", 120}, {"text", "Ravi: I will order the test phones by Monday."}},
    });
}
const ModelFn kFailing = [](const std::string &, const std::string &, const std::string &, int, std::string &) { return false; };
const ModelFn kYes = [](const std::string &, const std::string &, const std::string &, int, std::string &o) { o = "yes"; return true; };
std::string status_of(const VerifyResult &r, const std::string &part, const std::string &prefix) {
    for (const auto &e : r.evidence)
        if (jstr(e, "part") == part && jstr(e, "item").rfind(prefix, 0) == 0) return jstr(e, "status");
    return "";
}
}  // namespace

TEST(verify_could_is_not_decided) {
    const json p = planning();
    CHECK(undecided("We could approve the budget increase next quarter"));
    CHECK_EQ(said_check("We decided to approve the budget increase", p), -2);
    VerifyInput in;
    in.pieces = p;
    int calls = 0, unjudged = 0;
    auto f = check_output("notes", "## Decisions\n- We decided to approve the budget increase\n", in, kNo, calls, unjudged);
    CHECK_EQ(f.size(), (size_t)1);
    if (!f.empty()) CHECK_EQ(f[0].check, "meaning");
    CHECK(calls >= 1);   // the judge was asked
}

TEST(verify_date_moved_to_another_task_is_caught) {
    const json p = planning();
    CHECK_EQ(binding_conflict("Fix the login bug Priya Monday", p), "wd1");
    CHECK_EQ(binding_conflict("Fix the login bug Priya Friday", p), "");
    CHECK_EQ(binding_conflict("Order the test phones Ravi Monday", p), "");
    CHECK_EQ(binding_conflict("Order the test phones Kiran Monday", p), "");   // named in the window (Kiran asked)
    VerifyInput in;
    in.pieces = p;
    int calls = 0, unjudged = 0;
    auto f = check_output("notes", "## Action items\n- [ ] Fix the login bug — Priya — Monday\n", in, kNo, calls, unjudged);
    CHECK_EQ(f.size(), (size_t)1);
    if (!f.empty()) CHECK_EQ(f[0].check, "binding");
    calls = 0;
    CHECK(check_output("notes", "## Action items\n- [ ] Fix the login bug — Priya — Friday\n", in, kNo, calls, unjudged).empty());
}

TEST(verify_invented_relationship_with_real_quote_is_not_passed) {
    VerifyInput in;
    in.pieces = planning();
    in.memories = json::array({{{"id", 9}, {"text", "The team decided to buy an office in London"}, {"type", "decision"},
                                {"who", "Ravi"}, {"quote", "I will order the test phones by Monday."}, {"start_sec", 120}}});
    CHECK(!quote_supports("The team decided to buy an office in London", "I will order the test phones by Monday."));
    // A judge that says no: flagged.
    VerifyResult r = verify_outputs(in, kNo, nullptr, nullptr);
    CHECK_EQ(r.relation_flags.size(), (size_t)1);
    if (!r.relation_flags.empty()) CHECK_EQ(jstr(r.relation_flags[0], "check"), "grounded");
    CHECK_EQ(status_of(r, "relation", "The team decided"), "failed");
    // A judge that fails: unchecked, never passed.
    VerifyResult u = verify_outputs(in, kFailing, nullptr, nullptr);
    CHECK(u.relation_flags.empty());
    CHECK_EQ(status_of(u, "relation", "The team decided"), "unchecked");
    CHECK_EQ(u.unchecked.size(), (size_t)1);
    // A judge that says yes: passed.
    VerifyResult y = verify_outputs(in, kYes, nullptr, nullptr);
    CHECK_EQ(status_of(y, "relation", "The team decided"), "passed");
}

TEST(verify_judge_failure_leaves_claims_unchecked) {
    VerifyInput in;
    in.pieces = planning();
    in.summary = "The quarterly offsite moves to Goa.";   // nothing of it said: only the judge can decide
    VerifyResult r = verify_outputs(in, kFailing, nullptr, nullptr);
    CHECK(!r.parts.empty() && !r.parts[0].please_check);   // no "Please check" on a guess
    CHECK_EQ(r.parts[0].unchecked.size(), (size_t)1);
    CHECK_EQ(status_of(r, "summary", "The quarterly offsite"), "unchecked");
    CHECK(r.unjudged >= 1);
    // No judge at all is the same: unchecked, not passed.
    VerifyResult n = verify_outputs(in, nullptr, nullptr, nullptr);
    CHECK_EQ(status_of(n, "summary", "The quarterly offsite"), "unchecked");
}

TEST(verify_log_counts_each_summary_claim) {
    VerifyInput in;
    in.pieces = planning();
    in.summary = "The quarterly offsite moves to Goa. The office in London was sold.";
    std::vector<VerifyEvent> events;
    verify_outputs(in, kNo, nullptr, [&](const VerifyEvent &e) { events.push_back(e); });
    bool seen = false;
    for (const auto &e : events)
        if (e.part == "summary" && e.action == "check" && e.attempt == 0) {
            seen = true;
            CHECK_EQ(e.total, 2);             // two claims checked, not "the summary"
            CHECK(e.failed <= e.total);       // never "2 failed of 1"
        }
    CHECK(seen);
}
