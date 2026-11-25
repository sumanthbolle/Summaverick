#include "check.h"
#include "sve_onepass.h"

using namespace sve;

TEST(onepass_full) {
    auto r = split_all("### TITLE\n  Plan for May \n### SUMMARY\nThey met.\n\n### NOTES\n## Topic\nX\n- a\n### QUESTIONS\nQ1. a\nA1. b\n\n");
    CHECK(r.has_value());
    if (!r) return;
    CHECK_EQ(r->title, "Plan for May");
    CHECK_EQ(r->summary, "They met.");
    CHECK_EQ(r->notes, "## Topic\nX\n- a");
    CHECK_EQ(r->questions, "Q1. a\nA1. b");
}

TEST(onepass_preamble_ignored) {
    auto r = split_all("Sure, here it is:\n\n### TITLE\nT\n### SUMMARY\nS\n### NOTES\nN\n### QUESTIONS\nQ");
    CHECK(r.has_value());
    if (r) CHECK_EQ(r->title, "T");
}

TEST(onepass_missing_questions) {
    CHECK(!split_all("### TITLE\nT\n### SUMMARY\nS\n### NOTES\nN\n").has_value());
}

TEST(onepass_empty_notes) {
    CHECK(!split_all("### TITLE\nT\n### SUMMARY\nS\n### NOTES\n  \n### QUESTIONS\nQ").has_value());
}

TEST(onepass_out_of_order) {
    CHECK(!split_all("### TITLE\nT\n### NOTES\nN\n### SUMMARY\nS\n### QUESTIONS\nQ").has_value());
}

TEST(onepass_telugu_bytes) {
    const std::string te = "\xE0\xB0\xA8\xE0\xB0\xAE\xE0\xB0\xB8\xE0\xB1\x8D\xE0\xB0\x95\xE0\xB0\xBE\xE0\xB0\xB0\xE0\xB0\xAE\xE0\xB1\x8D (hello)";
    auto r = split_all("### TITLE\nT\n### SUMMARY\n" + te + "\n### NOTES\n- " + te + "\n### QUESTIONS\nQ1. " + te);
    CHECK(r.has_value());
    if (!r) return;
    CHECK_EQ(r->summary, te);
    CHECK_EQ(r->notes, "- " + te);
    CHECK_EQ(r->questions, "Q1. " + te);
}

#include "sve_budget.h"
#include "sve_prompts.h"

TEST(onepass_budget_tighter_than_one_output) {
    // The edge of the one-pass budget: material up to it fits, one token more does not.
    const int n_ctx = 8192, sys = 300;
    const int edge = sve::fit_budget(n_ctx, sys, n_ctx - sve::kAllMaxOut - sve::kAllScaffold - sys);
    CHECK(sve::all_material_fits(edge, n_ctx, sys));
    CHECK(!sve::all_material_fits(edge + 1, n_ctx, sys));
}

TEST(onepass_budget_reserves_2400_out) {
    // One pass writes four outputs, so its cap is above one stepped output's (notes_max_out, 1400 at 8192), and
    // the room is reserved against the same context: material fits with 2400 tokens reserved, not with one more.
    const int n_ctx = 8192, sys = 300;
    CHECK_EQ(sve::kAllMaxOut, 2400);
    CHECK(sve::kAllMaxOut > sve::notes_max_out(n_ctx));
    const int edge = n_ctx - 2400 - sve::kAllScaffold - sys;
    CHECK(edge <= sve::chunk_budget(n_ctx, sys));   // the one-pass room, not the chunk room, is the limit here
    CHECK(sve::all_material_fits(edge, n_ctx, sys));
    CHECK(!sve::all_material_fits(edge + 1, n_ctx, sys));
}

TEST(onepass_prompt_has_four_headers_in_order) {
    const std::string p = sve::all_outputs_prompt("hello");
    size_t a = p.find("### TITLE"), b = p.find("### SUMMARY"), c = p.find("### NOTES"), d = p.find("### QUESTIONS");
    CHECK(a != std::string::npos && a < b && b < c && c < d);
    CHECK(p.find("hello") != std::string::npos);
}

#include "sve_assemble.h"

TEST(onepass_questions_need_a_question) {
    // sve_generate_all returns NULL when count_questions is 0 (as the stepped path's check_part).
    CHECK_EQ(sve::count_questions("These are notes with no question in them."), 0);
    CHECK(sve::count_questions("Q1. What was decided?\nA1. To wait.") > 0);
}

TEST(onepass_questions_need_as_many_as_stepped) {
    // sve_generate_all returns NULL (the stepped path runs) when its questions part has fewer questions than the
    // stepped questions prompt asks for (kRevisionQuestions, the number in revision_questions_prompt).
    CHECK_EQ(sve::kRevisionQuestions, 6);
    const std::string q = sve::revision_questions_prompt("x", false);
    CHECK(q.find("Write 6 revision questions") != std::string::npos);
    CHECK(q.find("(continue to Q6)") != std::string::npos);
    CHECK(sve::all_outputs_prompt("x").find("### QUESTIONS\n6 revision questions") != std::string::npos);
    std::string five, six;
    for (int i = 1; i <= 6; i++) {
        const std::string one = "Q" + std::to_string(i) + ". What is point " + std::to_string(i) + "?\nA" +
                                std::to_string(i) + ". Point " + std::to_string(i) + ".\n\n";
        if (i <= 5) five += one;
        six += one;
    }
    CHECK_EQ(sve::count_questions(five), 5);
    CHECK(!sve::onepass_questions_enough(five));
    CHECK(sve::onepass_questions_enough(six));
    CHECK(!sve::onepass_questions_enough(""));
}
