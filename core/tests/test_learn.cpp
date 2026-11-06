#include "check.h"
#include "sve_learn.h"

using sve::Store;
using json = nlohmann::json;

static json edit_before() { return json::parse(R"({"text":"Walk","times":["20:00"],"when_text":"after dinner"})"); }
static json edit_after() { return json::parse(R"({"text":"Walk","times":["21:00"],"when_text":"after dinner"})"); }

TEST(second_identical_edit_suggests_preference) {
    Store s(":memory:");
    sve::record_edit(s, "reminder", edit_before(), edit_after());
    CHECK_EQ(s.suggestions("pending").size(), (size_t)0);
    CHECK_EQ(s.decisions("reminder", "times", 10).size(), (size_t)1);
    sve::record_edit(s, "reminder", edit_before(), edit_after());
    json p = s.suggestions("pending");
    CHECK_EQ(p.size(), (size_t)1);
    if (p.size() == 1) {
        CHECK_EQ(p[0]["kind"].get<std::string>(), std::string("fact"));
        json pl = p[0]["payload"].is_string() ? json::parse(p[0]["payload"].get<std::string>()) : p[0]["payload"];
        CHECK_EQ(pl["memory_kind"].get<std::string>(), std::string("preference"));
        CHECK_EQ(pl["text"].get<std::string>(), std::string("When I say \"after dinner\" I mean 21:00"));
        CHECK_EQ(p[0]["check_status"].get<std::string>(), std::string("ok"));
    }
}

TEST(different_edits_do_not_suggest) {
    Store s(":memory:");
    sve::record_edit(s, "reminder", edit_before(), edit_after());
    json other = edit_after(); other["times"] = json::array({"22:00"});
    sve::record_edit(s, "reminder", edit_before(), other);
    CHECK_EQ(s.suggestions("pending").size(), (size_t)0);
    // unchanged fields log nothing
    sve::record_edit(s, "reminder", edit_before(), edit_before());
    CHECK_EQ(s.decisions("", "", 50).size(), (size_t)2);
}

TEST(no_duplicate_preference_suggestion) {
    Store s(":memory:");
    for (int i = 0; i < 3; ++i) sve::record_edit(s, "reminder", edit_before(), edit_after());
    CHECK_EQ(s.suggestions("pending").size(), (size_t)1);
    // accepted still blocks
    s.set_suggestion_status(s.suggestions("pending")[0]["id"].get<int64_t>(), "accepted");
    sve::record_edit(s, "reminder", edit_before(), edit_after());
    CHECK_EQ(s.suggestions("").size(), (size_t)1);
}

TEST(existing_memory_blocks_suggestion) {
    Store s(":memory:");
    s.add_memory("preference", "When I say \"after dinner\" I mean 21:00", std::nullopt);
    sve::record_edit(s, "reminder", edit_before(), edit_after());
    sve::record_edit(s, "reminder", edit_before(), edit_after());
    CHECK_EQ(s.suggestions("").size(), (size_t)0);
}

TEST(missing_before_field_is_change_from_empty) {
    Store s(":memory:");
    sve::record_edit(s, "task", json::object(), json::parse(R"({"due_date":"2026-11-07"})"));
    json d = s.decisions("task", "due_date", 5);
    CHECK_EQ(d.size(), (size_t)1);
    CHECK_EQ(d[0]["from_value"].get<std::string>(), std::string(""));
    CHECK_EQ(d[0]["to_value"].get<std::string>(), std::string("2026-11-07"));
}
