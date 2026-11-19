// Relations: who decided, asked or told what in one recording (sve_consolidate.h), their review and the map.
#include "check.h"
#include <sqlite3.h>
#include <cstdlib>
#include <unistd.h>
#include "sve_brainmap.h"
#include "sve_consolidate.h"
#include "sve_store.h"

using namespace sve;

namespace {

std::string tmp_db() {
    const char *dir = getenv("TMPDIR");
    std::string t = std::string(dir && *dir ? dir : "/tmp") + "/sve_rel_XXXXXX";
    std::vector<char> buf(t.begin(), t.end());
    buf.push_back('\0');
    int fd = mkstemp(buf.data());
    if (fd >= 0) close(fd);
    return std::string(buf.data());
}

void drop_db(const std::string &p) {
    for (const char *suf : {"", "-wal", "-shm"}) unlink((p + suf).c_str());
}

void raw_exec(const std::string &p, const char *sql) {
    sqlite3 *db = nullptr;
    sqlite3_open(p.c_str(), &db);
    char *e = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &e) != SQLITE_OK) {
        std::fprintf(stderr, "raw: %s\n", e ? e : "?");
        sqlite3_free(e);
    }
    sqlite3_close(db);
}

// A memory from note n at start_sec, linked to people by role, and to a topic.
int64_t memory(Store &s, int64_t n, const std::string &type, const std::string &text, const std::string &who,
               int64_t who_topic, const std::string &to_whom = "", int64_t to_topic = 0, int64_t topic = 0, int64_t start = 5) {
    int64_t id = s.add_memory_item({{"type", type}, {"text", text}, {"who", who.empty() ? json(nullptr) : json(who)},
                                    {"to_whom", to_whom.empty() ? json(nullptr) : json(to_whom)}, {"review", "kept"}});
    s.add_memory_source(id, n, start, "quote of " + text, std::nullopt);
    if (who_topic > 0) s.link_memory_entity(id, who_topic, "who");
    if (to_topic > 0) s.link_memory_entity(id, to_topic, "to_whom");
    if (topic > 0) s.link_memory_entity(id, topic, "topic");
    return id;
}

// A recording where Ravi decided on Friday, Sita asked about the budget, and "I" told Ravi to call.
struct Meeting {
    int64_t note = 0, ravi = 0, sita = 0, budget = 0, decision = 0, question = 0, promise = 0;
};

Meeting meeting(Store &s) {
    Meeting m;
    m.note = s.create("Team meeting", "en", nullptr, "t", 60);
    m.ravi = s.upsert_topic("person", "Ravi");
    m.sita = s.upsert_topic("person", "Sita");
    m.budget = s.upsert_topic("topic", "Budget");
    s.link_topic(m.note, m.ravi);
    s.link_topic(m.note, m.sita);
    s.link_topic(m.note, m.budget);
    m.decision = memory(s, m.note, "decision", "Launch on Friday", "Ravi", m.ravi, "", 0, m.budget, 10);
    m.question = memory(s, m.note, "question", "Is the budget approved?", "Sita", m.sita, "Ravi", m.ravi, m.budget, 20);
    m.promise = memory(s, m.note, "promise", "Call the vendor", "I", 0, "Ravi", m.ravi, 0, 30);
    return m;
}

json find(const json &rows, const std::string &relation, const std::string &to_kind) {
    json out = json::array();
    for (const auto &r : rows)
        if (r["relation"] == relation && r["to_kind"] == to_kind) out.push_back(r);
    return out;
}

const json *branch(const json &m, const std::string &kind) {
    for (const auto &b : m["branches"]) if (b["kind"] == kind) return &b;
    return nullptr;
}

int count_links(const json &map, const std::string &kind) {
    int n = 0;
    for (const auto &l : map["links"]) if (l["kind"] == kind) n++;
    return n;
}

json map_of(Store &s, int64_t note) {
    return build_brain_map(s.memory_items_with_topics({{"note_id", note}}), s.topics("person"), s.topics("topic"), json::array(),
                           note, 6, s.relations({{"status", "accepted"}, {"note_id", note}}));
}

}  // namespace

TEST(names_me_words) {
    CHECK(names_me("I", ""));
    CHECK(names_me(" we ", ""));
    CHECK(names_me("నేను", ""));
    CHECK(names_me("Sumanth", "sumanth"));
    CHECK(!names_me("Ravi", "sumanth"));
    CHECK(!names_me("he", ""));
    CHECK(!names_me("", ""));
}

TEST(derive_from_memories) {
    Store s(":memory:");
    Meeting m = meeting(s);
    json rows = derive_relations(m.note, s.memory_items_with_topics({{"note_id", m.note}}), s.note_topics(m.note, "person"), "");
    // Both people involved, in the order the note names them.
    json inv = find(rows, "involved_in", "note");
    CHECK_EQ(inv.size(), (size_t)2);
    if (inv.size() == 2) { CHECK(inv[0]["from_topic"] == m.ravi); CHECK(inv[1]["from_topic"] == m.sita); }
    json dec = find(rows, "decided", "decision");
    CHECK_EQ(dec.size(), (size_t)1);
    if (dec.size() == 1) {
        CHECK(dec[0]["from_topic"] == m.ravi);
        CHECK(dec[0]["from_kind"] == "person");
        CHECK(dec[0]["memory_id"] == m.decision);
        CHECK(dec[0]["to_text"] == "Launch on Friday");
        CHECK(dec[0]["quote"] == "quote of Launch on Friday");
        CHECK(dec[0]["start_sec"] == 10);
    }
    json q = find(rows, "asked", "question");
    CHECK_EQ(q.size(), (size_t)1);
    if (q.size() == 1) CHECK(q[0]["from_topic"] == m.sita);
    // Sita asked Ravi (person to person); "I" told Ravi (the owner).
    json ap = find(rows, "asked", "person");
    CHECK_EQ(ap.size(), (size_t)1);
    if (ap.size() == 1) { CHECK(ap[0]["from_topic"] == m.sita); CHECK(ap[0]["to_topic"] == m.ravi); }
    json told = find(rows, "told", "person");
    CHECK_EQ(told.size(), (size_t)1);
    if (told.size() == 1) { CHECK(told[0]["from_kind"] == "me"); CHECK(told[0]["to_topic"] == m.ravi); }
    // The decision and the question are about the budget.
    CHECK_EQ(find(rows, "about", "topic").size(), (size_t)2);
}

TEST(derive_skips_done_questions_wrong_and_unnamed_people) {
    Store s(":memory:");
    int64_t n = s.create("n", "en", nullptr, "t", 1);
    int64_t done = memory(s, n, "question", "Was it paid?", "", 0);
    s.update_memory_item(done, {{"done", 1}});
    int64_t wrong = memory(s, n, "decision", "Sell the car", "", 0);
    s.update_memory_item(wrong, {{"review", "wrong"}});
    memory(s, n, "decision", "Paint the house", "he", 0);  // a pronoun names nobody
    memory(s, n, "fact", "The sky is blue", "", 0);
    json rows = derive_relations(n, s.memory_items_with_topics({{"note_id", n}}), s.note_topics(n, "person"), "");
    CHECK_EQ(rows.size(), (size_t)1);
    if (rows.size() == 1) {
        CHECK(rows[0]["relation"] == "decided");
        CHECK(rows[0]["from_kind"] == "person");
        CHECK(rows[0]["from_topic"].is_null());
        CHECK(rows[0]["from_name"] == "");
    }
}

TEST(review_lists_people_decisions_questions) {
    Store s(":memory:");
    Meeting m = meeting(s);
    CHECK(refresh_relations(s, m.note) > 0);
    json rv = relation_reviews(s);
    CHECK_EQ(rv.size(), (size_t)1);
    if (rv.size() != 1) return;
    CHECK(rv[0]["note_id"] == m.note);
    CHECK(rv[0]["title"] == "Team meeting");
    CHECK_EQ(rv[0]["people"].size(), (size_t)2);
    CHECK_EQ(rv[0]["decisions"].size(), (size_t)1);
    CHECK(rv[0]["decisions"][0]["who"] == "Ravi");
    CHECK(rv[0]["decisions"][0]["who_kind"] == "person");
    CHECK_EQ(rv[0]["questions"].size(), (size_t)1);
    CHECK(rv[0]["questions"][0]["who"] == "Sita");
    CHECK_EQ(rv[0]["between"].size(), (size_t)2);
    // Running again replaces the pending rows instead of adding to them.
    int before = (int)s.relations({{"note_id", m.note}}).size();
    refresh_relations(s, m.note);
    CHECK_EQ((int)s.relations({{"note_id", m.note}}).size(), before);
}

TEST(map_uses_only_accepted_relations) {
    Store s(":memory:");
    Meeting m = meeting(s);
    refresh_relations(s, m.note);
    // Pending: no relation links.
    json before = map_of(s, m.note);
    CHECK_EQ(count_links(before, "relation"), 0);
    // Even if the caller passed pending rows, they are not drawn.
    json leaked = build_brain_map(s.memory_items_with_topics({{"note_id", m.note}}), s.topics("person"), s.topics("topic"),
                                  json::array(), m.note, 6, s.relations({{"note_id", m.note}}));
    CHECK_EQ(count_links(leaked, "relation"), 0);
    CHECK(accept_relations(s, m.note, json::array()));
    json after = map_of(s, m.note);
    bool decided = false, asked = false, asked_person = false;
    for (const auto &l : after["links"]) {
        if (l["kind"] != "relation") continue;
        if (l["label"] == "decided" && l["a"] == "t" + std::to_string(m.ravi) && l["b"] == "m" + std::to_string(m.decision)) decided = true;
        if (l["label"] == "asked" && l["a"] == "t" + std::to_string(m.sita) && l["b"] == "m" + std::to_string(m.question)) asked = true;
        if (l["label"] == "asked" && l["a"] == "t" + std::to_string(m.sita) && l["b"] == "t" + std::to_string(m.ravi)) asked_person = true;
    }
    CHECK(decided);
    CHECK(asked);
    CHECK(asked_person);
    // The "me" relation is not drawn (the owner is the centre, not a point).
    CHECK_EQ(count_links(after, "relation"), 3);
    // Open questions show as open.
    const json *qs = branch(after, "questions");
    CHECK(qs != nullptr);
    if (qs) CHECK(qs->at("items")[0]["open"] == true);
    CHECK(relation_reviews(s).empty());
}

TEST(accept_with_edits) {
    Store s(":memory:");
    Meeting m = meeting(s);
    refresh_relations(s, m.note);
    json rv = relation_reviews(s)[0];
    int64_t ravi_line = 0, sita_line = 0;
    for (const auto &p : rv["people"]) {
        if (p["name"] == "Ravi") ravi_line = p["id"].get<int64_t>();
        if (p["name"] == "Sita") sita_line = p["id"].get<int64_t>();
    }
    const int64_t dec_line = rv["decisions"][0]["id"].get<int64_t>(), q_line = rv["questions"][0]["id"].get<int64_t>();
    // Ravi was misheard: it was Raju. The decision is Saturday, and it was the owner who asked.
    json edits = json::array({{{"id", ravi_line}, {"name", "Raju"}},
                              {{"id", dec_line}, {"text", "Launch on Saturday"}},
                              {{"id", q_line}, {"who", "me"}}});
    CHECK(accept_relations(s, m.note, edits));
    CHECK(s.relations({{"note_id", m.note}, {"status", "pending"}}).empty());
    // Raju is a person of this note; Ravi, used by nothing else, is gone.
    json people = s.note_topics(m.note, "person");
    bool raju = false, ravi = false;
    for (const auto &p : people) { raju = raju || p["name"] == "Raju"; ravi = ravi || p["name"] == "Ravi"; }
    CHECK(raju);
    CHECK(!ravi);
    bool ravi_known = false;
    for (const auto &t : s.topics("person")) ravi_known = ravi_known || t["name"] == "Ravi";
    CHECK(!ravi_known);
    // The decision's memory is corrected and its who follows the new name.
    json d = s.memory_item(m.decision);
    CHECK(d["text"] == "Launch on Saturday");
    CHECK(d["review"] == "corrected");
    CHECK(d["who"] == "Raju");
    bool raju_who = false;
    for (const auto &p : d["people"]) raju_who = raju_who || (p["name"] == "Raju" && p["role"] == "who");
    CHECK(raju_who);
    // The question: asked by the owner now, no person who-link.
    json q = s.memory_item(m.question);
    CHECK(q["who"] == "me");
    for (const auto &p : q["people"]) CHECK(p["role"] != "who");
    json accepted = s.relations({{"note_id", m.note}, {"status", "accepted"}});
    json dec = find(accepted, "decided", "decision");
    CHECK_EQ(dec.size(), (size_t)1);
    if (dec.size() == 1) {
        CHECK(dec[0]["from_name"] == "Raju");
        CHECK(dec[0]["to_text"] == "Launch on Saturday");
        CHECK(dec[0]["edited"] == true);
    }
    json qq = find(accepted, "asked", "question");
    CHECK_EQ(qq.size(), (size_t)1);
    if (qq.size() == 1) CHECK(qq[0]["from_kind"] == "me");
    (void)sita_line;
    // Accepting again: nothing pending.
    CHECK(!accept_relations(s, m.note, json::array()));
    // Made again later, nothing already decided comes back.
    CHECK_EQ(refresh_relations(s, m.note), 0);
}

TEST(accept_with_removed_lines) {
    Store s(":memory:");
    Meeting m = meeting(s);
    refresh_relations(s, m.note);
    json rv = relation_reviews(s)[0];
    int64_t ravi_line = 0;
    for (const auto &p : rv["people"]) if (p["name"] == "Ravi") ravi_line = p["id"].get<int64_t>();
    const int64_t q_line = rv["questions"][0]["id"].get<int64_t>();
    CHECK(accept_relations(s, m.note, json::array({{{"id", ravi_line}, {"removed", true}}, {{"id", q_line}, {"removed", true}}})));
    json acc = s.relations({{"note_id", m.note}, {"status", "accepted"}});
    // The decision stays, with nobody named; nothing about the question or Ravi is accepted.
    json dec = find(acc, "decided", "decision");
    CHECK_EQ(dec.size(), (size_t)1);
    if (dec.size() == 1) { CHECK(dec[0]["from_topic"].is_null()); CHECK(dec[0]["from_name"] == ""); }
    CHECK(find(acc, "asked", "question").empty());
    for (const auto &r : acc) {
        CHECK(r["memory_id"] != m.question);
        CHECK(r["from_topic"] != m.ravi);
        CHECK(r["to_topic"] != m.ravi);
    }
    // The memories themselves are untouched.
    CHECK(s.memory_item(m.question).is_object());
    json map = map_of(s, m.note);
    CHECK_EQ(count_links(map, "relation"), 0);
}

TEST(dismiss_rejects_and_is_not_offered_again) {
    Store s(":memory:");
    Meeting m = meeting(s);
    refresh_relations(s, m.note);
    CHECK(dismiss_relations(s, m.note));
    CHECK(relation_reviews(s).empty());
    CHECK(s.relations({{"note_id", m.note}, {"status", "accepted"}}).empty());
    CHECK(!s.relations({{"note_id", m.note}, {"status", "rejected"}}).empty());
    CHECK(!dismiss_relations(s, m.note));
    CHECK_EQ(refresh_relations(s, m.note), 0);
    CHECK_EQ(count_links(map_of(s, m.note), "relation"), 0);
    // A memory found later is offered on its own.
    memory(s, m.note, "decision", "Hire two people", "Sita", m.sita, "", 0, 0, 40);
    CHECK(refresh_relations(s, m.note) > 0);
    json rv = relation_reviews(s);
    CHECK_EQ(rv.size(), (size_t)1);
    if (rv.size() == 1) {
        CHECK_EQ(rv[0]["decisions"].size(), (size_t)1);
        CHECK(rv[0]["decisions"][0]["text"] == "Hire two people");
        CHECK(rv[0]["people"].empty());
    }
}

TEST(note_delete_removes_relations) {
    Store s(":memory:");
    Meeting m = meeting(s);
    refresh_relations(s, m.note);
    accept_relations(s, m.note, json::array());
    Meeting other = meeting(s);  // same people, another recording
    refresh_relations(s, other.note);
    CHECK(s.remove(m.note));
    CHECK(s.relations({{"note_id", m.note}}).empty());
    CHECK(!s.relations({{"note_id", other.note}}).empty());
    // A memory deleted on its own takes its relations with it.
    s.delete_memory_item(other.decision);
    CHECK(find(s.relations({{"note_id", other.note}}), "decided", "decision").empty());
}

TEST(merge_moves_relations) {
    Store s(":memory:");
    Meeting m = meeting(s);
    refresh_relations(s, m.note);
    accept_relations(s, m.note, json::array());
    int64_t raju = s.upsert_topic("person", "Raju");
    CHECK(s.merge_topics(m.ravi, raju));
    json dec = find(s.relations({{"note_id", m.note}, {"status", "accepted"}}), "decided", "decision");
    CHECK_EQ(dec.size(), (size_t)1);
    if (dec.size() == 1) { CHECK(dec[0]["from_topic"] == raju); CHECK(dec[0]["from_name"] == "Raju"); }
}

TEST(wipe_removes_relations) {
    Store s(":memory:");
    Meeting m = meeting(s);
    refresh_relations(s, m.note);
    CHECK(!s.relations(json::object()).empty());
    CHECK(s.wipe_assistant());
    CHECK(s.relations(json::object()).empty());
    CHECK(relation_reviews(s).empty());
}

TEST(old_database_gets_relations_table) {
    std::string p = tmp_db();
    unlink(p.c_str());
    int64_t note = 0, mem = 0;
    {
        Store s(p);
        CHECK(s.ok());
        note = s.create("Old", "en", nullptr, "t", 1);
        int64_t ravi = s.upsert_topic("person", "Ravi");
        s.link_topic(note, ravi);
        mem = memory(s, note, "decision", "Buy milk", "Ravi", ravi);
    }
    // A database from before relations: no table, no index.
    raw_exec(p, "DROP INDEX IF EXISTS relations_note; DROP TABLE relations;");
    {
        Store s(p);
        CHECK(s.ok());
        CHECK(s.memory_item(mem).is_object());  // nothing lost
        CHECK(refresh_relations(s, note) > 0);
        CHECK_EQ(relation_reviews(s).size(), (size_t)1);
        CHECK(accept_relations(s, note, json::array()));
    }
    {
        Store again(p);
        CHECK(again.ok());
        CHECK_EQ(find(again.relations({{"status", "accepted"}}), "decided", "decision").size(), (size_t)1);
    }
    drop_db(p);
}
