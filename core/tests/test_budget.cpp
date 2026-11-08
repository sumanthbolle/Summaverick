#include "check.h"
#include "sve_budget.h"

using namespace sve;

// Values as sve_generate computed them before the helpers (n_ctx 8192: max_out 700, chunk_out 400).
TEST(notes_budget_matches_sve_generate_arithmetic) {
    CHECK_EQ(notes_max_out(8192), 700);
    CHECK_EQ(notes_max_out(2048), 512);
    CHECK_EQ(notes_max_out(512), 160);
    CHECK_EQ(notes_budget(8192, 300), 8192 - 700 - 250 - 300);
    CHECK_EQ(chunk_max_out(8192), 400);
    CHECK_EQ(chunk_max_out(400), 120);
    CHECK_EQ(chunk_budget(8192, 300), 8192 - 400 - 300 - 80 - 32);
    CHECK_EQ(chunk_budget(256, 300), 128);
}

TEST(fit_budget_is_the_smaller_budget_floored_at_128) {
    CHECK_EQ(fit_budget(8192, 300, notes_budget(8192, 300)), 6942);   // notes budget below chunk budget
    CHECK_EQ(fit_budget(8192, 300, 99999), chunk_budget(8192, 300));
    CHECK_EQ(fit_budget(8192, 300, 10), 128);
}

TEST(transcript_fits_up_to_the_budget_and_not_past_it) {
    const int b = fit_budget(8192, 300, notes_budget(8192, 300));
    CHECK(notes_material_fits(0, 8192, 300));
    CHECK(notes_material_fits(b, 8192, 300));
    CHECK(!notes_material_fits(b + 1, 8192, 300));
    // A larger system prompt (profile, vocabulary) leaves less room.
    CHECK(!notes_material_fits(b, 8192, 301));
    // Tiny context: the 128 floor still takes a short transcript.
    CHECK(notes_material_fits(128, 512, 400));
    CHECK(!notes_material_fits(129, 512, 400));
}
