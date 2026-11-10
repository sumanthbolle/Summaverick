#include "check.h"
#include "sve_brainmap.h"
#include "sve_store.h"

using namespace sve;

namespace {

json mem(int id, const std::string &type, double imp, const std::string &review = "kept", int queue = 0) {
    return {{"id", id}, {"type", type}, {"text", "m" + std::to_string(id)}, {"importance", imp}, {"review", review},
            {"queue", queue}, {"done", 0}, {"confidence", "high"}, {"superseded_by", nullptr},
            {"sources", json::array({{{"note_id", 1}}})}, {"people", json::array()}, {"topics", json::array()}};
}

const json kNone = json::array();

const json *branch(const json &m, const std::string &kind) {
    for (const auto &b : m["branches"]) if (b["kind"] == kind) return &b;
    return nullptr;
}

}  // namespace

TEST(map_orders_by_importance_and_caps) {
    json ms = json::array();
    for (int i = 1; i <= 9; i++) ms.push_back(mem(i, "promise", i));
    json m = build_brain_map(ms, kNone, kNone, kNone, 0);
    const json *b = branch(m, "promises");
    CHECK(b != nullptr);
    CHECK_EQ(b->at("items").size(), (size_t)6);
    CHECK_EQ(b->at("more").get<int>(), 3);
    CHECK_EQ(b->at("items")[0]["ref"].get<std::string>(), std::string("m9"));
    CHECK_EQ(b->at("items")[0]["label"].get<std::string>(), std::string("m9"));
    CHECK(m["center"]["kind"] == "you");
}

TEST(map_open_before_done) {
    json a = mem(1, "promise", 9), b = mem(2, "promise", 1);
    a["done"] = 1;
    json m = build_brain_map(json::array({a, b}), kNone, kNone, kNone, 0);
    CHECK_EQ((*branch(m, "promises"))["items"][0]["ref"].get<std::string>(), std::string("m2"));
    CHECK((*branch(m, "promises"))["items"][1]["done"] == true);
}

TEST(map_excludes_wrong_queued_superseded) {
    json w = mem(1, "decision", 5, "wrong"), q = mem(2, "decision", 5, "kept", 1), s = mem(3, "decision", 5);
    s["superseded_by"] = 9;
    json ok = mem(4, "decision", 1), nw = mem(5, "decision", 5, "new");
    json m = build_brain_map(json::array({w, q, s, ok, nw}), kNone, kNone, kNone, 0);
    const json *b = branch(m, "decisions");
    CHECK(b != nullptr);
    CHECK_EQ(b->at("items").size(), (size_t)1);
    CHECK_EQ(b->at("items")[0]["ref"].get<std::string>(), std::string("m4"));
}

TEST(map_list_when_few) {
    json ms = json::array();
    for (int i = 1; i <= 5; i++) ms.push_back(mem(i, "question", i));
    CHECK(build_brain_map(ms, kNone, kNone, kNone, 0)["list"] == true);
    for (int i = 6; i <= 9; i++) ms.push_back(mem(i, "question", i));
    CHECK(build_brain_map(ms, kNone, kNone, kNone, 0)["list"] == false);
}

TEST(map_per_recording_filters_and_centres) {
    json a = mem(1, "promise", 1), b = mem(2, "promise", 1);
    b["sources"] = json::array({{{"note_id", 7}}});
    json m = build_brain_map(json::array({a, b}), kNone, kNone, kNone, 7);
    CHECK(m["center"]["kind"] == "recording");
    CHECK_EQ(m["center"]["note_id"].get<int>(), 7);
    CHECK_EQ((*branch(m, "promises"))["items"].size(), (size_t)1);
    CHECK_EQ((*branch(m, "promises"))["items"][0]["ref"].get<std::string>(), std::string("m2"));
}

TEST(map_meaning_links_capped) {
    json ms = json::array();
    for (int i = 1; i <= 4; i++) ms.push_back(mem(i, "promise", 1));
    json pairs = json::array({{{"a", 1}, {"b", 2}, {"cos", 0.95}}, {{"a", 1}, {"b", 3}, {"cos", 0.9}},
                              {{"a", 1}, {"b", 4}, {"cos", 0.85}}, {{"a", 2}, {"b", 3}, {"cos", 0.5}}});
    json m = build_brain_map(ms, kNone, kNone, pairs, 0);
    int n = 0;
    for (const auto &l : m["links"]) if (l["kind"] == "meaning") { n++; CHECK(l["a"] == "m1" || l["b"] == "m1"); }
    CHECK_EQ(n, 2);
}

TEST(map_direct_off_shows_only_kept) {
    json ms = json::array({mem(1, "promise", 1, "new"), mem(2, "promise", 1, "kept"), mem(3, "promise", 1, "corrected")});
    json m = build_brain_map(ms, kNone, kNone, kNone, 0);
    CHECK_EQ((*branch(m, "promises"))["items"].size(), (size_t)2);
}

TEST(map_people_topics_counts_and_links) {
    json a = mem(1, "promise", 3), b = mem(2, "fact", 2), c = mem(3, "idea", 1);
    a["people"] = json::array({{{"id", 10}, {"name", "Ravi"}, {"role", "who"}}});
    b["people"] = json::array({{{"id", 10}, {"name", "Ravi"}, {"role", "mentioned"}}});
    b["topics"] = json::array({{{"id", 20}}});
    c["topics"] = json::array({{{"id", 20}}});
    json people = json::array({{{"id", 10}, {"kind", "person"}, {"name", "Ravi"}}});
    json topics = json::array({{{"id", 20}, {"kind", "topic"}, {"name", "Budget"}}});
    json m = build_brain_map(json::array({a, b, c}), people, topics, kNone, 0);
    const json *p = branch(m, "people"), *t = branch(m, "topics");
    CHECK(p && t);
    CHECK_EQ(p->at("items")[0]["ref"].get<std::string>(), std::string("t10"));
    CHECK_EQ(p->at("items")[0]["label"].get<std::string>(), std::string("Ravi"));
    CHECK_EQ(p->at("items")[0]["count"].get<int>(), 2);
    CHECK_EQ(t->at("items")[0]["count"].get<int>(), 2);
    CHECK(branch(m, "promises") != nullptr);
    CHECK(branch(m, "facts") == nullptr);
    bool person_link = false;
    for (const auto &l : m["links"]) if (l["kind"] == "person") person_link = true;
    CHECK(!person_link);  // only one shown memory (the promise) carries Ravi
}

TEST(store_topic_memories_and_map_rows) {
    sve::Store st(":memory:");
    CHECK(st.ok());
    int64_t note = st.create("n", "en", nullptr, "x", 1);
    int64_t person = st.upsert_topic("person", "Ravi"), topic = st.upsert_topic("topic", "Budget");
    int64_t a = st.add_memory_item({{"type", "promise"}, {"text", "Send report"}, {"review", "kept"}, {"importance", 1.0}});
    int64_t b = st.add_memory_item({{"type", "fact"}, {"text", "Budget is 5 lakh"}, {"review", "kept"}, {"importance", 2.0}});
    int64_t q = st.add_memory_item({{"type", "fact"}, {"text", "Queued"}, {"review", "kept"}, {"queue", 1}});
    for (int64_t m : {a, b, q}) CHECK(st.add_memory_source(m, note, 0, "quote", std::nullopt));
    for (int64_t m : {a, b, q}) CHECK(st.link_memory_entity(m, person, "mentioned"));
    CHECK(st.link_memory_entity(b, topic, "topic"));
    json under = st.topic_memories(person);
    CHECK_EQ(under.size(), (size_t)2);  // queued one left out
    CHECK_EQ(under[0]["id"].get<int64_t>(), b);  // by importance
    CHECK_EQ(st.topic_memories(topic).size(), (size_t)1);
    json rows = st.memory_items_with_topics({{"queue", 0}});
    json map = build_brain_map(rows, st.topics("person"), st.topics("topic"), json::array(), 0);
    bool saw_ravi = false, saw_budget = false;
    for (const auto &br : map["branches"])
        for (const auto &it : br["items"]) {
            if (it["label"] == "Ravi") { saw_ravi = true; CHECK_EQ(it["count"].get<int>(), 2); }
            if (it["label"] == "Budget") saw_budget = true;
        }
    CHECK(saw_ravi && saw_budget);
    CHECK(st.update_memory_item(a, {{"last_used", 12345}}));
    CHECK_EQ(st.memory_item(a)["last_used"].get<int>(), 12345);
}
