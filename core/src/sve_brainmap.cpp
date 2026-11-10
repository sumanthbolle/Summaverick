#include "sve_brainmap.h"
#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>
#include "sve_memory.h"  // kDirectToMapDefault

namespace sve {
namespace {

struct Item {
    std::string ref, label;
    double importance = 0;
    bool is_new = false, low = false, done = false;
    int count = 0;
    bool counted = false;
};

bool has_source(const json &m, int64_t note_id) {
    if (!m.contains("sources") || !m["sources"].is_array()) return false;
    for (const auto &s : m["sources"])
        if (s.is_object() && s.value("note_id", (int64_t)-1) == note_id) return true;
    return false;
}

bool shown_on_map(const json &m) {
    if (m.value("queue", 0) != 0) return false;
    if (m.contains("superseded_by") && !m["superseded_by"].is_null()) return false;
    const std::string review = m.value("review", std::string());
    if (review == "wrong") return false;
    if (kDirectToMapDefault) return true;
    return review == "kept" || review == "corrected";
}

json item_json(const Item &it) {
    return {{"ref", it.ref}, {"label", it.label}, {"importance", it.importance}, {"new", it.is_new},
            {"low_confidence", it.low}, {"done", it.done}, {"count", it.count}};
}

std::string name_of(const json &list, int64_t id) {
    if (list.is_array())
        for (const auto &e : list)
            if (e.is_object() && e.value("id", (int64_t)-1) == id) return e.value("name", std::string());
    return "";
}

void add_branch(json &out, const std::string &kind, std::vector<Item> items, int per_branch, bool open_first) {
    if (items.empty()) return;
    std::stable_sort(items.begin(), items.end(), [&](const Item &a, const Item &b) {
        if (open_first && a.done != b.done) return !a.done;
        return a.importance > b.importance;
    });
    const int cap = std::max(per_branch, 0);
    json arr = json::array();
    for (size_t i = 0; i < items.size() && (int)i < cap; i++) arr.push_back(item_json(items[i]));
    out.push_back({{"kind", kind}, {"items", arr}, {"more", (int)items.size() - (int)arr.size()}});
}

}  // namespace

json build_brain_map(const json &memories, const json &people, const json &topics, const json &meaning_pairs,
                     int64_t note_id, int per_branch) {
    std::vector<const json *> mems;
    if (memories.is_array())
        for (const auto &m : memories)
            if (m.is_object() && shown_on_map(m) && (note_id <= 0 || has_source(m, note_id))) mems.push_back(&m);

    std::map<int64_t, Item> person_items, topic_items;
    std::vector<Item> promises, decisions, questions;
    std::map<int64_t, std::set<int64_t>> by_person, by_topic;  // entity id -> memory ids
    std::map<int64_t, Item> shown;                             // memory id -> item (promise/decision/question)

    auto touch = [&](std::map<int64_t, Item> &into, const json &list, const char *prefix, int64_t tid, const json &m,
                     const Item &mi) {
        Item &it = into[tid];
        if (it.ref.empty()) {
            it.ref = prefix + std::to_string(tid);
            it.label = name_of(list, tid);
        }
        it.count++;
        it.importance = std::max(it.importance, mi.importance);
        it.is_new = it.is_new || mi.is_new;
        it.low = it.low || mi.low;
        (void)m;
    };

    for (const json *mp : mems) {
        const json &m = *mp;
        const int64_t id = m.value("id", (int64_t)0);
        Item mi;
        mi.ref = "m" + std::to_string(id);
        mi.label = m.value("text", std::string());
        mi.importance = m.value("importance", 0.0);
        mi.is_new = m.value("review", std::string()) == "new";
        mi.low = m.value("confidence", std::string()) == "low";
        mi.done = m.value("done", 0) != 0;
        const std::string type = m.value("type", std::string());
        if (type == "promise") promises.push_back(mi);
        else if (type == "decision") decisions.push_back(mi);
        else if (type == "question") questions.push_back(mi);
        if (type == "promise" || type == "decision" || type == "question") shown[id] = mi;
        std::set<int64_t> seen;
        if (m.contains("people") && m["people"].is_array())
            for (const auto &p : m["people"]) {
                const int64_t pid = p.value("id", (int64_t)0);
                if (pid <= 0 || !seen.insert(pid).second) continue;
                touch(person_items, people, "t", pid, m, mi);
                by_person[pid].insert(id);
            }
        seen.clear();
        if (m.contains("topics") && m["topics"].is_array())
            for (const auto &t : m["topics"]) {
                const int64_t tid = t.value("id", (int64_t)0);
                if (tid <= 0 || !seen.insert(tid).second) continue;
                touch(topic_items, topics, "t", tid, m, mi);
                by_topic[tid].insert(id);
            }
    }

    // A promise's open/closed state is not a topic's: topics and people are never "done".
    std::vector<Item> ppl, tps;
    for (auto &kv : person_items) ppl.push_back(kv.second);
    for (auto &kv : topic_items) tps.push_back(kv.second);

    json out = {{"center", {{"kind", note_id > 0 ? "recording" : "you"}, {"note_id", note_id > 0 ? note_id : 0}}},
                {"branches", json::array()}, {"links", json::array()}};
    json &br = out["branches"];
    add_branch(br, "people", ppl, per_branch, false);
    add_branch(br, "topics", tps, per_branch, false);
    add_branch(br, "promises", promises, per_branch, true);
    add_branch(br, "decisions", decisions, per_branch, true);
    add_branch(br, "questions", questions, per_branch, true);

    int total = 0;
    std::set<std::string> shown_refs;
    for (const auto &b : br) {
        total += b["more"].get<int>();  // items beyond the cap still count toward "few"
        for (const auto &it : b["items"]) { total++; shown_refs.insert(it["ref"].get<std::string>()); }
    }
    out["list"] = total < 8;

    // Links among memories that are drawn: a shared person or topic, then meaning.
    json &links = out["links"];
    auto on_map = [&](int64_t id) { return shown_refs.count("m" + std::to_string(id)) > 0; };
    auto share = [&](const std::map<int64_t, std::set<int64_t>> &groups, const char *kind) {
        std::set<std::pair<int64_t, int64_t>> done;
        for (const auto &g : groups) {
            std::vector<int64_t> ids;
            for (int64_t id : g.second) if (on_map(id)) ids.push_back(id);
            for (size_t i = 0; i < ids.size(); i++)
                for (size_t j = i + 1; j < ids.size(); j++)
                    if (done.insert({ids[i], ids[j]}).second)
                        links.push_back({{"a", "m" + std::to_string(ids[i])}, {"b", "m" + std::to_string(ids[j])}, {"kind", kind}});
        }
    };
    share(by_person, "person");
    share(by_topic, "topic");

    // Meaning: cosine >= 0.8, strongest first, at most 2 per memory, both memories eligible.
    struct Pair { int64_t a, b; double cos; };
    std::vector<Pair> pairs;
    if (meaning_pairs.is_array())
        for (const auto &p : meaning_pairs) {
            if (!p.is_object() || !p.contains("a") || !p.contains("b") || !p.contains("cos")) continue;
            const int64_t a = p["a"].get<int64_t>(), b = p["b"].get<int64_t>();
            const double c = p["cos"].get<double>();
            if (a != b && c >= 0.8 && shown.count(a) && shown.count(b)) pairs.push_back({a, b, c});
        }
    std::stable_sort(pairs.begin(), pairs.end(), [](const Pair &x, const Pair &y) { return x.cos > y.cos; });
    std::map<int64_t, int> used;
    std::set<std::pair<int64_t, int64_t>> seen;
    for (const auto &p : pairs) {
        auto key = std::make_pair(std::min(p.a, p.b), std::max(p.a, p.b));
        if (used[p.a] >= 2 || used[p.b] >= 2 || !on_map(p.a) || !on_map(p.b) || !seen.insert(key).second) continue;
        used[p.a]++;
        used[p.b]++;
        links.push_back({{"a", "m" + std::to_string(p.a)}, {"b", "m" + std::to_string(p.b)}, {"kind", "meaning"}});
    }
    return out;
}

}  // namespace sve
