#include "check.h"
#include "sve_medcheck.h"
#include "sve_textmatch.h"

using namespace sve;
using V = std::vector<std::string>;

TEST(nfc_clean_composes_telugu_ai) {
    CHECK_EQ(nfc_clean("\xE0\xB1\x86\xE0\xB1\x96"), std::string("\xE0\xB1\x88"));  // U+0C46 U+0C56 -> U+0C48
    CHECK_EQ(nfc_clean("\xE0\xB0\xB0\xE2\x80\x8D\xE0\xB0\xB5\xE0\xB0\xBF"), std::string("\xE0\xB0\xB0\xE0\xB0\xB5\xE0\xB0\xBF"));  // ZWJ
    CHECK_EQ(nfc_clean("\xE0\xB0\xB0\xE2\x80\x8C\xE0\xB0\xB5"), std::string("\xE0\xB0\xB0\xE0\xB0\xB5"));  // ZWNJ
    CHECK_EQ(nfc_clean("Dolo 650 caf\xC3\xA9"), std::string("Dolo 650 caf\xC3\xA9"));
}
TEST(nfc_clean_reorders_marks) {
    // virama (ccc 9) must come before U+0C56 (ccc 91), whatever the input order
    CHECK_EQ(nfc_clean("\xE0\xB0\x95\xE0\xB1\x96\xE0\xB1\x8D"), std::string("\xE0\xB0\x95\xE0\xB1\x8D\xE0\xB1\x96"));
}
TEST(match_tokens_numbers) {
    CHECK_EQ(canonical_facts("పది రూపాయలు"), V{"10"});
    CHECK_EQ(canonical_facts("రెండు వందల యాభై"), V{"250"});
    CHECK_EQ(canonical_facts("ఒక లక్ష"), V{"100000"});
    CHECK_EQ(canonical_facts("twenty five"), V{"25"});
    CHECK_EQ(canonical_facts("two thousand five hundred"), V{"2500"});
    CHECK_EQ(canonical_facts("one lakh twenty thousand"), V{"120000"});
    CHECK_EQ(canonical_facts("వంద"), V{"100"});
    CHECK_EQ(canonical_facts("ఐదు వేలు"), V{"5000"});
    CHECK_EQ(canonical_facts("ఇరవై ఐదు"), V{"25"});
    CHECK_EQ(canonical_facts("two hundred and fifty"), V{"250"});
    CHECK_EQ(canonical_facts("5 mg"), V{"5"});
    CHECK_EQ(canonical_facts("Dolo 650"), V{"650"});
    CHECK_EQ(canonical_facts("two three"), (V{"2", "3"}));
    CHECK_EQ(canonical_facts("ten five"), (V{"10", "5"}));
}
TEST(match_tokens_days) {
    CHECK_EQ(canonical_facts("శుక్రవారం"), V{"wd5"});
    CHECK_EQ(canonical_facts("Friday"), V{"wd5"});
    CHECK_EQ(canonical_facts("Sunday"), V{"wd7"});
    CHECK_EQ(canonical_facts("సోమవారం"), V{"wd1"});
    CHECK_EQ(canonical_facts("రేపు"), V{"rd1"});
    CHECK_EQ(canonical_facts("day after tomorrow"), V{"rd2"});
    CHECK_EQ(canonical_facts("ఈ రోజు"), V{"rd0"});
    CHECK_EQ(canonical_facts("నిన్న"), V{"rd-1"});
    CHECK_EQ(canonical_facts("వచ్చే వారం"), V{"rw1"});
    CHECK_EQ(canonical_facts("this week"), V{"rw0"});
    CHECK_EQ(canonical_facts("వచ్చే నెల"), V{"rm1"});
    CHECK_EQ(canonical_facts("15 January"), (V{"15", "mo1"}));
    CHECK_EQ(canonical_facts("డిసెంబర్"), V{"mo12"});
    CHECK_EQ(canonical_facts("call Ravi"), V{});
}
TEST(ambiguous_english_words_need_a_date_cue) {
    CHECK_EQ(canonical_facts("I may come"), V{});
    CHECK_EQ(canonical_facts("he sat down"), V{});
    CHECK_EQ(canonical_facts("the sun is out"), V{});
    CHECK_EQ(canonical_facts("march forward"), V{});
    CHECK_EQ(canonical_facts("may 5"), (V{"mo5", "5"}));
    CHECK_EQ(canonical_facts("5 may"), (V{"5", "mo5"}));
    CHECK_EQ(canonical_facts("on sat"), V{"wd6"});
    CHECK_EQ(canonical_facts("next sun"), V{"wd7"});
    CHECK_EQ(canonical_facts("monday"), V{"wd1"});
    CHECK_EQ(canonical_facts("september"), V{"mo9"});
    CHECK_EQ(canonical_facts("నూట యాభై"), V{"150"});
}
TEST(name_in_case_endings) {
    CHECK(name_in("రవి", "రవికి చెప్పాను"));
    CHECK(name_in("రావు", "రావు గారికి"));
    CHECK(name_in("Dr. Rao", "Dr. Rao said"));
    CHECK(name_in("డాక్టర్ రావు", "డాక్టర్ రావుగారికి"));
    CHECK(!name_in("రవి", "రవీంద్ర వచ్చాడు"));
    CHECK(!name_in("Ravi", "రవికి చెప్పాను"));
    CHECK(!name_in("రవి", "కిరణ్ వచ్చాడు"));
    CHECK(!name_in("డాక్టర్ రావు", "రావుకి చెప్పాను"));
}
TEST(resolve_spoken_date_uses_said_date) {
    CHECK_EQ(resolve_spoken_date("next Friday", "2026-11-06"), std::string("2026-11-13"));
    CHECK_EQ(resolve_spoken_date("శుక్రవారం", "2026-11-04"), std::string("2026-11-06"));
    CHECK_EQ(resolve_spoken_date("రేపు", "2026-11-30"), std::string("2026-12-01"));
    CHECK_EQ(resolve_spoken_date("15 January", "2026-11-08"), std::string("2027-01-15"));
    CHECK_EQ(resolve_spoken_date("soon", "2026-11-08"), std::string(""));
}
TEST(resolve_spoken_date_more) {
    CHECK_EQ(resolve_spoken_date("శుక్రవారం", "2026-11-06"), std::string("2026-11-13"));  // same day: next one
    CHECK_EQ(resolve_spoken_date("next Monday", "2026-11-04"), std::string("2026-11-09"));  // following Mon-Sun week
    CHECK_EQ(resolve_spoken_date("వచ్చే శుక్రవారం", "2026-11-04"), std::string("2026-11-13"));
    CHECK_EQ(resolve_spoken_date("yesterday", "2026-03-01"), std::string("2026-02-28"));
    CHECK_EQ(resolve_spoken_date("ఎల్లుండి", "2026-12-31"), std::string("2027-01-02"));
    CHECK_EQ(resolve_spoken_date("in 3 days", "2026-11-08"), std::string("2026-11-11"));
    CHECK_EQ(resolve_spoken_date("in two weeks", "2026-11-08"), std::string("2026-11-22"));
    CHECK_EQ(resolve_spoken_date("5 రోజుల్లో", "2026-11-08"), std::string("2026-11-13"));
    CHECK_EQ(resolve_spoken_date("రెండు వారాల్లో", "2026-11-08"), std::string("2026-11-22"));
    CHECK_EQ(resolve_spoken_date("January 15", "2026-01-15"), std::string("2026-01-15"));
    CHECK_EQ(resolve_spoken_date("15 జనవరి", "2026-11-08"), std::string("2027-01-15"));
    CHECK_EQ(resolve_spoken_date("31 February", "2026-11-08"), std::string(""));
    CHECK_EQ(resolve_spoken_date("tomorrow or Friday", "2026-11-08"), std::string(""));  // two dates: do not guess
    CHECK_EQ(resolve_spoken_date("15 January 3 pm", "2026-11-08"), std::string("2027-01-15"));
    CHECK_EQ(resolve_spoken_date("next week", "2026-11-08"), std::string(""));
    CHECK_EQ(resolve_spoken_date("tomorrow", "bad"), std::string(""));
}
TEST(medcheck_tokens_unchanged) {
    CHECK_EQ(normalize_for_match("Dolo 650, పది"), std::string("dolo 650 పది"));
    CHECK_EQ(med_tokens("Dolo650", false), V{"dolo650"});
    CHECK_EQ(med_tokens("Dolo650"), (V{"dolo", "650"}));
    CHECK(contains_run(V{"a", "b", "c"}, V{"b", "c"}));
    CHECK(name_ok("Dolo", "take dolo 650"));
}
