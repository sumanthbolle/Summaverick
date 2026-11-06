#include "check.h"
#include "sve_medcheck.h"

using namespace sve;

static bool has(const MedCheck &c, const char *f) {
    for (auto &x : c.fields) if (x == f) return true;
    return false;
}

TEST(exact_match_ok) {
    MedFields f{"Amlodipine", "5 mg", "after dinner", "once a day", ""};
    MedCheck c = check_medicine(f, "Take Amlodipine 5 mg once a day after dinner");
    CHECK_EQ(c.status, std::string("ok"));
    CHECK(c.fields.empty());
}
TEST(invented_dose_is_check) {
    MedFields f{"Amlodipine", "10 mg", "after dinner", "once a day", ""};
    MedCheck c = check_medicine(f, "Take Amlodipine 5 mg once a day after dinner");
    CHECK_EQ(c.status, std::string("check"));
    CHECK_EQ(c.fields.size(), (size_t)1);
    CHECK(has(c, "dose"));
}
TEST(number_words_match) {
    MedFields f{"", "2 tablets", "", "", ""};
    CHECK_EQ(check_medicine(f, "two tablets twice a day").status, std::string("ok"));
}
TEST(telugu_number_words_match) {
    MedFields f{"", "2", "", "", ""};
    CHECK(!has(check_medicine(f, "రెండు మాత్రలు రాత్రి"), "dose"));
}
TEST(telugu_digits_match) {
    MedFields f{"", "500 mg", "", "", ""};
    CHECK(!has(check_medicine(f, "౫౦౦ mg"), "dose"));
}
TEST(name_in_other_script_is_check) {
    MedFields f{"Paracetamol", "500 mg", "", "", ""};
    MedCheck c = check_medicine(f, "పారాసిటమాల్ 500 mg");
    CHECK_EQ(c.status, std::string("check"));
    CHECK_EQ(c.fields.size(), (size_t)1);
    CHECK(has(c, "name"));
}
TEST(nothing_matches_is_drop) {
    MedFields f{"Metformin", "500 mg", "", "", ""};
    CHECK_EQ(check_medicine(f, "come back next week").status, std::string("drop"));
}
TEST(dose_not_substring_of_larger_number) {
    MedFields f{"", "5 mg", "", "", ""};
    MedCheck c = check_medicine(f, "Take 50 mg daily");
    CHECK_EQ(c.status, std::string("check"));
    CHECK_EQ(c.fields.size(), (size_t)1);
    CHECK(has(c, "dose"));
    MedFields g{"", "1", "", "", ""};
    CHECK_EQ(check_medicine(g, "take 10 tablets").status, std::string("check"));
}
TEST(glued_units_match) {
    MedFields f{"", "5 mg", "", "", ""};
    CHECK(!has(check_medicine(f, "Amlodipine 5mg"), "dose"));
    MedFields g{"", "5mg", "", "", ""};
    CHECK(!has(check_medicine(g, "Amlodipine 5 mg"), "dose"));
}
TEST(decimal_kept_and_not_confused) {
    MedFields f{"", "2.5 mg", "", "", ""};
    CHECK_EQ(check_medicine(f, "give 2.5 mg").status, std::string("ok"));
    CHECK_EQ(check_medicine(f, "give 25 mg").status, std::string("check"));
    CHECK_EQ(check_medicine(f, "give 2. 5 mg").status, std::string("check"));
}
TEST(whole_token_mapping_only) {
    MedFields f{"", "1", "", "", ""};
    CHECK(has(check_medicine(f, "ఒకసారి తీసుకోండి"), "dose"));
    CHECK(!has(check_medicine(f, "ఒక మాత్ర"), "dose"));
    CHECK_EQ(normalize_for_match("Someone ONE"), std::string("someone 1"));
}
TEST(name_long_ascii_tokens) {
    MedFields f{"Tab Amlodipine", "", "", "", ""};
    CHECK_EQ(check_medicine(f, "amlodipine 5 mg").status, std::string("ok"));
}
TEST(non_ascii_only_name_not_vacuous) {
    MedFields f{"పారాసిటమాల్", "", "", "", ""};
    CHECK_EQ(check_medicine(f, "come back").status, std::string("check"));
}
TEST(normalize_basic) {
    CHECK_EQ(normalize_for_match("  Take,  TWO\ttablets. "), std::string("take 2 tablets"));
    CHECK_EQ(normalize_for_match(""), std::string(""));
}
