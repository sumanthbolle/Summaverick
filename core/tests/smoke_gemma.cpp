// On-device-model smoke test. Env: SVE_GEMMA, SVE_MMPROJ, SVE_TEST_WAV, SVE_FIXTURE.
// Arg: mode
//   transcribe  transcribe SVE_TEST_WAV
//   live        CPU, 4 threads: sve_transcribe_next_piece in a loop over SVE_TEST_WAV as if it were being
//               recorded (copied in 10 s steps into a WAV whose header says data size 0), then final
//   extract     doctor-visit fixture: extract, list pending suggestions, two Ask questions
//   api         assistant C API without a model (accept/reject, medicines, medicine Ask route)
#include "sve_core.h"
#include "sve_internal.h"
#include "sve_sections.h"  // api mode adds model-like suggestions straight to the store
#include "third_party/nlohmann/json.hpp"
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <cstring>
#include <fstream>
#include <string>

using nlohmann::json;

static int progress(const char *phase, int done, int total, void *) {
    fprintf(stderr, "  [%s %d/%d]\n", phase ? phase : "?", done, total);
    return 0;
}

static json take(char *s) {
    json j = s ? json::parse(s, nullptr, false) : json();
    sve_free_string(s);
    return j.is_discarded() ? json() : j;
}

static int fails = 0;
static void expect(bool ok, const char *what) {
    printf("%s %s\n", ok ? "EXPECT-OK  " : "EXPECT-MISS", what);
    if (!ok) ++fails;
}

static bool contains(const json &j, const char *s) { return j.dump().find(s) != std::string::npos; }

// "[t=SEC] text" lines -> pieces JSON.
static json read_fixture(const std::string &path) {
    json pieces = json::array();
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind("[t=", 0) != 0) continue;
        size_t close = line.find(']');
        if (close == std::string::npos) continue;
        int sec = atoi(line.c_str() + 3);
        std::string text = line.substr(close + 1);
        while (!text.empty() && text[0] == ' ') text.erase(0, 1);
        pieces.push_back({{"start_sec", sec}, {"text", text}});
    }
    return pieces;
}

static int64_t make_note(sve_ctx *ctx, const json &pieces, const char *title) {
    std::string transcript;
    for (const auto &p : pieces) transcript += p["text"].get<std::string>() + "\n";
    int64_t id = sve_note_create(ctx, title, "en", nullptr, transcript.c_str(), 140);
    sve_note_set_pieces(ctx, id, pieces.dump().c_str());
    return id;
}

static int run_extract(sve_ctx *ctx, const std::string &fixture) {
    json pieces = read_fixture(fixture);
    if (pieces.size() < 2) { fprintf(stderr, "fixture %s missing or too short\n", fixture.c_str()); return 1; }
    sve_profile_set(ctx, R"({"name":"Lakshmi","lang":"en","routine":{"dinner":"20:00"}})");
    int64_t note = make_note(ctx, pieces, "Doctor visit");
    int n = sve_extract(ctx, note, progress, nullptr);
    printf("sve_extract -> %d\n", n);
    json pending = take(sve_suggestions_json(ctx, "pending"));
    printf("--- pending suggestions\n%s\n", pending.dump(2).c_str());
    printf("--- topics\n%s\n", take(sve_topics_json(ctx, nullptr)).dump(2).c_str());

    bool amlo = false, task = false;
    for (const auto &s : pending) {
        if (s["kind"] == "reminder" && contains(s["payload"], "Amlodipine") && s["check_status"] == "ok" &&
            s["quote"].is_string() && s["quote"].get<std::string>().find("5 mg") != std::string::npos) amlo = true;
        if (s["kind"] == "task" && (contains(s, "blood test") || contains(s, "report"))) task = true;
    }
    expect(amlo, "reminder mentioning Amlodipine, check_status ok, quote contains 5 mg");
    expect(task, "task for the blood test report");

    json salt = take(sve_assistant_ask(ctx, "What did the doctor say about salt?", progress, nullptr));
    printf("--- ask salt\n%s\n", salt.dump(2).c_str());
    bool salt_src = false;
    if (salt.is_object())
        for (const auto &s : salt["sources"]) if (s["start_sec"] == 84) salt_src = true;
    expect(salt.is_object() && salt["not_found"] == false, "salt answer not_found false");
    expect(salt_src, "salt answer has a source at the salt piece (84 s)");

    json amq = take(sve_assistant_ask(ctx, "When do I take Amlodipine?", progress, nullptr));
    printf("--- ask Amlodipine\n%s\n", amq.dump(2).c_str());
    std::string line = pieces[1]["text"].get<std::string>();
    expect(amq.is_object() && amq["medicine"] == true, "Amlodipine answer medicine true");
    expect(amq.is_object() && amq["answer"].is_string() && amq["answer"].get<std::string>().find(line) != std::string::npos,
           "Amlodipine answer quotes the fixture line verbatim");

    // Accept the medicine reminder and the task; reject the rest.
    for (const auto &s : pending) {
        int64_t id = s["id"];
        if (s["kind"] == "reminder" || s["kind"] == "task") {
            int64_t row = sve_suggestion_accept(ctx, id, nullptr);
            printf("accept %s #%lld -> %lld\n", s["kind"].get<std::string>().c_str(), (long long)id, (long long)row);
        } else sve_suggestion_reject(ctx, id);
    }
    printf("--- reminders\n%s\n--- tasks\n%s\n", take(sve_reminders_json(ctx, 1)).dump(2).c_str(),
           take(sve_tasks_json(ctx, 1)).dump(2).c_str());
    expect(!take(sve_reminders_json(ctx, 1)).empty(), "accepted reminder stored");
    expect(take(sve_suggestions_json(ctx, "pending")).empty(), "nothing left pending");

    json topics = take(sve_topics_json(ctx, "topic"));
    if (!topics.empty()) {
        json sum = take(sve_topic_summary(ctx, topics[0]["id"], "en", progress, nullptr));
        printf("--- topic summary\n%s\n", sum.dump(2).c_str());
        expect(sum.is_object() && sum["summary"].is_string(), "topic summary JSON");
    }
    sve_note_set_output(ctx, note, SVE_OUT_SHORT_SUMMARY, "Doctor visit: BP high, Amlodipine started, blood test report on Saturday.");
    json ins = take(sve_insight_generate(ctx, note));
    printf("--- insight\n%s\n", ins.dump(2).c_str());
    json again = take(sve_insight_generate(ctx, note));
    expect(again.is_null(), "second insight the same day is null");
    return 0;
}


static json pending_kind(sve_ctx *ctx, const char *kind) {
    json out = json::array();
    for (const auto &s : take(sve_suggestions_json(ctx, "pending"))) if (s["kind"] == kind) out.push_back(s);
    return out;
}

static int64_t add_sugg(sve_ctx *ctx, const json &s) { return ctx->store->add_suggestion(s); }

static json active_named(sve_ctx *ctx, const char *name) {
    json out = json::array();
    for (const auto &r : take(sve_reminders_json(ctx, 1)))
        if (r["text"].get<std::string>().find(name) != std::string::npos) out.push_back(r);
    return out;
}



static json med_named(sve_ctx *ctx, const char *name) {
    for (const auto &m : take(sve_medicines_json(ctx))) if (m["name"] == name) return m;
    return nullptr;
}

static bool has_field(const json &s, const char *f) {
    for (const auto &x : s["check_fields"]) if (x == f) return true;
    return false;
}

// Fix round 4: profile add/update cards go through the same stale guard as reminders; only a dose
// edit skips it; accepting a new dose supersedes the sibling cards.
static void run_api_fixes4(sve_ctx *ctx, int64_t note) {
    for (const auto &s : take(sve_suggestions_json(ctx, "pending"))) sve_suggestion_reject(ctx, s["id"]);
    auto pcard = [&](const char *change, const char *name, const char *dose, const char *status, json fields) {
        std::string nd = std::string(name) + " " + dose;
        return json{{"kind", "profile"}, {"note_id", note}, {"start_sec", 0}, {"quote", "Take " + nd + " after dinner"},
                    {"check_status", status}, {"check_fields", fields},
                    {"payload", {{"change", change}, {"text", "Add medicine: " + nd}, {"when_text", "after dinner"},
                                 {"medicine", {{"name", name}, {"dose", dose}, {"when_text", "after dinner"}}}}}};
    };
    auto rcard = [&](const char *name, const char *dose, const char *status, json fields) {
        std::string nd = std::string(name) + " " + dose;
        return json{{"kind", "reminder"}, {"note_id", note}, {"start_sec", 0}, {"quote", "Take " + nd + " after dinner"},
                    {"check_status", status}, {"check_fields", fields},
                    {"payload", {{"text", "Take " + nd}, {"when_text", "after dinner"}, {"times", {"21:30"}}, {"repeat", "daily"},
                                 {"medicine", {{"name", name}, {"dose", dose}, {"when_text", "after dinner"}}}}}};
    };

    // 1 (add): recording card "add Telmikind 25 mg"; the person types 50 mg -> card superseded, 50 mg kept.
    int64_t add_card = add_sugg(ctx, pcard("add_medicine", "Telmikind", "25 mg", "ok", json::array()));
    int64_t tid = sve_medicine_save(ctx, R"({"name":"Telmikind","dose":"50 mg","when":"after dinner"})");
    expect(tid > 0 && ctx->store->suggestion(add_card)["status"] == "superseded", "typing the medicine supersedes the profile add card");
    expect(sve_suggestion_accept(ctx, add_card, nullptr) < 0, "superseded profile add card cannot be accepted");
    json own = pending_kind(ctx, "reminder");
    expect(own.size() == 1 && sve_suggestion_accept(ctx, own[0]["id"], nullptr) > 0, "person's own Telmikind card accepted");
    expect(med_named(ctx, "Telmikind")["dose"] == "50 mg" && active_named(ctx, "Telmikind").size() == 1 &&
           active_named(ctx, "Telmikind 50 mg").size() == 1, "add: 50 mg kept, exactly one active 50 mg reminder");

    // 1 (update): saved Olmy 10 mg; recording card "update Olmy 25 mg"; the person edits to 50 mg.
    int64_t oid = sve_medicine_save(ctx, R"({"name":"Olmy","dose":"10 mg","when":"after dinner"})");
    for (const auto &s : pending_kind(ctx, "reminder")) sve_suggestion_reject(ctx, s["id"]);
    int64_t upd_card = add_sugg(ctx, pcard("update_medicine", "Olmy", "25 mg", "ok", json::array()));
    sve_medicine_save(ctx, (R"({"id":)" + std::to_string(oid) + R"(,"dose":"50 mg"})").c_str());
    expect(ctx->store->suggestion(upd_card)["status"] == "superseded", "editing the dose supersedes the profile update card");
    expect(sve_suggestion_accept(ctx, upd_card, nullptr) < 0, "superseded profile update card cannot be accepted");
    own = pending_kind(ctx, "reminder");
    expect(own.size() == 1 && sve_suggestion_accept(ctx, own[0]["id"], nullptr) > 0, "person's own Olmy card accepted");
    expect(med_named(ctx, "Olmy")["dose"] == "50 mg" && active_named(ctx, "Olmy").size() == 1 &&
           active_named(ctx, "Olmy 50 mg").size() == 1, "update: 50 mg kept, exactly one active 50 mg reminder");

    // 1 (c): a crafted stale profile card accepted unedited -> -2, flagged, pending, nothing applied.
    int64_t stale_p = add_sugg(ctx, pcard("add_medicine", "telmikind", "25 mg", "ok", json::array()));
    expect(sve_suggestion_accept(ctx, stale_p, nullptr) == -2, "stale profile card returns -2");
    json sp = ctx->store->suggestion(stale_p);
    expect(sp["status"] == "pending" && sp["check_status"] == "check" && has_field(sp, "dose"), "stale profile card re-flagged on dose, still pending");
    expect(med_named(ctx, "Telmikind")["dose"] == "50 mg" && active_named(ctx, "Telmikind 50 mg").size() == 1, "stale profile card applied nothing");
    sve_suggestion_reject(ctx, stale_p);
    // 2: an edit to the time alone does not count as looking at the dose.
    int64_t stale_r = add_sugg(ctx, rcard("Telmikind", "25 mg", "ok", json::array()));
    expect(sve_suggestion_accept(ctx, stale_r, R"({"times":["21:00"]})") == -2, "time-only edit on a stale reminder card returns -2");
    int64_t stale_p2 = add_sugg(ctx, pcard("update_medicine", "Telmikind", "25 mg", "ok", json::array()));
    expect(sve_suggestion_accept(ctx, stale_p2, R"({"when_text":"after lunch"})") == -2, "time-only edit on a stale profile card returns -2");
    expect(med_named(ctx, "Telmikind")["dose"] == "50 mg", "time-only edits applied nothing");
    // A dose edit does count: it applies the edited dose and keeps the saved name.
    int64_t edit_r = add_sugg(ctx, rcard("telmikind", "25 mg", "ok", json::array()));
    expect(sve_suggestion_accept(ctx, edit_r, R"({"medicine":{"name":"telmikind","dose":"40 mg","when_text":"after dinner"}})") > 0,
           "dose edit on a stale card applies");
    json tm = med_named(ctx, "Telmikind");
    expect(tm.is_object() && tm["id"] == tid && tm["dose"] == "40 mg" && med_named(ctx, "telmikind").is_null(),
           "edited dose applied, saved name kept");
    expect(active_named(ctx, "Telmikind").size() == 1 && active_named(ctx, "Telmikind 40 mg").size() == 1, "one active reminder, 40 mg");

    // 3: siblings. Two pending cards for one medicine; accepting one with a new dose supersedes the other.
    for (const auto &s : take(sve_suggestions_json(ctx, "pending"))) sve_suggestion_reject(ctx, s["id"]);
    int64_t sib_r = add_sugg(ctx, rcard("Telmikind", "60 mg", "check", json::array({"dose"})));
    int64_t sib_p = add_sugg(ctx, pcard("update_medicine", "Telmikind", "60 mg", "check", json::array({"dose"})));
    expect(sve_suggestion_accept(ctx, sib_r, nullptr) > 0, "flagged reminder card with a new dose accepted");
    expect(ctx->store->suggestion(sib_r)["status"] == "accepted" && ctx->store->suggestion(sib_p)["status"] == "superseded",
           "accepting the new dose supersedes the sibling profile card");
    expect(med_named(ctx, "Telmikind")["dose"] == "60 mg" && active_named(ctx, "Telmikind").size() == 1 &&
           active_named(ctx, "Telmikind 60 mg").size() == 1, "60 mg saved, one active reminder");
    // The flagged profile card, accepted, updates the dose, keeps the name, and supersedes a sibling reminder card.
    int64_t sib_r2 = add_sugg(ctx, rcard("Telmikind", "80 mg", "check", json::array({"dose"})));
    int64_t sib_p2 = add_sugg(ctx, pcard("update_medicine", "telmikind", "80 mg", "check", json::array({"dose"})));
    expect(sve_suggestion_accept(ctx, sib_p2, nullptr) == tid, "flagged profile card with a new dose accepted");
    expect(ctx->store->suggestion(sib_r2)["status"] == "superseded", "accepting the profile card supersedes the sibling reminder card");
    tm = med_named(ctx, "Telmikind");
    expect(tm.is_object() && tm["dose"] == "80 mg" && med_named(ctx, "telmikind").is_null(), "profile update keeps the saved name");
    json pr = pending_kind(ctx, "reminder");
    expect(active_named(ctx, "Telmikind").empty() && pr.size() == 1 && contains(pr[0]["payload"], "80 mg"),
           "old reminder off, new 80 mg reminder proposed");

    // Round 5: deleting the medicine supersedes the recording's pending cards for it (medicine_id NULL).
    if (!pr.empty()) sve_suggestion_accept(ctx, pr[0]["id"], nullptr);
    int64_t rec_r = add_sugg(ctx, rcard("Telmikind", "80 mg", "ok", json::array()));
    int64_t rec_p = add_sugg(ctx, pcard("add_medicine", "telmikind", "80 mg", "ok", json::array()));
    expect(active_named(ctx, "Telmikind 80 mg").size() == 1 && ctx->store->suggestion(rec_r)["medicine_id"].is_null(),
           "setup: active 80 mg reminder and unlinked recording cards");
    expect(sve_medicine_delete(ctx, tid) == 0, "medicine deleted");
    expect(ctx->store->suggestion(rec_r)["status"] == "superseded" && ctx->store->suggestion(rec_p)["status"] == "superseded",
           "delete supersedes the recording's reminder and profile cards");
    expect(sve_suggestion_accept(ctx, rec_r, nullptr) < 0 && sve_suggestion_accept(ctx, rec_p, nullptr) < 0,
           "old cards cannot be accepted after the delete");
    expect(med_named(ctx, "Telmikind").is_null() && active_named(ctx, "Telmikind").empty(), "no medicine row, no active reminder");
}

// Fix round 3: a stale "ok" card never overwrites a dose the person entered later.
static void run_api_fixes3(sve_ctx *ctx, int64_t note) {
    for (const auto &s : take(sve_suggestions_json(ctx, "pending"))) sve_suggestion_reject(ctx, s["id"]);
    auto card = [&](const char *dose) {
        return json{{"kind", "reminder"}, {"note_id", note}, {"start_sec", 0}, {"quote", std::string("Take Cardace ") + dose + " after dinner"},
                    {"check_status", "ok"}, {"check_fields", json::array()},
                    {"payload", {{"text", std::string("Take Cardace ") + dose}, {"when_text", "after dinner"}, {"times", {"21:30"}},
                                 {"repeat", "daily"}, {"medicine", {{"name", "cardace"}, {"dose", dose}, {"when_text", "after dinner"}}}}}};
    };
    // Recording proposes 25 mg; the person then types 50 mg; the old card is superseded and cannot apply.
    int64_t old_card = add_sugg(ctx, card("25 mg"));
    int64_t cid = sve_medicine_save(ctx, R"({"name":"Cardace","dose":"50 mg","when":"after dinner"})");
    expect(ctx->store->suggestion(old_card)["status"] == "superseded", "typing the medicine supersedes the recording's card");
    expect(sve_suggestion_accept(ctx, old_card, nullptr) < 0, "superseded card cannot be accepted");
    json own = pending_kind(ctx, "reminder");
    expect(own.size() == 1 && sve_suggestion_accept(ctx, own[0]["id"], nullptr) > 0, "person's own card accepted");
    expect(med_named(ctx, "Cardace")["dose"] == "50 mg" && active_named(ctx, "Cardace 50 mg").size() == 1, "50 mg kept and active");

    // A stale pending "ok" card accepted unedited -> -2, re-flagged, still pending, nothing changed.
    int64_t stale = add_sugg(ctx, card("25 mg"));
    expect(sve_suggestion_accept(ctx, stale, nullptr) == -2, "stale card returns -2");
    json st = ctx->store->suggestion(stale);
    bool dose_flag = false;
    for (const auto &f : st["check_fields"]) dose_flag = dose_flag || f == "dose";
    expect(st["status"] == "pending" && st["check_status"] == "check" && dose_flag, "stale card re-flagged on dose and kept pending");
    expect(med_named(ctx, "Cardace")["dose"] == "50 mg" && active_named(ctx, "Cardace 50 mg").size() == 1, "nothing applied");
    // After the warning, Yes applies; the saved name is kept.
    expect(sve_suggestion_accept(ctx, stale, nullptr) > 0, "flagged card accepted on second Yes");
    json cm = med_named(ctx, "Cardace");
    expect(cm.is_object() && cm["id"] == cid && cm["dose"] == "25 mg", "accepted dose applied, saved name kept");
    expect(med_named(ctx, "cardace").is_null(), "no rename to the payload's spelling");
    run_api_fixes4(ctx, note);
}

// Fix round 2: accepted dose updates the saved medicine; editing a dose turns old reminders off;
// stop supersedes pending cards; Ask quotes medicine pieces first and medicine facts.
static void run_api_fixes2(sve_ctx *ctx, int64_t note) {
    for (const auto &s : take(sve_suggestions_json(ctx, "pending"))) sve_suggestion_reject(ctx, s["id"]);

    // 5: saved Losar 25 mg; a recording says 50 mg; Yes updates the saved medicine.
    int64_t los = sve_medicine_save(ctx, R"({"name":"Losar","dose":"25 mg","when":"after breakfast"})");
    for (const auto &s : pending_kind(ctx, "reminder")) sve_suggestion_reject(ctx, s["id"]);
    json rem = {{"kind", "reminder"}, {"note_id", note}, {"start_sec", 0}, {"quote", "Take Losar 50 mg after dinner"}, {"check_status", "check"},
                {"check_fields", {"dose"}},
                {"payload", {{"text", "Take Losar 50 mg"}, {"when_text", "after dinner"}, {"times", {"21:30"}}, {"repeat", "daily"},
                             {"medicine", {{"name", "Losar"}, {"dose", "50 mg"}, {"when_text", "after dinner"}}}}}};
    expect(sve_suggestion_accept(ctx, add_sugg(ctx, rem), nullptr) > 0, "accept reminder with a new dose");
    json lm;
    for (const auto &m : take(sve_medicines_json(ctx))) if (m["id"] == los) lm = m;
    expect(lm.is_object() && lm["dose"] == "50 mg" && lm["when_text"] == "after dinner" && lm["times"] == json::array({"21:30"}),
           "saved medicine takes the accepted dose and time");
    expect(active_named(ctx, "Losar").size() == 1 && active_named(ctx, "Losar 50 mg").size() == 1, "one active Losar reminder");

    // 6: editing the dose turns the old reminder off at once; rejecting the new card leaves none.
    sve_medicine_save(ctx, (R"({"id":)" + std::to_string(los) + R"(,"dose":"100 mg"})").c_str());
    expect(active_named(ctx, "Losar").empty(), "dose edit turns off the old reminder immediately");
    json pr = pending_kind(ctx, "reminder");
    expect(pr.size() == 1 && contains(pr[0]["payload"], "100 mg"), "dose edit proposes a new reminder");
    if (!pr.empty()) sve_suggestion_reject(ctx, pr[0]["id"]);
    expect(active_named(ctx, "Losar").empty(), "rejecting the new card leaves no active reminder");

    // 7: stop supersedes pending reminder cards for the medicine (by id and by name).
    sve_medicine_save(ctx, (R"({"id":)" + std::to_string(los) + R"(,"dose":"25 mg"})").c_str());  // pending card by id
    json byname = rem;
    byname["payload"]["medicine"]["dose"] = "25 mg";
    int64_t bn = add_sugg(ctx, byname);  // pending card by name only
    json stop = {{"kind", "profile"}, {"note_id", note}, {"quote", "Stop Losar"}, {"check_status", "ok"},
                 {"payload", {{"change", "stop_medicine"}, {"text", "Stop medicine: Losar"}, {"medicine", {{"name", "Losar"}}}}}};
    sve_suggestion_accept(ctx, add_sugg(ctx, stop), nullptr);
    expect(pending_kind(ctx, "reminder").empty() && ctx->store->suggestion(bn)["status"] == "superseded",
           "stop supersedes pending reminder cards for it");

    // 8: medicine pieces first, at most 3; a medicine fact also triggers the verbatim route.
    json night = json::parse(R"([{"start_sec":0,"text":"Walk for ten minutes at night."},{"start_sec":28,"text":"Read a book at night."},
        {"start_sec":56,"text":"Sleep early at night."},{"start_sec":84,"text":"Drink warm milk at night."},{"start_sec":112,"text":"Take Nitrest 10 mg at night."}])");
    make_note(ctx, night, "Evening plan");
    json a = take(sve_assistant_ask(ctx, "Plans for night time?", nullptr, nullptr));
    printf("--- ask night\n%s\n", a.dump(2).c_str());
    expect(a.is_object() && a["medicine"] == true && a["sources"].size() <= 3 && contains(a["answer"], "Take Nitrest 10 mg at night.") &&
           a["sources"].size() > 0, "medicine piece quoted first, at most 3");
    ctx->store->add_memory("fact", "Take the iron tablet with orange juice", std::nullopt);
    json f = take(sve_assistant_ask(ctx, "Anything about orange juice?", nullptr, nullptr));
    printf("--- ask fact\n%s\n", f.dump(2).c_str());
    expect(f.is_object() && f["medicine"] == true && contains(f["answer"], "Take the iron tablet with orange juice"),
           "medicine fact quoted verbatim");
    run_api_fixes3(ctx, note);
}

// Fix round 1: recording-born reminders link to the medicine; stop/update/delete turn them off;
// Ask never generates over medicine material; routine changes; times check; transactions.
static void run_api_fixes(sve_ctx *ctx) {
    sve_profile_set(ctx, R"({"name":"Lakshmi","lang":"en","routine":{"breakfast":"08:00","dinner":"20:00"}})");
    json pieces = json::parse(R"([{"start_sec":0,"text":"Take Ramipril 5 mg once a day after dinner."},
        {"start_sec":28,"text":"Take Zolfra 20 mg at night."},{"start_sec":56,"text":"Vitamin B12 1000 mcg once a week."}])");
    int64_t note = make_note(ctx, pieces, "Clinic");
    json med = {{"name", "Ramipril"}, {"dose", "5 mg"}, {"when_text", "after dinner"}, {"frequency", "once a day"}, {"duration", ""}};
    json rem = {{"kind", "reminder"}, {"note_id", note}, {"start_sec", 0}, {"quote", "Take Ramipril 5 mg once a day after dinner."},
                {"check_status", "ok"}, {"check_fields", json::array()},
                {"payload", {{"text", "Take Ramipril 5 mg"}, {"when_text", "after dinner"}, {"times", {"20:00"}}, {"repeat", "daily"},
                             {"start_date", nullptr}, {"until_date", nullptr}, {"medicine", med}}}};

    // Accept a recording-born reminder -> medicine row created and linked.
    int64_t rid = sve_suggestion_accept(ctx, add_sugg(ctx, rem), nullptr);
    json meds = take(sve_medicines_json(ctx));
    int64_t mid = 0;
    for (const auto &m : meds) if (m["name"] == "Ramipril") mid = m["id"];
    json act = active_named(ctx, "Ramipril");
    expect(rid > 0 && mid > 0 && act.size() == 1 && act[0]["medicine_id"] == mid && act[0]["text"] == "Take Ramipril 5 mg",
           "accepted recording reminder creates and links the medicine");

    // Update -> old reminder off, new reminder proposed. (Round 4: extraction flags a card whose dose
    // differs from the saved 5 mg with "dose"; an unflagged one would be stale and return -2.)
    json upd = {{"kind", "profile"}, {"note_id", note}, {"start_sec", 0}, {"quote", "Take Ramipril 10 mg"}, {"check_status", "check"},
                {"check_fields", {"dose"}},
                {"payload", {{"change", "update_medicine"}, {"text", "Change medicine: Ramipril 10 mg"},
                             {"medicine", {{"name", "Ramipril"}, {"dose", "10 mg"}, {"when_text", "after dinner"}}}}}};
    expect(sve_suggestion_accept(ctx, add_sugg(ctx, upd), nullptr) == mid, "update_medicine accepted");
    json pr = pending_kind(ctx, "reminder");
    expect(active_named(ctx, "Ramipril").empty() && pr.size() == 1 && contains(pr[0]["payload"], "10 mg") && pr[0]["medicine_id"] == mid,
           "update turns off the old reminder and proposes a new one");
    expect(sve_suggestion_accept(ctx, pr[0]["id"], nullptr) > 0 && active_named(ctx, "Ramipril 10 mg").size() == 1, "new reminder accepted");

    // Stop -> no active reminder, medicine gone.
    json stop = {{"kind", "profile"}, {"note_id", note}, {"start_sec", 0}, {"quote", "Stop Ramipril"}, {"check_status", "ok"},
                 {"payload", {{"change", "stop_medicine"}, {"text", "Stop medicine: Ramipril"}, {"medicine", {{"name", "Ramipril"}}}}}};
    expect(sve_suggestion_accept(ctx, add_sugg(ctx, stop), nullptr) == mid, "stop_medicine returns the deleted id");
    expect(active_named(ctx, "Ramipril").empty() && take(sve_medicines_json(ctx)).empty(), "stop leaves no active reminder for it");

    // Stop when no medicine row exists still turns off an unlinked reminder that names it.
    ctx->store->add_reminder({{"text", "Take Telma 40 mg"}, {"times", {"08:00"}}, {"repeat", "daily"}});
    json stop2 = stop;
    stop2["payload"]["medicine"]["name"] = "Telma";
    expect(sve_suggestion_accept(ctx, add_sugg(ctx, stop2), nullptr) == 0 && active_named(ctx, "Telma").empty(),
           "stop without a medicine row turns off reminders naming it");

    // sve_medicine_delete turns off unlinked reminders naming it.
    ctx->store->add_reminder({{"text", "Take Metformin 500 mg"}, {"times", {"08:00"}}, {"repeat", "daily"}});
    int64_t met = sve_medicine_save(ctx, R"({"name":"Metformin","dose":"500 mg","when":"after breakfast"})");
    sve_medicine_delete(ctx, met);
    expect(active_named(ctx, "Metformin").empty(), "medicine delete turns off reminders naming it");

    // Ask over material that mentions an unknown medicine: verbatim quotes, no generation (no model loaded).
    json a = take(sve_assistant_ask(ctx, "What should I do at night?", nullptr, nullptr));
    printf("--- ask unknown medicine\n%s\n", a.dump(2).c_str());
    expect(a.is_object() && a["medicine"] == true && contains(a["answer"], "“Take Zolfra 20 mg at night.”") &&
           !a["sources"].empty(), "unknown medicine in pieces -> verbatim quotes");

    // Vitamin D3 must not quote the Vitamin B12 piece.
    sve_medicine_save(ctx, R"({"name":"Vitamin D3","dose":"60000 IU","when":"after breakfast","repeat":"weekly"})");
    json d3 = take(sve_assistant_ask(ctx, "When do I take Vitamin D3?", nullptr, nullptr));
    expect(d3.is_object() && !contains(d3["answer"], "B12") && contains(d3["answer"], "Vitamin D3 60000 IU"), "D3 answer does not quote B12");

    // Word-only medicine question with nothing matching: the list is shown, not as an answer.
    json w = take(sve_assistant_ask(ctx, "Which pills for sugar?", nullptr, nullptr));
    printf("--- ask word-only\n%s\n", w.dump(2).c_str());
    expect(w.is_object() && w["not_found"] == true && contains(w["answer"], "Your saved medicines:") &&
           contains(w["answer"], "Your recordings don't name a medicine for this question."), "word-only question -> saved list, not an answer");

    // Routine change: accept merges into the profile; later times use it.
    json rt = {{"kind", "profile"}, {"note_id", note}, {"quote", "dinner at 9:30"}, {"check_status", "ok"},
               {"payload", {{"change", "routine"}, {"text", "Dinner at 21:30"}, {"routine", {{"dinner", "21:30"}}}}}};
    expect(sve_suggestion_accept(ctx, add_sugg(ctx, rt), nullptr) == 0, "routine change accepted");
    json prof = take(sve_profile_get(ctx));
    expect(prof["routine"]["dinner"] == "21:30" && prof["routine"]["breakfast"] == "08:00", "routine merged into profile");
    int64_t amid = sve_medicine_save(ctx, R"({"name":"Atorva","dose":"10 mg","when":"after dinner"})");
    json am;
    for (const auto &m : take(sve_medicines_json(ctx))) if (m["id"] == amid) am = m;
    expect(am.is_object() && am["times"] == json::array({"21:30"}), "later resolve uses the new dinner time");

    // No time from the routine -> reminder suggestion is "check" on times.
    int64_t pr_id = sve_medicine_save(ctx, R"({"name":"Dolo","dose":"650 mg","when":"when needed"})");
    bool flagged = false;
    for (const auto &s : pending_kind(ctx, "reminder"))
        if (s["medicine_id"] == pr_id) flagged = s["check_status"] == "check" && s["check_fields"] == json::array({"times"});
    expect(flagged, "medicine without a time -> times check");

    // A failing accept changes nothing (transaction).
    json bad = rt;
    bad["payload"]["routine"] = json::object();
    int64_t bid = add_sugg(ctx, bad);
    expect(sve_suggestion_accept(ctx, bid, nullptr) < 0 && ctx->store->suggestion(bid)["status"] == "pending",
           "failed accept leaves the suggestion pending");
    ctx->store->set_suggestion_status(bid, "rejected");
    run_api_fixes2(ctx, note);
}

// Ask's "Make reminder" / "Make to-do": the person's own rows, created directly with the source moment.
static void run_api_direct(sve_ctx *ctx) {
    json pieces = json::parse(R"([{"start_sec":0,"text":"Walk every evening."}])");
    int64_t note = make_note(ctx, pieces, "Walk advice");
    size_t before = take(sve_reminders_json(ctx, 0)).size();
    std::string rj = R"({"text":"Walk every evening","times":["18:30"],"repeat":"daily","note_id":)" + std::to_string(note) + R"(,"start_sec":0})";
    int64_t rid = sve_reminder_create(ctx, rj.c_str());
    json rems = take(sve_reminders_json(ctx, 0));
    expect(rid > 0 && rems.size() == before + 1 && rems.back()["id"] == rid && rems.back()["note_id"] == note &&
           rems.back()["start_sec"] == 0 && rems.back()["times"] == json::array({"18:30"}) && rems.back()["repeat"] == "daily" &&
           rems.back()["medicine_id"].is_null(), "sve_reminder_create saves the row with its source");
    expect(sve_reminder_create(ctx, R"({"text":"x","times":[]})") < 0, "reminder create needs a time");
    expect(sve_reminder_create(ctx, R"({"text":"","times":["09:00"]})") < 0, "reminder create needs text");
    expect(sve_reminder_create(ctx, R"({"text":"x","times":["9:00 pm"]})") < 0, "reminder create refuses a bad time");
    expect(sve_reminder_create(ctx, "{bad") < 0 && sve_reminder_create(ctx, nullptr) < 0 && sve_reminder_create(nullptr, "{}") < 0,
           "reminder create refuses bad JSON / null");
    expect(take(sve_reminders_json(ctx, 0)).size() == before + 1, "refused reminders save nothing");
    // A source note deleted after the answer: the row is kept without the link.
    int64_t gone = make_note(ctx, pieces, "Deleted later");
    sve_note_delete(ctx, gone);
    int64_t r2 = sve_reminder_create(ctx, (R"({"text":"y","times":["07:00"],"note_id":)" + std::to_string(gone) + R"(,"start_sec":5})").c_str());
    json last = take(sve_reminders_json(ctx, 0)).back();
    expect(r2 > 0 && last["id"] == r2 && last["note_id"].is_null() && last["start_sec"].is_null(), "deleted source note dropped from the reminder");

    size_t tbefore = take(sve_tasks_json(ctx, 0)).size();
    int64_t tid = sve_task_create(ctx, (R"({"text":"Buy walking shoes","note_id":)" + std::to_string(note) + R"(,"start_sec":0})").c_str());
    json tasks = take(sve_tasks_json(ctx, 0));
    expect(tid > 0 && tasks.size() == tbefore + 1 && tasks.back()["id"] == tid && tasks.back()["note_id"] == note &&
           tasks.back()["due_date"].is_null() && tasks.back()["who"].is_null(), "sve_task_create saves the row with its source");
    expect(sve_task_create(ctx, R"({"text":"  "})") < 0 && sve_task_create(ctx, R"({"text":"x","due_date":"soon"})") < 0 &&
           sve_task_create(ctx, "[]") < 0, "task create refuses empty text / bad date / non-object");
    sve_reminder_set_active(ctx, rid, 0);
    sve_reminder_set_active(ctx, r2, 0);
    sve_task_set_done(ctx, tid, 1);

    // Medicine reminders from Ask are linked by id, so edited wording does not lose them.
    auto rem = [&](int64_t id) { for (const auto &r : take(sve_reminders_json(ctx, 0))) if (r["id"] == id) return r; return json(); };
    int64_t mid = sve_medicine_save(ctx, R"({"name":"Cilacar","dose":"10 mg","when":"after dinner"})");
    int64_t other = sve_medicine_save(ctx, R"({"name":"Glycomet","dose":"500 mg","when":"after breakfast"})");
    for (const auto &s : take(sve_suggestions_json(ctx, "pending"))) sve_suggestion_reject(ctx, s["id"]);
    std::string mj = R"({"text":"my night BP tablet","times":["21:00"],"repeat":"daily","source_text":"Take Cilacar 10 mg after dinner.","medicine_id":)" + std::to_string(mid) + "}";
    int64_t e1 = sve_reminder_create(ctx, mj.c_str());
    expect(e1 > 0 && rem(e1)["medicine_id"] == mid, "explicit medicine_id kept on an edited-wording reminder");
    sve_medicine_save(ctx, (R"({"id":)" + std::to_string(mid) + R"(,"dose":"20 mg"})").c_str());
    expect(rem(e1)["active"] == false, "dose change turns off the linked Ask reminder");
    int64_t e2 = sve_reminder_create(ctx, mj.c_str());
    json stop = {{"kind", "profile"}, {"quote", "Stop Cilacar"}, {"check_status", "ok"},
                 {"payload", {{"change", "stop_medicine"}, {"text", "Stop medicine: Cilacar"}, {"medicine", {{"name", "Cilacar"}}}}}};
    expect(sve_suggestion_accept(ctx, add_sugg(ctx, stop), nullptr) == mid && rem(e2).is_null(),
           "stopping the medicine removes the linked Ask reminder");
    int64_t mid2 = sve_medicine_save(ctx, R"({"name":"Cilacar","dose":"10 mg","when":"after dinner"})");
    int64_t e3 = sve_reminder_create(ctx, (R"({"text":"Cilacar tablet","times":["21:00"],"repeat":"daily","medicine_id":)" + std::to_string(mid2) + "}").c_str());
    sve_medicine_delete(ctx, mid2);
    expect(e3 > 0 && rem(e3).is_null(), "deleting the medicine removes the linked Ask reminder");
    int64_t auto1 = sve_reminder_create(ctx, R"({"text":"Take Glycomet 500 mg after breakfast","times":["08:30"],"repeat":"daily"})");
    expect(rem(auto1)["medicine_id"] == other, "text naming exactly one medicine links it");
    sve_medicine_save(ctx, R"({"name":"Cilacar","dose":"10 mg","when":"after dinner"})");
    int64_t two = sve_reminder_create(ctx, R"({"text":"Glycomet and Cilacar","times":["08:30"],"repeat":"daily"})");
    expect(two > 0 && rem(two)["medicine_id"].is_null(), "text naming two medicines stays unlinked");
    expect(sve_reminder_create(ctx, R"({"text":"x","times":["08:30"],"medicine_id":99999})") < 0, "unknown medicine_id rejected");
    int64_t cil = take(sve_medicines_named(ctx, "Cilacar"))[0].get<int64_t>();
    auto withId = [&](const char *text, const char *source, int64_t id) {
        json j = {{"text", text}, {"times", {"21:00"}}, {"repeat", "daily"}, {"medicine_id", id}};
        if (source) j["source_text"] = source;
        return sve_reminder_create(ctx, j.dump().c_str());
    };
    expect(withId("Take Glycomet after breakfast", nullptr, cil) < 0, "explicit id with text naming another medicine rejected");
    expect(withId("my tablets", "Take Cilacar at night and Glycomet in the morning.", cil) < 0, "explicit id with a two-medicine source rejected");
    expect(withId("my tablet", nullptr, cil) < 0, "explicit id with words naming no medicine rejected");
    int64_t okid = withId("my night tablet", "Take Cilacar 10 mg after dinner.", cil);
    expect(okid > 0 && rem(okid)["medicine_id"] == cil, "explicit id matching the one named medicine accepted");
    expect(withId("x", nullptr, cil) < 0 && sve_reminder_create(ctx, (R"({"text":"Cilacar","times":["21:00"],"source_text":5,"medicine_id":)" + std::to_string(cil) + "}").c_str()) < 0,
           "non-string source_text rejected");
    sve_medicine_save(ctx, R"({"name":"Telma 40","dose":"1 tablet"})");
    json tn = take(sve_medicines_named(ctx, "Take Telma40 daily"));
    expect(tn.is_array() && tn.size() == 1, "sve_medicines_named matches Telma40 to Telma 40");
    expect(take(sve_medicines_named(ctx, "walk every evening")) == json::array() && sve_medicines_named(ctx, nullptr) == nullptr,
           "sve_medicines_named: none -> [], NULL text -> NULL");
    for (const auto &s : take(sve_suggestions_json(ctx, "pending"))) sve_suggestion_reject(ctx, s["id"]);
}

// Times the person gave are kept as given; only a missing "times" key is resolved; Yes never fills them.
static void run_api_times(sve_ctx *ctx) {
    sve_profile_set(ctx, R"({"name":"Lakshmi","lang":"en","routine":{"breakfast":"08:00","dinner":"20:00","bed":"22:00"}})");
    for (const auto &s : take(sve_suggestions_json(ctx, "pending"))) sve_suggestion_reject(ctx, s["id"]);
    auto med = [&](int64_t id) { for (const auto &m : take(sve_medicines_json(ctx))) if (m["id"] == id) return m; return json(); };
    auto card = [&](int64_t id) { for (const auto &s : take(sve_suggestions_json(ctx, "pending"))) if (s["medicine_id"] == id) return s; return json(); };
    int64_t a = sve_medicine_save(ctx, R"({"id":0,"name":"Ecosprin","dose":"75 mg","when":"after dinner","times":[],"repeat":"daily"})");
    json ca = card(a);
    expect(a > 0 && med(a)["times"] == json::array() && ca["payload"]["times"] == json::array() &&
           ca["check_status"] == "check" && contains(ca["check_fields"], "times"),
           "times [] kept empty: medicine and card times empty, card flagged check on times");
    expect(sve_suggestion_accept(ctx, ca["id"], nullptr) == -1 && card(a)["status"] == "pending",
           "accept of a reminder with empty times -> -1, card stays pending");
    expect(sve_suggestion_accept(ctx, ca["id"], R"({"times":["21:00"]})") > 0, "accept after adding a time works");
    int64_t b = sve_medicine_save(ctx, R"({"id":0,"name":"Pantop","dose":"40 mg","when":"before breakfast","repeat":"daily"})");
    expect(med(b)["times"] == json::array({"08:00"}) && card(b)["check_status"] == "ok", "no times key -> resolved from the routine");
    int64_t b2 = sve_medicine_save(ctx, (R"({"id":)" + std::to_string(b) + R"(,"times":[]})").c_str());
    expect(b2 == b && med(b)["times"] == json::array(), "edit with times [] removes the times");
    expect(take(sve_resolve_times(ctx, "after tiffin")) == json::array({"08:00"}), "sve_resolve_times: after tiffin -> breakfast");
    expect(take(sve_resolve_times(ctx, "నిద్ర ముందు")) == json::array({"22:00"}), "sve_resolve_times: నిద్ర -> bed");
    expect(take(sve_resolve_times(ctx, "when needed")) == json::array() && sve_resolve_times(ctx, nullptr) == nullptr,
           "sve_resolve_times: no keyword -> [], NULL -> NULL");
    for (const auto &s : take(sve_suggestions_json(ctx, "pending"))) sve_suggestion_reject(ctx, s["id"]);
}

// Delete all: everything the assistant stored goes, the notes stay for the host to delete.
static void run_api_wipe(sve_ctx *ctx) {
    sve_profile_set(ctx, R"({"name":"Lakshmi","lang":"te","routine":{"dinner":"20:00"}})");
    int64_t mid = sve_medicine_save(ctx, R"({"name":"Amlodipine","dose":"5 mg","when":"after dinner","repeat":"daily"})");
    json pend = take(sve_suggestions_json(ctx, "pending"));
    if (!pend.empty()) sve_suggestion_accept(ctx, pend[0]["id"], nullptr);
    sve_vocab_add(ctx, "Amlodipine");
    sve_task_create(ctx, R"({"text":"Buy tablets"})");
    int64_t note = make_note(ctx, json::parse(R"([{"start_sec":0,"text":"Amlodipine after dinner."}])"), "Before wipe");
    sve_setting_set(ctx, "insight.last_date", "2026-11-06");
    expect(mid > 0 && !take(sve_reminders_json(ctx, 0)).empty() && !take(sve_memory_json(ctx, "")).empty(), "wipe: data before");
    expect(sve_assistant_wipe(ctx) == 0, "sve_assistant_wipe returns 0");
    expect(take(sve_profile_get(ctx)).is_null() && take(sve_medicines_json(ctx)).empty() &&
           take(sve_reminders_json(ctx, 0)).empty() && take(sve_tasks_json(ctx, 0)).empty() &&
           take(sve_memory_json(ctx, "")).empty() && take(sve_suggestions_json(ctx, "")).empty() &&
           take(sve_topics_json(ctx, "")).empty() && take(sve_insight_latest(ctx)).is_null() &&
           take(sve_note_pieces(ctx, note)).empty(), "wipe clears profile, medicines, reminders, tasks, memory, cards, topics, pieces");
    char *last = sve_setting_get(ctx, "insight.last_date");
    expect(last == nullptr, "wipe clears insight.last_date");
    sve_free_string(last);
    expect(sve_assistant_wipe(nullptr) < 0, "wipe refuses a null context");
}

// Final review fixes: twice a day needs two times; repeat/until edits replace the old reminder;
// names from cards the person said No to stay known; an empty-times Yes writes nothing.
static void run_api_final(sve_ctx *ctx) {
    sve_profile_set(ctx, R"({"name":"Lakshmi","lang":"en","routine":{"breakfast":"08:00","dinner":"20:00"}})");
    for (const auto &s : take(sve_suggestions_json(ctx, "pending"))) sve_suggestion_reject(ctx, s["id"]);
    auto card = [&](int64_t id) { for (const auto &s : take(sve_suggestions_json(ctx, "pending"))) if (s["medicine_id"] == id) return s; return json(); };
    // I-2
    int64_t t1 = sve_medicine_save(ctx, R"({"id":0,"name":"Glycomet","dose":"500 mg","when":"after dinner","repeat":"twice_daily"})");
    json c1 = card(t1);
    expect(t1 > 0 && c1["payload"]["times"] == json::array({"20:00"}) && c1["check_status"] == "check" && has_field(c1, "times"),
           "twice_daily with one time: card flagged on times");
    int64_t t2 = sve_medicine_save(ctx, R"({"id":0,"name":"Telma","dose":"40 mg","times":["08:00","20:00"],"repeat":"twice_daily"})");
    expect(card(t2)["check_status"] == "ok", "twice_daily with two times: card ok");
    for (const auto &s : take(sve_suggestions_json(ctx, "pending"))) sve_suggestion_reject(ctx, s["id"]);

    // I-3: a change to repeat or until_date alone turns the old reminder off and supersedes the old card.
    auto active_for = [&](int64_t mid) {
        json out = json::array();
        for (const auto &r : take(sve_reminders_json(ctx, 1))) if (r["medicine_id"] == mid) out.push_back(r);
        return out;
    };
    int64_t u = sve_medicine_save(ctx, R"({"id":0,"name":"Cefixime","dose":"200 mg","when":"after dinner","repeat":"daily","until_date":"2026-11-12"})");
    int64_t r1 = sve_suggestion_accept(ctx, card(u)["id"], nullptr);
    expect(r1 > 0 && active_for(u).size() == 1 && active_for(u)[0]["until_date"] == "2026-11-12", "until: reminder active before the edit");
    sve_medicine_save(ctx, (R"({"id":)" + std::to_string(u) + R"(,"until_date":"2026-11-08"})").c_str());
    json cu = card(u);
    expect(active_for(u).empty() && cu["payload"]["until_date"] == "2026-11-08",
           "until_date change: old reminder off, new card carries the new end date");
    int64_t r2 = sve_suggestion_accept(ctx, cu["id"], nullptr);
    expect(r2 > 0 && active_for(u).size() == 1, "until: new reminder after Yes");
    sve_medicine_save(ctx, (R"({"id":)" + std::to_string(u) + R"(,"repeat":"weekly"})").c_str());
    expect(active_for(u).empty() && card(u)["payload"]["repeat"] == "weekly", "repeat change: old reminder off, new card weekly");
    json pend = take(sve_suggestions_json(ctx, "pending"));
    int n = 0;
    for (const auto &s : pend) if (s["medicine_id"] == u) ++n;
    expect(n == 1, "repeat change: exactly one live card for the medicine");
    sve_suggestion_accept(ctx, card(u)["id"], nullptr);
    sve_medicine_save(ctx, (R"({"id":)" + std::to_string(u) + R"(,"until_date":""})").c_str());
    expect(active_for(u).empty(), "clearing the end date also replaces the reminder");
    // I-4: a medicine named only on a card the person said No to (or that was superseded) stays known.
    int64_t dolo = add_sugg(ctx, {{"kind", "reminder"}, {"note_id", nullptr}, {"start_sec", 0}, {"quote", "Dolo 650 when fever"},
                                  {"check_status", "ok"}, {"check_fields", json::array()},
                                  {"payload", {{"text", "Take Dolo 650"}, {"times", json::array({"21:00"})}, {"repeat", "daily"},
                                               {"medicine", {{"name", "Dolo"}, {"dose", "650"}, {"when_text", "when fever"}}}}}});
    int64_t zinc = add_sugg(ctx, {{"kind", "profile"}, {"note_id", nullptr}, {"start_sec", 0}, {"quote", "Zincovit daily"},
                                  {"check_status", "ok"}, {"check_fields", json::array()},
                                  {"payload", {{"change", "add_medicine"}, {"text", "Add medicine: Zincovit"},
                                               {"medicine", {{"name", "Zincovit"}, {"dose", ""}, {"when_text", ""}}}}}});
    sve_suggestion_reject(ctx, dolo);
    ctx->store->set_suggestion_status(zinc, "superseded");
    json qa = take(sve_assistant_ask(ctx, "When do I take Dolo?", nullptr, nullptr));
    json qz = take(sve_assistant_ask(ctx, "Should I continue Zincovit?", nullptr, nullptr));
    expect(qa.is_object() && qa["medicine"] == true && qz.is_object() && qz["medicine"] == true,
           "names from rejected and superseded cards still route to the medicine answer");
    // Minor (c): a Yes on a reminder card with no times writes nothing, not even the medicine.
    int64_t nt = add_sugg(ctx, {{"kind", "reminder"}, {"note_id", nullptr}, {"start_sec", 0}, {"quote", "Shelcal 500 mg after lunch"},
                                {"check_status", "check"}, {"check_fields", json::array({"times"})},
                                {"payload", {{"text", "Take Shelcal 500 mg"}, {"times", json::array()}, {"repeat", "daily"},
                                             {"medicine", {{"name", "Shelcal"}, {"dose", "500 mg"}, {"when_text", "after lunch"}}}}}});
    expect(sve_suggestion_accept(ctx, nt, nullptr) == -1 && med_named(ctx, "Shelcal").is_null(),
           "empty-times Yes: -1 and no medicine created");
    // I-6: per-note extracted flag.
    int64_t old_note = make_note(ctx, json::parse(R"([{"start_sec":0,"text":"Call the bank on Monday."}])"), "Old note");
    expect(sve_note_extracted(ctx, old_note) == 0 && sve_note_extracted(nullptr, old_note) == -1,
           "sve_note_extracted: never extracted -> 0, NULL ctx -> -1");
    ctx->store->mark_extracted(old_note);
    expect(sve_note_extracted(ctx, old_note) == 1, "sve_note_extracted after a run -> 1");
    sve_note_delete(ctx, old_note);
    char *flag = sve_setting_get(ctx, ("extracted." + std::to_string(old_note)).c_str());
    expect(flag == nullptr, "deleting the note deletes its extracted flag");
    sve_free_string(flag);
    int64_t same = sve_medicine_save(ctx, (R"({"id":)" + std::to_string(u) + R"(,"dose":"200 mg"})").c_str());
    expect(same == u && card(u).is_object(), "unchanged save: still saved, card proposed");
    for (const auto &s : take(sve_suggestions_json(ctx, "pending"))) sve_suggestion_reject(ctx, s["id"]);
}

static int run_api(sve_ctx *ctx) {
    sve_profile_set(ctx, R"({"name":"Lakshmi","lang":"en","routine":{"breakfast":"08:00","dinner":"20:00"}})");
    json prof = take(sve_profile_get(ctx));
    expect(prof.is_object() && prof["name"] == "Lakshmi", "profile round trip");
    expect(sve_profile_set(ctx, "[1]") < 0 && sve_profile_set(ctx, "{bad") < 0, "profile set rejects non-objects");

    // The person's own medicine: saved directly, plus a reminder suggestion timed by the routine.
    int64_t mid = sve_medicine_save(ctx, R"({"id":0,"name":"Amlodipine","dose":"5 mg","when":"after dinner"})");
    expect(mid > 0, "medicine saved");
    json meds = take(sve_medicines_json(ctx));
    expect(meds.size() == 1 && meds[0]["times"] == json::array({"20:00"}), "medicine times from routine");
    json pend = take(sve_suggestions_json(ctx, "pending"));
    expect(pend.size() == 1 && pend[0]["kind"] == "reminder" && pend[0]["medicine_id"] == mid && pend[0]["check_status"] == "ok" &&
           pend[0]["quote"] == "Amlodipine 5 mg after dinner", "medicine save proposes a reminder");
    expect(sve_medicine_save(ctx, R"({"id":0,"name":"X","repeat":"hourly"})") < 0, "invalid repeat rejected");
    expect(sve_medicine_save(ctx, R"({"id":0,"name":""})") < 0, "empty name rejected");
    expect(sve_medicine_save(ctx, R"({"id":999,"dose":"1 mg"})") < 0, "unknown id rejected");

    expect(sve_medicine_save(ctx, (R"({"id":)" + std::to_string(mid) + R"(,"dose":"10 mg"})").c_str()) == mid, "medicine update");
    pend = take(sve_suggestions_json(ctx, "pending"));
    expect(pend.size() == 1 && contains(pend[0]["payload"], "10 mg"), "update supersedes the older pending reminder");

    // Accept with an edit -> reminder row from the final payload.
    int64_t sid = pend.empty() ? 0 : pend[0]["id"].get<int64_t>();
    int64_t rid = sve_suggestion_accept(ctx, sid, R"({"times":["21:00"]})");
    expect(rid > 0, "accept returns the reminder id");
    json rems = take(sve_reminders_json(ctx, 1));
    expect(rems.size() == 1 && rems[0]["id"] == rid && rems[0]["times"] == json::array({"21:00"}) && rems[0]["repeat"] == "daily" &&
           rems[0]["medicine_id"] == mid, "reminder row from the edited payload");
    expect(sve_suggestion_accept(ctx, sid, nullptr) < 0, "double accept refused");
    expect(sve_suggestion_accept(ctx, 9999, nullptr) < 0, "unknown suggestion refused");
    expect(sve_suggestion_reject(ctx, sid) < 0, "reject of an accepted suggestion refused");

    // Medicine Ask route works without a model and quotes stored facts.
    json pieces = json::parse(R"([{"start_sec":0,"text":"Take Amlodipine 10 mg once a day after dinner."},{"start_sec":28,"text":"Salt less than one teaspoon."}])");
    int64_t note = make_note(ctx, pieces, "Doctor visit");
    json a = take(sve_assistant_ask(ctx, "When do I take Amlodipine?", nullptr, nullptr));
    printf("--- ask (no model)\n%s\n", a.dump(2).c_str());
    expect(a.is_object() && a["medicine"] == true && a["not_found"] == false &&
           contains(a["answer"], "Take Amlodipine 10 mg once a day after dinner.") && a["sources"].size() == 1 &&
           a["sources"][0]["start_sec"] == 0, "medicine route quotes the piece with its source");
    sve_profile_set(ctx, R"({"name":"Lakshmi","lang":"te"})");
    json te = take(sve_assistant_ask(ctx, "Which pills for sugar?", nullptr, nullptr));
    printf("--- ask te (no model)\n%s\n", te.dump(2).c_str());
    expect(te.is_object() && te["medicine"] == true, "medicine word routes to the medicine answer");
    expect(take(sve_assistant_ask(ctx, nullptr, nullptr, nullptr)).is_null(), "null question -> NULL");
    expect(take(sve_assistant_ask(ctx, "What about salt?", nullptr, nullptr)).is_null(), "general Ask without a model -> NULL");

    // Profile suggestion: accept adds the medicine; a later stop deletes it.
    sve_medicine_delete(ctx, mid);
    expect(take(sve_reminders_json(ctx, 0)).empty(), "medicine delete cascades reminders");
    expect(take(sve_suggestions_json(ctx, "")).empty(), "medicine delete cascades its suggestions");

    expect(sve_vocab_add(ctx, "Amlodipine") > 0 && sve_vocab_add(ctx, "amlodipine") == sve_vocab_add(ctx, "Amlodipine"), "vocab add dedupes");
    expect(sve_note_set_pieces(ctx, note, "{\"x\":1}") < 0, "pieces must be an array");
    json np = take(sve_note_pieces(ctx, note));
    expect(np.size() == 2 && np[1]["start_sec"] == 28, "pieces round trip");
    expect(take(sve_insight_latest(ctx)).is_null(), "no insight yet");
    expect(sve_extract(ctx, note, nullptr, nullptr) < 0, "extract without a model is an error");
    expect(sve_topic_summary(ctx, 12345, "en", nullptr, nullptr) == nullptr, "unknown topic -> NULL");
    run_api_fixes(ctx);
    run_api_direct(ctx);
    run_api_times(ctx);
    run_api_final(ctx);
    run_api_wipe(ctx);
    return 0;
}

// Copies the WAV's samples into `dst` in steps, with a header whose data size is 0 (as while recording),
// and asks for the next piece after every step; after the last step, is_final = 1 until "end".
static int run_live(sve_ctx *ctx, const char *wav) {
    std::ifstream in(wav, std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    size_t data = bytes.find("data");
    if (bytes.size() < 44 || data == std::string::npos) { fprintf(stderr, "not a WAV: %s\n", wav); return 1; }
    const size_t head = data + 8;
    std::string dst = std::string(getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp/") + "/sve_live_smoke.wav";
    std::string header = bytes.substr(0, head);
    memset(&header[data + 4], 0, 4);   // data size 0 until the recorder stops
    { std::ofstream o(dst, std::ios::binary | std::ios::trunc); o << header; }
    const size_t step = 10 * 16000 * 2, total = bytes.size() - head;
    size_t written = 0;
    int64_t cursor = 0;
    std::string prev;
    int pieces = 0, waits = 0;
    double audio_sec = 0, took_sec = 0;
    for (bool final = false;;) {
        if (!final) {
            size_t n = std::min(step, total - written);
            std::ofstream o(dst, std::ios::binary | std::ios::app);
            o.write(bytes.data() + head + written, (std::streamsize)n);
            written += n;
            if (written >= total) final = true;
        }
        for (;;) {
            auto t0 = std::chrono::steady_clock::now();
            json r = take(sve_transcribe_next_piece(ctx, dst.c_str(), cursor, final ? 1 : 0, prev.empty() ? nullptr : prev.c_str(), nullptr, nullptr));
            double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            if (r.is_null()) { fprintf(stderr, "next_piece failed at %lld\n", (long long)cursor); return 1; }
            std::string st = r.value("status", "");
            if (st == "wait") { ++waits; printf("[recorded %5.1f s] wait (cursor %.1f s)\n", written / 32000.0, cursor / 16000.0); break; }
            if (st == "end") { printf("[final] end at %.1f s\n", cursor / 16000.0); goto done; }
            int64_t end = r.value("end_sample", (int64_t)0);
            double au = (end - cursor) / 16000.0;
            audio_sec += au; took_sec += s; ++pieces;
            std::string text = r.value("text", "");
            printf("[recorded %5.1f s%s] piece start_sec=%lld end_sample=%lld (%.1f s audio, %.1f s, rtf %.2f)\n  %s\n",
                   written / 32000.0, final ? ", final" : "", (long long)r.value("start_sec", (int64_t)0), (long long)end, au, s, s / au, text.c_str());
            if (end <= cursor) { fprintf(stderr, "cursor did not advance\n"); return 1; }
            cursor = end;
            if (!text.empty()) prev = text;
        }
    }
done:
    printf("LIVE OK: %d pieces, %d waits, %.1f s audio in %.1f s (rtf %.2f)\n", pieces, waits, audio_sec, took_sec, took_sec / std::max(1.0, audio_sec));
    remove(dst.c_str());
    return 0;
}

// Transcribes the WAV piece by piece (as live does), plans ~5 minute sections, summarises each and prints
// the JSON with the seconds it took. Expects a title and 1..6 points per section; "fallback" is reported.
// SVE_SECTION_MAX_OUT=8 truncates every answer, so the retry and the fallback path run and must still return a summary.
static int run_sections(sve_ctx *ctx, const char *wav, const char *lang, json *stored = nullptr, double *dur_out = nullptr) {
    if (const char *m = getenv("SVE_SECTION_MAX_OUT")) g_sve_section_max_out = atoi(m);
    const bool force_fb = getenv("SVE_SECTION_MAX_OUT") != nullptr;
    std::vector<std::pair<int64_t, std::string>> pieces;
    int64_t cursor = 0;
    double dur = 0;
    { std::ifstream f(wav, std::ios::binary | std::ios::ate); dur = f.tellg() > 44 ? (double)(f.tellg() - (std::streamoff)44) / 32000.0 : 0; }
    std::string prev;
    for (;;) {
        json r = take(sve_transcribe_next_piece(ctx, wav, cursor, 1, prev.empty() ? nullptr : prev.c_str(), nullptr, nullptr));
        if (r.is_null()) { fprintf(stderr, "next_piece failed\n"); return 1; }
        if (r.value("status", "") != "piece") break;
        int64_t end = r.value("end_sample", (int64_t)0);
        if (end <= cursor) return 1;
        cursor = end;
        std::string t = r.value("text", "");
        if (!t.empty()) { pieces.push_back({r.value("start_sec", (int64_t)0), t}); prev = t; }
    }
    if (dur_out) *dur_out = dur;
    printf("transcribed %zu pieces, %.1f s audio\n", pieces.size(), dur);
    std::vector<sve::PieceRef> refs;
    for (size_t i = 0; i < pieces.size(); ++i)
        refs.push_back({pieces[i].first, i + 1 < pieces.size() ? pieces[i + 1].first : (int64_t)dur});
    auto secs = sve::plan_sections(refs, true);
    for (size_t k = 0; k < secs.size(); ++k) {
        json arr = json::array();
        for (size_t i = secs[k].first; i < secs[k].second; ++i) arr.push_back({{"start_sec", pieces[i].first}, {"text", pieces[i].second}});
        auto t0 = std::chrono::steady_clock::now();
        json s = take(sve_section_summarize(ctx, arr.dump().c_str(), lang, nullptr, nullptr));
        double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        printf("--- section %zu (pieces %zu..%zu, %.1f s)\n%s\n", k, secs[k].first, secs[k].second, sec, s.dump(2).c_str());
        expect(s.is_object() && s.contains("title") && s["points"].is_array() && !s["points"].empty() && s["points"].size() <= 6,
               "section summary has a title and 1..6 points");
        if (stored) stored->push_back({{"idx", (int)k}, {"first_piece", (int)secs[k].first}, {"last_piece", (int)secs[k].second},
                                       {"start_sec", refs[secs[k].first].start_sec}, {"end_sec", refs[secs[k].second - 1].end_sec}, {"summary", s}});
        if (s.value("fallback", false)) printf("(fallback used)\n");
        if (force_fb) expect(s.value("fallback", false), "forced truncation returns the fallback summary");
    }
    return 0;
}

// Synthetic ~2 hour input: the fixture's section summaries repeated with shifted starts (no new model work per
// repeat). Times notes for all three kinds and the title, and prints chapters. Output is labelled synthetic.
static int run_longnotes(sve_ctx *ctx, const char *wav, const char *lang) {
    json base = json::array();
    double dur = 0;
    if (run_sections(ctx, wav, lang, &base, &dur) != 0 || base.empty()) return 1;
    const int64_t span = std::max<int64_t>(1, (int64_t)dur + 1);
    const int repeats = (int)std::max<int64_t>(1, (7200 + span - 1) / span);
    json all = json::array();
    for (int r = 0; r < repeats; ++r)
        for (auto sec : base) {
            int64_t off = (int64_t)r * span;
            sec["idx"] = (int)all.size();
            sec["start_sec"] = sec["start_sec"].get<int64_t>() + off;
            sec["end_sec"] = sec["end_sec"].get<int64_t>() + off;
            for (auto &p : sec["summary"]["points"]) p["start_sec"] = p["start_sec"].get<int64_t>() + off;
            all.push_back(sec);
        }
    int64_t id = sve_note_create(ctx, "synthetic long note", lang, nullptr, "x", (int64_t)repeats * span);
    expect(id > 0, "note created");
    expect(sve_note_set_sections(ctx, id, all.dump().c_str()) == 0, "sections stored");
    printf("SYNTHETIC ~%.1f h input: %zu sections (fixture's %zu repeated %d times)\n", repeats * span / 3600.0, all.size(), base.size(), repeats);
    json ch = take(sve_chapters_json(ctx, id));
    printf("chapters: %zu\n", ch.size());
    const char *names[] = {"short summary", "detailed notes", "revision questions"};
    for (int k = 0; k < 3; ++k) {
        auto t0 = std::chrono::steady_clock::now();
        char *o = sve_generate_from_sections(ctx, id, (sve_output_kind)k, lang, nullptr, nullptr);
        double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        printf("--- SYNTHETIC %s: %.1f s\n%s\n", names[k], sec, o ? o : "(NULL)");
        expect(o && *o, "notes generated from sections");
        sve_free_string(o);
    }
    auto t0 = std::chrono::steady_clock::now();
    char *t = sve_title_from_sections(ctx, id, lang);
    printf("--- SYNTHETIC title: %.1f s\n%s\n", std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(), t ? t : "(NULL)");
    expect(t && *t, "title from sections");
    sve_free_string(t);
    return 0;
}

int main(int argc, char **argv) {
    const char *mode = argc > 1 ? argv[1] : "transcribe";
    const char *gguf = getenv("SVE_GEMMA"), *mmproj = getenv("SVE_MMPROJ"), *wav = getenv("SVE_TEST_WAV");
    sve_ctx *ctx = sve_init(":memory:");
    if (!ctx) { fprintf(stderr, "sve_init failed\n"); return 1; }
    if (!strcmp(mode, "api")) {
        run_api(ctx);
        sve_free(ctx);
        printf("%s (%d missed)\n", fails ? "API FAIL" : "API OK", fails);
        return fails ? 1 : 0;
    }
    if (!gguf) { fprintf(stderr, "set SVE_GEMMA\n"); return 2; }
    const bool live = !strcmp(mode, "live") || !strcmp(mode, "sections") || !strcmp(mode, "longnotes");
    // live and sections run as on the phone: CPU only (the projector follows), 4 threads.
    if (sve_load_llm(ctx, gguf, 8192, live ? 4 : 0, live ? 0 : 99) != 0) { fprintf(stderr, "sve_load_llm failed\n"); return 1; }
    if (live) {
        const bool sections = !strcmp(mode, "sections");
        if (!mmproj || !wav) { fprintf(stderr, "set SVE_MMPROJ and SVE_TEST_WAV\n"); return 2; }
        if (sve_load_audio(ctx, mmproj) != 0) { fprintf(stderr, "sve_load_audio failed\n"); return 1; }
        const char *lang = getenv("SVE_LANG");
        const bool longnotes = !strcmp(mode, "longnotes");
        int rc = longnotes ? run_longnotes(ctx, wav, lang ? lang : "te") : sections ? run_sections(ctx, wav, lang ? lang : "te") : run_live(ctx, wav);
        if (longnotes) { printf("%s (%d missed)\n", fails ? "LONGNOTES FAIL" : "LONGNOTES OK", fails); if (fails) rc = 1; }
        if (sections) { printf("%s (%d missed)\n", fails ? "SECTIONS FAIL" : "SECTIONS OK", fails); if (fails) rc = 1; }
        sve_free(ctx);
        return rc;
    }
    if (!strcmp(mode, "transcribe")) {
        if (!mmproj || !wav) { fprintf(stderr, "set SVE_MMPROJ and SVE_TEST_WAV\n"); return 2; }
        if (sve_load_audio(ctx, mmproj) != 0) { fprintf(stderr, "sve_load_audio failed\n"); return 1; }
        char *t = sve_transcribe_audio(ctx, wav, progress, nullptr);
        if (!t || !*t) { fprintf(stderr, "empty transcript\n"); return 1; }
        printf("%s\n", t);
        sve_free_string(t);
    } else if (!strcmp(mode, "extract")) {
        const char *fx = getenv("SVE_FIXTURE");
        int rc = run_extract(ctx, fx ? fx : SVE_FIXTURE_DIR "/doctor_visit.txt");
        if (rc) return rc;
        printf("%d expectation(s) missed (judged qualitatively)\n", fails);
    } else { fprintf(stderr, "unknown mode %s\n", mode); return 2; }
    sve_free(ctx);
    return 0;
}
