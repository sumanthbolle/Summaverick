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
    json src = sve::relevant_sources(s, "salt", 2000);
    CHECK_EQ(src.size(), (size_t)2);
    CHECK_EQ(src[0]["label"].get<std::string>(), std::string("S1"));
    CHECK_EQ(src[0]["note_id"].get<int64_t>(), id);
    CHECK_EQ(src[0]["title"].get<std::string>(), std::string("Doctor visit"));
    int first = src[0]["start_sec"].get<int>();
    CHECK(first == 252 || first == 65);
    std::string tag = first == 252 ? "@4:12" : "@1:05";
    CHECK(b.find("[S1] Doctor visit " + tag) != std::string::npos);
}

TEST(relevant_budget_keeps_labels_and_sources_in_sync) {
    Store s(":memory:");
    int64_t id = s.create("Talk", "en", "a.m4a", "tx", 300);
    json ps = json::array();
    for (int i = 0; i < 6; i++) ps.push_back({{"start_sec", i * 10}, {"text", "salt note number " + std::to_string(i) + " with some padding words"}});
    s.set_pieces(id, ps);
    s.add_memory("fact", "avoids salt", std::nullopt);
    for (size_t budget : {60u, 120u, 200u, 400u, 2000u}) {
        sve::Relevant r = sve::relevant(s, "salt", budget);
        CHECK(r.block.size() <= budget);
        for (int k = 1; k <= 6; k++) {
            bool in_block = r.block.find("[S" + std::to_string(k) + "]") != std::string::npos;
            bool in_src = false;
            for (auto &x : r.sources) if (x["label"].get<std::string>() == "S" + std::to_string(k)) in_src = true;
            CHECK_EQ(in_block, in_src);
        }
        CHECK(sve::relevant_block(s, "salt", budget) == r.block);
        CHECK(sve::relevant_sources(s, "salt", budget) == r.sources);
    }
    CHECK_EQ(sve::relevant(s, "salt", 2000).sources.size(), (size_t)6);
    CHECK(sve::relevant(s, "salt", 200).sources.size() < 6);
}

TEST(relevant_oversized_first_piece_truncated_but_kept) {
    Store s(":memory:");
    int64_t id = s.create("Talk", "en", "a.m4a", "tx", 300);
    s.set_pieces(id, json::parse(R"([{"start_sec":7,"text":"salt salt salt salt salt salt salt salt salt salt salt salt"}])"));
    sve::Relevant r = sve::relevant(s, "salt", 50);
    CHECK(r.block.size() <= 50);
    CHECK(r.block.find("[S1]") != std::string::npos);
    CHECK_EQ(r.sources.size(), (size_t)1);
}

TEST(always_block_drops_preferences_after_vocab) {
    Store s(":memory:");
    s.setting_set("profile", R"({"name":"Ravi"})");
    for (int i = 0; i < 30; i++) s.add_memory("preference", "pref" + std::to_string(i) + " xxxxxxxxxx", std::nullopt);
    s.add_memory("vocab", "zzvocab", std::nullopt);
    std::string b = sve::always_block(s, 200);
    CHECK(b.size() <= 200);
    CHECK(b.find("zzvocab") == std::string::npos);
    CHECK(b.find("pref29") != std::string::npos);
    CHECK(b.find("pref0 ") == std::string::npos);
    CHECK(b.find("Ravi") != std::string::npos);
}

TEST(always_block_malformed_profile_and_sparse_medicine) {
    Store s(":memory:");
    s.setting_set("profile", "{not json");
    s.add_medicine(json::parse(R"({"name":"Metformin"})"));
    s.add_medicine(json::parse(R"({"name":"","dose":"1 mg"})"));
    std::string b = sve::always_block(s);
    CHECK(b.find("About the person") == std::string::npos);
    CHECK(b.find("- Metformin daily\n") != std::string::npos);
    CHECK(b.find("1 mg") == std::string::npos);
}

TEST(context_system_prefixes_always_block) {
    Store s(":memory:");
    CHECK_EQ(sve::context_system(s, "SYS"), std::string("SYS"));
    s.setting_set("profile", R"({"name":"Ravi"})");
    std::string c = sve::context_system(s, "SYS");
    CHECK(c.find("Ravi") != std::string::npos);
    CHECK(c.size() > 3 && c.substr(c.size() - 3) == "SYS");
}

TEST(transcription_vocab_line_from_saved_vocab) {
    Store s(":memory:");
    CHECK_EQ(sve::transcription_vocab_line(s), std::string());
    s.add_medicine(json::parse(R"({"name":"Amlodipine","dose":"5 mg"})"));
    s.add_memory("fact", "avoids salt", std::nullopt);
    CHECK_EQ(sve::transcription_vocab_line(s), std::string());  // vocabulary only
    s.add_memory("vocab", "Lakshmi", std::nullopt);
    s.add_memory("vocab", "Dr Rao", std::nullopt);
    CHECK_EQ(sve::transcription_vocab_line(s), std::string("Spell these words this way: Dr Rao, Lakshmi"));
}

TEST(transcription_vocab_line_budget_utf8) {
    Store s(":memory:");
    for (int i = 0; i < 200; i++)
        s.add_memory("vocab", "\xE0\xB0\xA4\xE0\xB1\x86\xE0\xB0\xB2\xE0\xB1\x81" + std::to_string(i), std::nullopt);
    std::string l = sve::transcription_vocab_line(s, 400);
    CHECK(l.size() <= 400);
    CHECK(valid_utf8_end(l));
    CHECK(l.find("199") != std::string::npos);        // newest kept
    CHECK(l.find("\x81" "0,") == std::string::npos);  // oldest dropped
    Store t(":memory:");
    t.add_memory("vocab", std::string(300, 'a') + "\xE0\xB0\xA4\xE0\xB0\xA4", std::nullopt);
    std::string one = sve::transcription_vocab_line(t, 330);
    CHECK(one.size() <= 330 && valid_utf8_end(one));
}

TEST(notes_block_has_no_medicine_line) {
    Store s(":memory:");
    s.setting_set("profile", R"({"name":"Ravi","routine":{"dinner":"20:00"}})");
    s.add_medicine(json::parse(R"({"name":"Amlodipine","dose":"5 mg","when_text":"after dinner","times":["20:00"],"repeat":"daily"})"));
    s.add_memory("vocab", "Dr Rao", std::nullopt);
    std::string full = sve::always_block(s);
    CHECK(full.find("Amlodipine") != std::string::npos);
    std::string notes = sve::always_block(s, 2000, false);
    CHECK(notes.find("Medicines") == std::string::npos);
    CHECK(notes.find("Amlodipine") == std::string::npos);
    CHECK(notes.find("5 mg") == std::string::npos);
    CHECK(notes.find("Ravi") != std::string::npos);
    CHECK(notes.find("Dr Rao") != std::string::npos);
    std::string sys = sve::context_system(s, "SYS", false);
    CHECK(sys.find("Amlodipine") == std::string::npos && sys.find("Ravi") != std::string::npos);
    CHECK(sve::context_system(s, "SYS").find("Amlodipine") != std::string::npos);  // Ask, extraction: full block
    Store m(":memory:");
    m.add_medicine(json::parse(R"({"name":"Metformin","dose":"500 mg"})"));
    CHECK_EQ(sve::context_system(m, "SYS", false), std::string("SYS"));  // only medicines known: nothing added
}
