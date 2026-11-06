#include "check.h"
#include "sve_context.h"

using sve::Store;
using json = nlohmann::json;

static bool valid_utf8_end(const std::string &s) {
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = s[i];
        size_t n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        if (!n || i + n > s.size()) return false;
        for (size_t k = 1; k < n; k++) if (((unsigned char)s[i + k] & 0xC0) != 0x80) return false;
        i += n;
    }
    return true;
}

TEST(always_block_includes_profile_and_meds) {
    Store s(":memory:");
    s.setting_set("profile", R"({"name":"Ravi","lang":"te","style":"short","routine":{"wake":"06:30","dinner":"20:00"}})");
    s.add_medicine(json::parse(R"({"name":"Amlodipine","dose":"5 mg","when_text":"after dinner","times":["20:00"],"repeat":"daily"})"));
    std::string b = sve::always_block(s);
    CHECK(b.find("Ravi") != std::string::npos);
    CHECK(b.find("Telugu") != std::string::npos);
    CHECK(b.find("Amlodipine 5 mg") != std::string::npos);
    CHECK(b.find("- Amlodipine 5 mg \xE2\x80\x94 after dinner (20:00) daily") != std::string::npos);
    CHECK(b.find("20:00") != std::string::npos);
}

TEST(always_block_respects_budget_utf8) {
    Store s(":memory:");
    s.setting_set("profile", R"({"name":"Ravi","lang":"te"})");
    for (int i = 0; i < 500; i++) s.add_memory("vocab", "\xE0\xB0\xA4\xE0\xB1\x86\xE0\xB0\xB2\xE0\xB1\x81\xE0\xB0\x97\xE0\xB1\x81" + std::to_string(i), std::nullopt);
    std::string b = sve::always_block(s, 2000);
    CHECK(b.size() <= 2000);
    CHECK(valid_utf8_end(b));
    CHECK(b.find("Ravi") != std::string::npos);
}

TEST(truncate_utf8_backs_off) {
    std::string t = "ab\xE0\xB0\xA4";  // 2 + 3 bytes
    CHECK_EQ(sve::truncate_utf8(t, 4), std::string("ab"));
    CHECK_EQ(sve::truncate_utf8(t, 5), t);
    CHECK_EQ(sve::truncate_utf8(t, 3), std::string("ab"));
}

TEST(always_block_drops_vocab_before_preferences) {
    Store s(":memory:");
    s.setting_set("profile", R"({"name":"Ravi"})");
    s.add_memory("preference", "likes tea", std::nullopt);
    for (int i = 0; i < 100; i++) s.add_memory("vocab", "word" + std::to_string(i), std::nullopt);
    std::string b = sve::always_block(s, 300);
    CHECK(b.size() <= 300);
    CHECK(b.find("likes tea") != std::string::npos);
    CHECK(b.find("word99") != std::string::npos);   // newest vocab kept
    CHECK(b.find("word0,") == std::string::npos);   // oldest dropped
}

TEST(always_block_empty_when_nothing) {
    Store s(":memory:");
    CHECK_EQ(sve::always_block(s), std::string());
    CHECK_EQ(sve::relevant_block(s, "salt", 1000), std::string());
}

TEST(relevant_block_labels_sources) {
    Store s(":memory:");
    int64_t id = s.create("Doctor visit", "en", "a.m4a", "tx", 300);
    s.set_pieces(id, json::parse(R"([{"start_sec":252,"text":"reduce salt in food"},{"start_sec":65,"text":"also less salt at dinner"}])"));
    s.add_memory("fact", "avoids salt", std::nullopt);
    std::string b = sve::relevant_block(s, "salt", 2000);
    CHECK(b.find("[S1]") != std::string::npos);
    CHECK(b.find("[S2]") != std::string::npos);
    CHECK(b.find("avoids salt") != std::string::npos);
    CHECK(b.find("@4:12") != std::string::npos);
    CHECK(b.find("@1:05") != std::string::npos);
    json src = sve::relevant_sources(s, "salt");
    CHECK_EQ(src.size(), (size_t)2);
    CHECK_EQ(src[0]["label"].get<std::string>(), std::string("S1"));
    CHECK_EQ(src[0]["note_id"].get<int64_t>(), id);
    CHECK_EQ(src[0]["title"].get<std::string>(), std::string("Doctor visit"));
    int first = src[0]["start_sec"].get<int>();
    CHECK(first == 252 || first == 65);
    std::string tag = first == 252 ? "@4:12" : "@1:05";
    CHECK(b.find("[S1] Doctor visit " + tag) != std::string::npos);
}
