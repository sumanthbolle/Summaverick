// On-device-model smoke test. Env: SVE_GEMMA, SVE_MMPROJ, SVE_TEST_WAV, SVE_FIXTURE.
// Arg: mode
//   transcribe  transcribe SVE_TEST_WAV
//   live        CPU, 4 threads: sve_transcribe_next_piece in a loop over SVE_TEST_WAV as if it were being
//               recorded (copied in 10 s steps into a WAV whose header says data size 0), then final
//   extract     doctor-visit fixture: extract, list pending suggestions, two Ask questions
//   threads     runtime CPU threads (SVE_GEMMA, CPU): load-time values, set, a run still works, set back
//   api         assistant C API without a model (accept/reject, medicines, medicine Ask route)
//   fits        model only: sve_transcript_fits on the doctor-visit fixture and on longer text, checked
//               against the first phase sve_generate runs (final = one prompt, chunk = summarised first)
//   notes       SVE_TEST_WAV: pieces, then the three note kinds from the transcript (SVE_LANG, default te),
//               then one chat question over the note (SVE_QUESTION)
//   memories    SVE_DB (a copy of a notes.sqlite): missing sections, then memories for every due section;
//               counts, drop reasons and seconds per section (see run_memories)
// SVE_GREEDY=1 (any value but empty or 0) makes every generation greedy; SVE_TOKEN_LOG=<file> appends prompt/generated token ids
// (both for tools/model/compare_outputs.sh).
#include "sve_core.h"
#include "sve_internal.h"
#include "sve_threads.h"   // threads mode checks the core's thread rule
#include "sve_chat.h"     // the health line, api mode
#include "sve_sections.h"  // api mode adds model-like suggestions straight to the store
#include <sqlite3.h>
#include <sys/resource.h>  // embed/gate modes: peak resident memory
#include <unistd.h>        // priya: mkstemp, close
#include <map>
#include "third_party/nlohmann/json.hpp"
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <cstring>
#include <algorithm>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

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
    json ins = take(sve_insight_generate(ctx, note, progress, nullptr));
    printf("--- insight\n%s\n", ins.dump(2).c_str());
    json again = take(sve_insight_generate(ctx, note, nullptr, nullptr));
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
    ctx->store->add_memory_item({{"type", "fact"}, {"text", "Take the iron tablet with orange juice"}, {"review", "kept"}});
    json f = take(sve_assistant_ask(ctx, "Anything about orange juice?", nullptr, nullptr));
    printf("--- ask fact\n%s\n", f.dump(2).c_str());
    expect(f.is_object() && f["medicine"] == true && contains(f["answer"], "Take the iron tablet with orange juice"),
           "medicine fact quoted verbatim");
    // An accepted fact card is a kept memory with the card's quote; the Learned list shows and deletes it.
    json fc = {{"kind", "fact"}, {"note_id", note}, {"quote", "Keep the spare key with Ravi"}, {"start_sec", 0},
               {"payload", {{"text", "Spare key is with Ravi"}}}};
    int64_t fm = sve_suggestion_accept(ctx, add_sugg(ctx, fc), nullptr);
    json fmem = ctx->store->memory_item(fm);
    expect(fm > 0 && fmem.is_object() && fmem["review"] == "kept" && fmem["sources"].size() == 1 &&
           fmem["sources"][0]["quote"] == "Keep the spare key with Ravi" && ctx->store->memory("fact").empty(),
           "accepted fact card becomes a memory with its quote");
    bool listed = false;
    for (const auto &m : take(sve_memory_json(ctx, "fact"))) listed = listed || (m["id"] == -fm && m["text"] == "Spare key is with Ravi");
    expect(listed && sve_memory_delete(ctx, -fm) == 0 && ctx->store->memory_item(fm).is_null(), "Learned lists and deletes memory facts");
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

// Memory C API without a model (g_sve_model_reply stands in for it): grounding drops, the yes/no check, a cut
// reply keeping its whole items, the review queue, the person's review/edit/remove, people and card links,
// medicine memories, a deleted recording.
static std::vector<std::string> g_api_drops;
static void api_drop(int64_t, int, const char *reason, const char *) { g_api_drops.push_back(reason); }
static const char *api_model(const char *phase, const char *prompt) {
    const std::string p = prompt;
    if (!strcmp(phase, "memory_check")) return p.find("kitchen pipe") != std::string::npos ? "No." : "Yes";
    if (p.find("[t=90] The electrician") != std::string::npos)  // cut off at the cap inside the second item
        return R"({"memories":[{"type":"fact","text":"The electrician comes back on Monday.","quote":"The electrician comes back on Monday morning","start_sec":90},{"type":"idea","text":"cu)";
    return R"(```json
{"memories":[
 {"type":"fact","text":"The kitchen pipe must be replaced soon.","quote":"the kitchen pipe must be replaced soon","start_sec":0},
 {"type":"promise","text":"Ravi will send the quotation by Friday.","who":"Ravi","due_text":"Friday evening",
  "quote":"Ravi will send the quotation by Friday evening","start_sec":30,"names":[{"as_said":"Ravi","as_written":"Ravi"}]},
 {"type":"fact","text":"The roof leaks every monsoon.","quote":"the roof leaks every monsoon","start_sec":0},
 {"type":"fact","text":"Take Dolo 650 twice for fever.","quote":"Take Dolo 650 twice if the fever comes back","start_sec":60}]}
```)";
}

static void run_api_memories(sve_ctx *ctx) {
    g_sve_memory_drop = api_drop;
    json pieces = json::parse(R"([{"start_sec":0,"text":"The plumber said the kitchen pipe must be replaced soon."},
        {"start_sec":30,"text":"Ravi will send the quotation by Friday evening."},
        {"start_sec":60,"text":"Take Dolo 650 twice if the fever comes back."},
        {"start_sec":90,"text":"The electrician comes back on Monday morning at nine."}])");
    int64_t note = make_note(ctx, pieces, "Plumber visit");
    int64_t tid = ctx->store->upsert_topic("topic", "Kitchen");
    ctx->store->link_topic(note, tid);
    // A pending task card for the same moment and words (extraction makes these).
    int64_t card = ctx->store->add_suggestion({{"kind", "task"}, {"payload", {{"text", "Get the quotation from Ravi"}}},
                                               {"quote", "Ravi will send the quotation by Friday evening."}, {"note_id", note},
                                               {"start_sec", 30}});
    auto sec = [](int idx, int a, int b, int s0, int s1) {
        return json{{"idx", idx}, {"first_piece", a}, {"last_piece", b}, {"start_sec", s0}, {"end_sec", s1},
                    {"summary", {{"title", "T"}, {"points", json::array({{{"text", "p"}, {"start_sec", s0}}})}}}};
    };
    json secs = json::array({sec(0, 0, 2, 0, 90), sec(1, 3, 3, 90, 120), sec(2, 4, 4, 120, 120)});   // both ends included
    expect(sve_note_set_sections(ctx, note, secs.dump().c_str()) == 0, "memories: sections stored");
    expect(take(sve_memory_sections_due(ctx, note)) == json::parse("[0,1,2]"), "memories: every section due");
    json needing = take(sve_notes_needing_memories(ctx, 10));
    expect(needing.is_array() && std::find(needing.begin(), needing.end(), json(note)) != needing.end(), "memories: note needs memories");

    expect(sve_memories_from_section(ctx, note, 0, nullptr, nullptr) == -1, "memories: no model -> -1");
    expect(take(sve_memory_sections_due(ctx, note)).size() == 3, "memories: ... and the section stays due");
    g_sve_model_reply = api_model;
    expect(sve_memories_from_section(ctx, note, 7, nullptr, nullptr) == -1, "memories: unknown section -> -1");
    expect(sve_memories_from_section(ctx, 987654, 0, nullptr, nullptr) == -1, "memories: unknown note -> -1");
    expect(sve_memories_from_section(nullptr, note, 0, nullptr, nullptr) == -1, "memories: no ctx -> -1");
    g_api_drops.clear();
    int n = sve_memories_from_section(ctx, note, 0, nullptr, nullptr);
    printf("--- memories from section 0 (stand-in model): %d stored, drops:", n);
    for (const auto &d : g_api_drops) printf(" %s", d.c_str());
    printf("\n");
    auto dropped = [](const char *r) { return std::find(g_api_drops.begin(), g_api_drops.end(), r) != g_api_drops.end(); };
    expect(n == 1, "memories: one of four candidates stored");
    expect(g_api_drops.size() == 3 && dropped("quote_not_found") && dropped("medicine") && dropped("double_check_no"),
           "memories: unsaid, medicine and checked-no candidates dropped with their reasons");
    expect(sve_memories_from_section(ctx, note, 0, nullptr, nullptr) == 0, "memories: a done section is not made again");
    g_api_drops.clear();
    expect(sve_memories_from_section(ctx, note, 1, nullptr, nullptr) == 1 && g_api_drops.empty(),
           "memories: a cut reply keeps its whole item, no redo");
    expect(sve_memories_from_section(ctx, note, 2, nullptr, nullptr) == 0, "memories: a section with no pieces stores none");
    expect(take(sve_memory_sections_due(ctx, note)).empty(), "memories: no section due");

    json all = take(sve_memories_json(ctx, (R"({"note_id":)" + std::to_string(note) + "}").c_str()));
    printf("--- memories\n%s\n", all.dump(2).c_str());
    json q = take(sve_review_queue_json(ctx));
    expect(q.is_array() && q.size() == 2, "memories: both wait for review (direct-to-map is off)");
    json promise, fact;
    for (const auto &m : q) (m["type"] == "promise" ? promise : fact) = m;
    expect(q.size() == 2 && q[0]["sources"][0]["start_sec"] == 30, "memories: queue in the order said");
    expect(promise.is_object() && promise["who"] == "Ravi" && promise["due_text"] == "Friday evening" &&
           promise["suggestion_id"] == card && promise["suggestion"].is_object() && promise["suggestion"]["id"] == card,
           "memories: promise keeps who/due and links the pending card");
    expect(promise.is_object() && promise["people"].size() == 1 && promise["people"][0]["name"] == "Ravi" &&
           promise["people"][0]["role"] == "who", "memories: Ravi linked once, as who");
    expect(promise.is_object() && promise["sources"].size() == 1 && promise["sources"][0]["start_sec"] == 30 &&
           promise["sources"][0]["quote"] == "Ravi will send the quotation by Friday evening.", "memories: source is the transcript's words");
    expect(fact.is_object() && fact["text"] == "The electrician comes back on Monday." && fact["suggestion"].is_null() &&
           fact["importance"].get<double>() > 0, "memories: the cut reply's fact, no card, has importance");

    const int64_t pid = promise.value("id", (int64_t)0), fid = fact.value("id", (int64_t)0);
    expect(sve_memory_review(ctx, pid, "maybe") == -1, "memories: unknown review state refused");
    expect(sve_memory_review(ctx, pid, "kept") == 0, "memories: keep");
    json kept = take(sve_memory_get(ctx, pid));
    expect(kept["review"] == "kept" && kept["queue"] == 0, "memories: kept leaves the queue");
    expect(sve_memory_edit(ctx, fid, "  ") == -1, "memories: empty edit refused");
    expect(sve_memory_edit(ctx, fid, "The electrician comes on Monday.") == 0, "memories: edit");
    json ed = take(sve_memory_get(ctx, fid));
    expect(ed["text"] == "The electrician comes on Monday." && ed["review"] == "corrected" && ed["queue"] == 0, "memories: edit is corrected");
    expect(take(sve_review_queue_json(ctx)).empty(), "memories: queue empty after review");
    expect(sve_memory_set_done(ctx, pid, 1) == 0 && take(sve_memory_get(ctx, pid))["done"] == true, "memories: set done");
    expect(sve_memory_review(ctx, fid, "wrong") == 0, "memories: wrong");
    expect(take(sve_memories_json(ctx, (R"({"note_id":)" + std::to_string(note) + "}").c_str())).size() == 1, "memories: wrong rows hidden");
    expect(sve_memory_remove(ctx, fid) == 0 && take(sve_memory_get(ctx, fid)).is_null(), "memories: remove");
    expect(sve_memory_remove(ctx, fid) == -1, "memories: remove twice refused");

    // Medicine memories: made from the saved row, not editable or removable on their own, gone with the medicine.
    int64_t med = sve_medicine_save(ctx, R"({"id":0,"name":"Telma","dose":"40 mg","when":"after breakfast"})");
    json mm;
    for (const auto &m : take(sve_memories_json(ctx, nullptr))) if (m["origin"] == "medicine" && m["medicine_id"] == med) mm = m;
    expect(mm.is_object() && mm["text"] == "Telma 40 mg, after breakfast" && mm["review"] == "kept" && mm["health"] == 1,
           "medicine memory from the saved medicine");
    const int64_t mmid = mm.value("id", (int64_t)0);
    expect(sve_medicine_save(ctx, (R"({"id":)" + std::to_string(med) + R"(,"dose":"80 mg"})").c_str()) == med &&
           take(sve_memory_get(ctx, mmid))["text"] == "Telma 80 mg, after breakfast", "medicine memory follows a dose change");
    expect(sve_memory_edit(ctx, mmid, "Telma 20 mg") == -1 && sve_memory_remove(ctx, mmid) == -1 &&
               sve_memory_review(ctx, mmid, "wrong") == -1, "medicine memory is changed by the medicine only");
    expect(sve_medicine_delete(ctx, med) == 0 && take(sve_memory_get(ctx, mmid)).is_null(), "medicine delete removes its memory");

    // A recording deleted mid-way: its memories go, the next call for it is -1.
    sve_note_delete(ctx, note);
    expect(take(sve_memory_get(ctx, pid)).is_null(), "memories go with their only recording");
    expect(sve_memories_from_section(ctx, note, 0, nullptr, nullptr) == -1, "memories: deleted note -> -1");

    // The memory call itself fails (the stand-in returns NULL): -3, nothing stored, the section stays due; a reply
    // that comes back but is unusable marks it done with 0.
    int64_t fail_note = make_note(ctx, pieces, "Model failed");
    expect(sve_note_set_sections(ctx, fail_note, json::array({sec(0, 0, 1, 0, 60), sec(1, 2, 3, 60, 120)}).dump().c_str()) == 0,
           "model failure: two sections stored");
    g_sve_model_reply = [](const char *phase, const char *) -> const char * { return !strcmp(phase, "memories") ? nullptr : "Yes"; };
    expect(sve_memories_from_section(ctx, fail_note, 0, nullptr, nullptr) == -3, "model failure: the call fails -> -3");
    expect(take(sve_memory_sections_due(ctx, fail_note)) == json::parse("[0,1]") &&
               take(sve_memories_json(ctx, (R"({"note_id":)" + std::to_string(fail_note) + "}").c_str())).empty(),
           "model failure: nothing stored, the section stays due");
    g_sve_model_reply = [](const char *, const char *) -> const char * { return "I cannot help with that."; };
    expect(sve_memories_from_section(ctx, fail_note, 0, nullptr, nullptr) == 0 &&
               take(sve_memory_sections_due(ctx, fail_note)) == json::parse("[1]"),
           "model failure: an unusable reply marks the section done with 0");
    sve_note_delete(ctx, fail_note);
    g_sve_model_reply = api_model;

    // Review Focus 4, as the app's memory pass runs it (ProcessingPipeline.writeMemories): the due sections are read
    // once, then written one at a time; the recording is deleted between section 0 and section 1.
    int64_t mid_note = make_note(ctx, pieces, "Deleted mid-way");
    expect(sve_note_set_sections(ctx, mid_note, json::array({sec(0, 0, 1, 0, 60), sec(1, 2, 3, 60, 120)}).dump().c_str()) == 0,
           "mid-way: two sections stored");
    json due = take(sve_memory_sections_due(ctx, mid_note));
    expect(due == json::parse("[0,1]"), "mid-way: both sections due");
    std::vector<int> results;
    int64_t first_id = 0;
    for (const auto &idx : due) {
        g_api_drops.clear();
        int r = sve_memories_from_section(ctx, mid_note, idx.get<int>(), nullptr, nullptr);
        results.push_back(r);
        printf("--- mid-way: section %d -> %d, drops:", idx.get<int>(), r);
        for (const auto &d : g_api_drops) printf(" %s", d.c_str());
        printf("\n");
        if (r < 0) break;  // the pass ends quietly on -1
        if (idx == 0) {
            json mine = take(sve_memories_json(ctx, (R"({"note_id":)" + std::to_string(mid_note) + "}").c_str()));
            first_id = mine.empty() ? 0 : mine[0].value("id", (int64_t)0);
            sve_note_delete(ctx, mid_note);  // the person deletes the recording while its memories are being written
        }
    }
    expect(results.size() == 2 && results[0] >= 1 && results[1] == -1, "mid-way: section 0 stored, section 1 after the delete -> -1");
    expect(first_id > 0 && take(sve_memory_get(ctx, first_id)).is_null(), "mid-way: section 0's memories went with the recording");
    expect(take(sve_memories_json(ctx, (R"({"note_id":)" + std::to_string(mid_note) + "}").c_str())).empty() &&
               take(sve_memory_sections_due(ctx, mid_note)).empty(), "mid-way: nothing of it is left or due");
    json still = take(sve_notes_needing_memories(ctx, 100));
    expect(still.is_array() && std::find(still.begin(), still.end(), json(mid_note)) == still.end(),
           "mid-way: the deleted note never comes back for memories");
    g_sve_model_reply = nullptr;
    g_sve_memory_drop = nullptr;
}

// Ask as a chat (N9) without a model (g_sve_model_reply stands in, its reply fed in 3-byte pieces): the streamed
// pieces join to the answer, thinking and labels never shown; the question's language; history in the prompt,
// sources as text, medicine answers left out of it; cancel through on_text; medicine never streamed.
static std::string g_chat_prompt;
static const char *g_chat_reply = nullptr;
static const char *g_classify_reply = nullptr;
static int g_model_calls = 0, g_classify_calls = 0;
static const char *chat_model(const char *phase, const char *prompt) {
    if (!strcmp(phase, "classify")) { ++g_classify_calls; return g_classify_reply; }
    ++g_model_calls;
    g_chat_prompt = prompt;
    return g_chat_reply;
}
struct ChatSink {
    std::string text;
    int pieces = 0, stop_after = -1;
};
static int chat_text(const char *p, void *u) {
    ChatSink *s = static_cast<ChatSink *>(u);
    s->text += p;
    ++s->pieces;
    return s->stop_after >= 0 && s->pieces >= s->stop_after ? 1 : 0;
}

// N10: the right source for each kind of question, without a model.
template <class Chat>
static void run_api_route(sve_ctx *ctx, Chat chat) {
    const auto ask = [](const char *t) { return json::array({{{"role", "user"}, {"text", t}}}); };
    // Period summary: stored summaries of today's notes, each note a source, medicine sentences left out.
    int64_t a = sve_note_create(ctx, "Budget meeting", "en", nullptr, "We set the budget.", 60);
    sve_note_set_output(ctx, a, SVE_OUT_SHORT_SUMMARY, "The team set the walnut budget for March.");
    sve_note_set_output(ctx, a, SVE_OUT_REVISION_QUESTIONS,
                        "Q1. Which budget was set?\nA1. The walnut budget.\n\nQ2. Which tablet?\nA2. Dolo 650.\n\nQ3. For when?\nA3. March.");
    g_chat_reply = "- [Budget meeting] The walnut budget was set. Paracetamol relieves pain and fever.\n- [Budget meeting] It is for March.\n";
    ChatSink s1;
    json sum = chat(ask("Highlight this week key discussions to remember"), s1, nullptr);
    printf("--- summary (no model)\n%s\n", sum.dump(2).c_str());
    bool cites = false;
    if (sum.is_object() && sum["sources"].is_array())
        for (const auto &x : sum["sources"]) cites = cites || (x["note_id"] == a && x["title"] == "Budget meeting");
    expect(sum.is_object() && sum["kind"] == "summary" && cites && g_chat_prompt.find("The team set the walnut budget") != std::string::npos,
           "summary: from the stored summaries, cites the note");
    expect(sum.is_object() && sum["answer"].get<std::string>() == s1.text && s1.text.find("Paracetamol") == std::string::npos &&
           s1.text.find("It is for March") != std::string::npos, "summary: streamed; a medicine sentence left out, the rest goes on");
    ChatSink s2;
    json lastm = chat(ask("What happened last week?"), s2, nullptr);
    expect(lastm.is_object() && lastm["kind"] == "summary" && lastm["answer"] == "I don't have any recordings from then." && lastm["not_found"] == true,
           "summary: an empty period says so");

    // Quiz: stored questions, unasked, never about a medicine; no model.
    g_chat_prompt.clear();
    ChatSink s3;
    json q = chat(ask("Quiz me on my recent recordings"), s3, nullptr);
    printf("--- quiz (no model)\n%s\n", q.dump(2).c_str());
    expect(q.is_object() && q["kind"] == "quiz" && q["question"] == "Which budget was set?" && q["answer"] == "The walnut budget." &&
           q["source"]["note_id"] == a && q["quiz_id"] == std::to_string(a) + ":1" && s3.pieces == 0 && g_chat_prompt.empty(),
           "quiz: a stored question with its answer and source, no model");
    json msgs = ask("Next question");
    msgs[0]["quiz_asked"] = json::array({std::to_string(a) + ":1"});
    ChatSink s4;
    json q2 = chat(msgs, s4, nullptr);
    expect(q2.is_object() && q2["kind"] == "quiz" && q2["question"] == "For when?", "quiz: not one already asked, never a medicine one");
    msgs[0]["quiz_asked"] = json::array({std::to_string(a) + ":1", std::to_string(a) + ":3"});
    ChatSink s5;
    json q3 = chat(msgs, s5, nullptr);
    expect(q3.is_object() && q3["kind"] == "quiz" && q3["quiz_done"] == true && q3["not_found"] == true, "quiz: all used: says so");

    // Due: from reminders and tasks, no model, even with no model loaded.
    int64_t tid = sve_task_create(ctx, R"({"text":"Pay the walnut invoice","due_date":"2020-01-01"})");
    auto *hook = g_sve_model_reply;
    g_sve_model_reply = nullptr;
    ChatSink s6;
    json due = chat(ask("What's due soon?"), s6, nullptr);
    printf("--- due (no model)\n%s\n", due.dump(2).c_str());
    expect(tid > 0 && due.is_object() && due["kind"] == "due" && contains(due["answer"], "Pay the walnut invoice") && contains(due["answer"], "overdue") && s6.pieces == 0,
           "due: listed in code with no model");
    ChatSink s6b;
    expect(chat(ask("Which tablets this week?"), s6b, nullptr)["kind"] == "medicine", "medicine word beats a period");
    ChatSink s6c;
    // Medicine: runs with no model too, so the host answers it on the store context during other work.
    {
        char *rk = sve_route_kind(ctx, "Which tablet should I take?");
        expect(rk && !strcmp(rk, "medicine"), "route kind: a medicine question");
        sve_free_string(rk);
        ChatSink sm;
        json med = chat(ask("Which tablet should I take?"), sm, nullptr);
        expect(med.is_object() && med["kind"] == "medicine" && med["medicine"] == true && sm.pieces == 0, "medicine: built in code with no model");
    }
    // Nothing scores and no model: recordings, the not-found sentence; never general.
    json nomodel = chat(ask("Who wrote Gitanjali?"), s6c, nullptr);
    expect(nomodel.is_object() && nomodel["kind"] == "recordings" && nomodel["not_found"] == true, "no model: recordings, not general");
    g_sve_model_reply = hook;

    // General: nothing scores and the model says G: labelled, no sources; a medicine sentence left out.
    g_classify_reply = "G";
    g_chat_reply = "Rabindranath Tagore wrote it. Take 5 mg of rest. He won the Nobel Prize.";
    ChatSink s7;
    json gen = chat(ask("Who wrote Gitanjali?"), s7, nullptr);
    printf("--- general (no model)\n%s\n", gen.dump(2).c_str());
    expect(gen.is_object() && gen["kind"] == "recordings" && gen["not_found"] == true && contains(gen["answer"], "ask a doctor or pharmacist") &&
           s7.text == "Rabindranath Tagore wrote it.", "general: drifts into health: the code-built line; that sentence never shown");
    // ... in the question's language, exactly the fixed line.
    g_chat_reply = "రవీంద్రనాథ్ ఠాగూర్ రాశారు. జ్వరానికి paracetamol తీసుకోండి.";
    ChatSink s7t;
    json gent = chat(ask("గీతాంజలి ఎవరు రాశారు?"), s7t, nullptr);
    expect(gent.is_object() && gent["answer"] == sve::health_only_text("te") && gent["lang"] == "te" && gent["kind"] != "general" &&
               gent["medicine"] == false && s7t.text.find("paracetamol") == std::string::npos,
           "general drifts into health in Telugu: the Telugu line, that sentence never shown");
    g_chat_reply = "Rabindranath Tagore wrote it. He won the Nobel Prize.";
    ChatSink s7a;
    json gen2 = chat(ask("Who wrote Gitanjali?"), s7a, nullptr);
    expect(gen2.is_object() && gen2["kind"] == "general" && gen2["sources"].empty() && gen2["answer"].get<std::string>() == s7a.text &&
           s7a.text.find("Nobel") != std::string::npos, "general: streamed from the model's own knowledge, no sources");

    // Verified leaks: health questions never reach classify or general, whatever the model would say.
    g_classify_reply = "G";
    for (const char *h : {"What is paracetamol used for?", "What should I take for a headache?", "How do I bring down a fever?",
                          "జ్వరం తగ్గడానికి ఏం చేయాలి?"}) {
        g_chat_prompt.clear();
        ChatSink sh;
        json hj = chat(ask(h), sh, nullptr);
        std::string what = std::string("health question never general: ") + h;
        expect(hj.is_object() && hj["kind"] != "general" && sh.text.find("Tagore") == std::string::npos, what.c_str());
    }
    ChatSink shl;
    json hl = chat(ask("What should I take for a walnut headache tomorrow morning?"), shl, nullptr);
    expect(hl.is_object() && hl["not_found"] == true && hl["answer"] == sve::health_only_text("en"), "health question, nothing found: the code-built line");
    ChatSink shl2;
    json hl2 = chat(ask("జ్వరం తగ్గడానికి ఏం చేయాలి?"), shl2, nullptr);
    expect(hl2.is_object() && hl2["answer"] == sve::health_only_text("te") && hl2["lang"] == "te", "... in the question's language");
    // A follow-up of a medicine answer stays out of general.
    ChatSink sf;
    json fu = chat(json::array({{{"role", "user"}, {"text", "Which tablets?"}}, {{"role", "assistant"}, {"text", "Take Dolo 650."}, {"medicine", true}},
                                {{"role", "user"}, {"text", "Who wrote Gitanjali?"}}}), sf, nullptr);
    expect(fu.is_object() && fu["answer"] == sve::health_only_text("en"), "a follow-up of a medicine answer never goes to general");

    // The kind comes before the first piece, so a stopped general answer keeps its label.
    {
        struct Seen { std::vector<std::string> order; } seen;
        g_chat_reply = "Rabindranath Tagore wrote it. He won the Nobel Prize.";
        auto text_cb = [](const char *p, void *u) -> int { static_cast<Seen *>(u)->order.push_back(std::string("t:") + p); return 1; };
        auto prog_cb = [](const char *ph, int, int, void *u) -> int {
            if (!strncmp(ph, "kind:", 5)) static_cast<Seen *>(u)->order.push_back(ph);
            return 0;
        };
        char *got = sve_assistant_chat(ctx, ask("Who wrote Gitanjali?").dump().c_str(), nullptr, text_cb, &seen, prog_cb, &seen);
        expect(got == nullptr && seen.order.size() == 2 && seen.order[0] == "kind:general" && seen.order[1].rfind("t:", 0) == 0,
               "the kind comes before the first piece (and the stopped call still gave it)");
    }
    // Fix round 2: the reviewer's flows.
    {
        sve_ctx *c3 = sve_init(":memory:");
        int64_t n3 = sve_note_create(c3, "Plumbing call", "en", nullptr, "x", 60);
        sve_note_set_pieces(c3, n3, R"([{"start_sec":0,"text":"We used the old pipe and will bring it down tomorrow.","conf":0.9},
            {"start_sec":30,"text":"We will try again at the airport.","conf":0.9},
            {"start_sec":45,"text":"Ravi asked about the kitchen tap.","conf":0.9},
            {"start_sec":60,"text":"The tap also causes pain in the budget and the pipe bill.","conf":0.9}])");
        sve_note_set_output(c3, n3, SVE_OUT_SHORT_SUMMARY, "The team set the walnut budget.");
        auto go = [&](const json &msgs, const char *reply, const char *cls, ChatSink &sk) {
            g_chat_reply = reply;
            g_classify_reply = cls;
            g_chat_prompt.clear();
            return take(sve_assistant_chat(c3, msgs.dump().c_str(), nullptr, chat_text, &sk, nullptr, nullptr));
        };
        const std::string line = sve::health_only_text("en");
        ChatSink a1;
        json j = go(ask("How do I bring down a fever?"), "Rest and drink plenty of water [S1].", "G", a1);
        expect(j["answer"] == line && a1.pieces == 0 && g_chat_prompt.empty(), "junk hit on a health question: the health line, no model");
        ChatSink a2;
        j = go(ask("What is paracetamol used for?"), "Paracetamol is used for pain relief [S1].", "G", a2);
        expect(j["answer"] == line && !contains(j["answer"], "pipe") && a2.pieces == 0, "health question: never quotes a junk line");
        ChatSink a3;
        j = go(json::array({{{"role", "user"}, {"text", "What should I take for a headache?"}}, {{"role", "assistant"}, {"text", line}},
                            {{"role", "user"}, {"text", "And for a child of five?"}}}), "For a child, give half with water.", "G", a3);
        expect(j["answer"] == line && a3.pieces == 0 && g_chat_prompt.empty(), "a follow-up of the refusal: no classify, no general");
        for (const char *q : {"How do I treat a cold?", "జ్వరానికి ఏం చేయాలి?", "feverకి ఏం చేయాలి?", "What is Meftal used for?"}) {
            ChatSink a4;
            j = go(ask(q), "Rest and drink warm water.", "G", a4);
            std::string what = std::string("never general: ") + q;
            expect(j["kind"] != "general" && a4.pieces == 0 && j["not_found"] == true, what.c_str());
        }
        // Three-way classify: H gives the line, anything unparsable is recordings, never general.
        ChatSink b1;
        j = go(ask("Is my grandmother's walking ok for her age?"), "Maybe.", "H", b1);
        expect(j["answer"] == line && b1.pieces == 0 && g_chat_prompt.empty(), "classify H: the health line");
        for (const char *odd : {"Great", "G?maybe", "", "General knowledge"}) {
            ChatSink b2;
            j = go(ask("Who wrote Gitanjali?"), "Tagore.", odd, b2);
            std::string what = std::string("classify unparsable -> recordings: [") + odd + "]";
            expect(j["kind"] == "recordings" && j["not_found"] == true, what.c_str());
        }
        expect(sve::classify_prompt("q").find("H") != std::string::npos, "classify prompt is three-way");
        // Telugu: not held for "only".
        ChatSink c1;
        j = go(ask("భారతదేశ రాజధాని ఏది?"), "భారతదేశానికి న్యూ ఢిల్లీ మాత్రమే రాజధాని.", "G", c1);
        expect(j["kind"] == "general" && contains(j["answer"], "ఢిల్లీ") && c1.pieces == 1, "మాత్రమే does not hold a general answer");
        // The Telugu refusal line goes out as itself and is not a medicine answer.
        ChatSink c2;
        j = go(ask("జ్వరం తగ్గడానికి ఏం చేయాలి?"), "x", "G", c2);
        expect(j["answer"] == sve::health_only_text("te") && j["medicine"] == false, "the Telugu refusal is plain text");
        // Scoring.
        ChatSink d1;
        j = go(ask("What is AI?"), "AI means artificial intelligence.", "G", d1);
        expect(j["kind"] == "general", "What is AI? does not score on 'again' or 'airport'");
        ChatSink d2;
        j = go(ask("What did Ravi say today?"), "He asked about the tap [S1].", "G", d2);
        expect(j["kind"] == "recordings" && j["not_found"] == false, "period word stripped: Ravi today scores with no classify");
        // A flagged piece that does not score never reaches the model.
        ChatSink d3;
        j = go(ask("What did Ravi say about the kitchen tap?"), "He asked about the tap [S1].", "G", d3);
        expect(j["kind"] == "recordings" && g_chat_prompt.find("Ravi asked about the kitchen tap") != std::string::npos &&
               g_chat_prompt.find("causes pain") == std::string::npos, "a flagged piece that does not score is stripped from the material");
        // Ordinary questions are not refused by the gate.
        for (const char *q : {"What is a sales report?", "Who is Dr Ambedkar?", "How many times has India won the World Cup?"}) {
            ChatSink e1;
            j = go(ask(q), "An ordinary answer.", "G", e1);
            std::string what = std::string("not refused: ") + q;
            expect(j["kind"] == "general" && j["answer"] == "An ordinary answer.", what.c_str());
        }
        // A reminder with a medicine: a medicine turn when replayed.
        sve_reminder_create(c3, R"({"text":"Take crocin","times":["08:00"],"repeat":"daily"})");
        ChatSink f1;
        j = go(ask("What's due soon?"), "x", "G", f1);
        expect(j["kind"] == "due" && j["medicine"] == true && contains(j["answer"], "Take crocin"), "due with a health word is a medicine turn");
        ChatSink f2;
        j = go(ask("What is the deadline for the exam?"), "An ordinary answer.", "G", f2);
        expect(j["kind"] != "due", "deadline with a subject is not due");
        sve_free(c3);
    }
    g_classify_reply = "G";
    // Fix round 3: a health question never calls the model; quotes the passage or refuses.
    {
        const std::string line = sve::health_only_text("en");
        const json U1 = {{"role", "user"}, {"text", "What should I take for a headache?"}};
        const json RF = {{"role", "assistant"}, {"text", line}};
        const json RFH = {{"role", "assistant"}, {"text", line}, {"kind", "health"}};
        auto u = [](const char *t) { return json{{"role", "user"}, {"text", t}}; };
        struct Fix { const char *piece; json msgs; const char *reply; const char *what; };
        std::vector<Fix> fixes = {
            {"The doctor came at nine and we talked about the school.", ask("What did the doctor say?"), "The doctor said rest and drink warm water with honey [S1].", "doctor"},
            {"The child is five and sang a song.", json::array({U1, RF, u("And for a child of five?")}), "For a child of five, give half the amount with water [S1].", "child after a refusal"},
            {"My blood sugar level was high this morning.", ask("What is my blood sugar level?"), "It was high [S1]. Avoid rice and walk every morning [S1].", "blood sugar"},
            {"We will treat the cold room on Friday.", ask("How do I treat a cold?"), "Rest and gargle with warm salt water [S1].", "treat a cold"},
            {"Take the trip bags with you.", ask("What should I take on the trip?"), "Rest and drink warm water [S1].", "take on the trip"},
            {"రక్త పరీక్ష రిపోర్ట్ ఈ రోజు వచ్చింది.", ask("రక్త పరీక్ష రిపోర్ట్ ఏమిటి?"), "రిపోర్ట్ వచ్చింది, విశ్రాంతి తీసుకోండి [S1].", "te report"},
        };
        for (const auto &f : fixes) {
            sve_ctx *cf = sve_init(":memory:");
            int64_t nf = sve_note_create(cf, "Family", "en", nullptr, "x", 60);
            sve_note_set_pieces(cf, nf, json::array({{{"start_sec", 0}, {"text", f.piece}, {"conf", 0.9}}}).dump().c_str());
            g_chat_reply = f.reply;
            g_classify_reply = "G";
            g_model_calls = g_classify_calls = 0;
            ChatSink sk;
            json j = take(sve_assistant_chat(cf, f.msgs.dump().c_str(), nullptr, chat_text, &sk, nullptr, nullptr));
            std::string what = std::string("health: no model, the passage word for word: ") + f.what;
            expect(g_model_calls == 0 && g_classify_calls == 0 && sk.pieces == 0 && j.is_object() && j["medicine"] == true && j["kind"] == "medicine" &&
                   contains(j["answer"], f.piece) && j["sources"].size() == 1, what.c_str());
            sve_free(cf);
        }

        // Health turns are out of the prompt.
        sve_ctx *ch = sve_init(":memory:");
        int64_t nh = sve_note_create(ch, "Plumbing call", "en", nullptr, "x", 60);
        sve_note_set_pieces(ch, nh, R"([{"start_sec":0,"text":"We used the old pipe and will bring it down tomorrow.","conf":0.9}])");
        sve_note_set_output(ch, nh, SVE_OUT_SHORT_SUMMARY, "The team set the walnut budget.");
        auto clean_prompt = [&](const json &msgs, const char *reply, const char *cls) {
            g_chat_reply = reply;
            g_classify_reply = cls;
            g_chat_prompt.clear();
            g_model_calls = 0;
            ChatSink sk;
            json j = take(sve_assistant_chat(ch, msgs.dump().c_str(), nullptr, chat_text, &sk, nullptr, nullptr));
            for (const char *w : {"headache", "grandmother", "tablets", "Is it safe", "pharmacist", "fever"})
                if (g_chat_prompt.find(w) != std::string::npos) return false;
            return j.is_object();
        };
        expect(clean_prompt(json::array({U1, RF, u("And for a child of five?"), RF, u("What about an adult?")}), "Rest.", "G") && g_model_calls == 1 && g_chat_prompt.find("child of five") == std::string::npos,
               "a chain of refusals: the next question reaches the model with no health context");
        expect(clean_prompt(json::array({u("Is my grandmother's walking ok for her age?"), RF, u("And for a child of five?")}), "Rest.", "G"),
               "after a refusal with no flag: no health context");
        expect(clean_prompt(json::array({u("Which tablets do I take?"), {{"role", "assistant"}, {"text", "(quote)"}, {"medicine", true}}, u("Is it safe?"), RF, u("And for children?")}), "Rest.", "G"),
               "medicine, refusal, follow-up: no health context");
        expect(clean_prompt(json::array({U1, RF, u("And this week?")}), "- [Plumbing call] The walnut budget was set.", "G") && g_model_calls == 1,
               "a period follow-up: the summary, with no health context");
        // The flag on a classify-H refusal: the next turn is sticky, with no model.
        g_chat_reply = "x";
        g_classify_reply = "H";
        ChatSink sh1;
        json h1 = take(sve_assistant_chat(ch, ask("Is my grandmother's walking ok for her age?").dump().c_str(), nullptr, chat_text, &sh1, nullptr, nullptr));
        expect(h1.is_object() && h1["kind"] == "health" && h1["answer"] == line, "a classify-H refusal is flagged kind health");
        g_classify_reply = "G";
        g_model_calls = g_classify_calls = 0;
        ChatSink sh2;
        json h2 = take(sve_assistant_chat(ch, json::array({u("Is my grandmother's walking ok for her age?"), RFH, u("And for a child of five?")}).dump().c_str(),
                                          nullptr, chat_text, &sh2, nullptr, nullptr));
        expect(h2.is_object() && h2["answer"] == line && h2["kind"] != "health" && g_model_calls == 0 && g_classify_calls == 0,
               "after a classify-H refusal: one sticky turn, no model; not itself flagged, so the chain ends");
        ChatSink sh3;
        json h3 = take(sve_assistant_chat(ch, ask("What should I take for a headache?").dump().c_str(), nullptr, chat_text, &sh3, nullptr, nullptr));
        expect(h3.is_object() && h3["kind"] == "health", "a gate refusal is flagged kind health");
        // Before the summary route.
        g_model_calls = 0;
        ChatSink sh4;
        json h4 = take(sve_assistant_chat(ch, ask("What can I take today?").dump().c_str(), nullptr, chat_text, &sh4, nullptr, nullptr));
        expect(h4.is_object() && h4["kind"] != "summary" && g_model_calls == 0, "What can I take today? is not a summary");
        sve_free(ch);
        // A flagged title in a kept line.
        sve_ctx *ct = sve_init(":memory:");
        int64_t ntt = sve_note_create(ct, "Doctor visit", "en", nullptr, "x", 60);
        sve_note_set_pieces(ct, ntt, R"([{"start_sec":0,"text":"Ravi fixed the kitchen tap.","conf":0.9}])");
        g_chat_reply = "He fixed it [S1].";
        ChatSink stt;
        json tj = take(sve_assistant_chat(ct, ask("What did Ravi say about the kitchen tap?").dump().c_str(), nullptr, chat_text, &stt, nullptr, nullptr));
        expect(tj.is_object() && g_chat_prompt.find("Doctor") == std::string::npos && g_chat_prompt.find("Recording") != std::string::npos,
               "a flagged note title is a neutral label in the material");
        sve_free(ct);
    }
    g_classify_reply = "G";
    // sve_route_kind
    auto kind_of = [&](const char *q) { char *k = sve_route_kind(ctx, q); std::string r = k ? k : ""; sve_free_string(k); return r; };
    expect(kind_of("What's due soon?") == "due" && kind_of("Quiz me on my recent recordings") == "quiz" && kind_of("Which tablets?") == "medicine" &&
           kind_of("Who wrote Gitanjali?") == "recordings", "sve_route_kind");
    expect(sve_route_kind(ctx, nullptr) == nullptr && sve_route_kind(nullptr, "x") == nullptr, "sve_route_kind: bad input -> NULL");
    // A period named with a subject: only that period's notes.
    g_classify_reply = nullptr;
    g_chat_reply = "The walnut budget was set [S1].";
    ChatSink sr1, sr2;
    json inr = chat(ask("What did we say about the walnut budget today?"), sr1, nullptr);
    json outr = chat(ask("What did we say about the walnut budget last week?"), sr2, nullptr);
    expect(inr.is_object() && inr["kind"] == "recordings" && inr["not_found"] == false, "a subject and a period: recordings within the period");
    expect(outr.is_object() && outr["kind"] == "recordings" && outr["not_found"] == true, "... nothing in another period");
    // Every note with nothing stored: the titles, no model call.
    {
        sve_ctx *c2 = sve_init(":memory:");
        int64_t n1 = sve_note_create(c2, "Lecture one", "en", nullptr, "x", 60);
        (void)n1;
        sve_note_create(c2, "Doctor visit", "en", nullptr, "x", 60);
        g_chat_prompt.clear();
        ChatSink st;
        json tl = take(sve_assistant_chat(c2, ask("What happened today?").dump().c_str(), nullptr, chat_text, &st, nullptr, nullptr));
        expect(tl.is_object() && tl["kind"] == "summary" && contains(tl["answer"], "Lecture one") && !contains(tl["answer"], "Doctor visit") &&
               g_chat_prompt.empty() && tl["sources"].size() == 2, "summary: nothing stored: the titles in code, no model; a health title left out");
        sve_free(c2);
    }
    g_classify_reply = "G";
    ChatSink s8;
    expect(chat(ask("Which medicine helps with sleep?"), s8, nullptr)["kind"] == "medicine", "medicine is never general, even when the model would say G");
    // A question that scores never asks the model.
    g_classify_reply = "G";
    g_chat_reply = "The walnut budget was set [S1].";
    ChatSink s9;
    json rec = chat(ask("What about the walnut budget"), s9, nullptr);
    expect(rec.is_object() && rec["kind"] == "recordings" && rec["not_found"] == false && !rec["sources"].empty() && rec["sources"][0]["note_id"] == a,
           "recordings: found through a stored summary (no transcript has those words), with its link");
    // The model says R: recordings.
    g_classify_reply = "R";
    ChatSink s10;
    json r2 = chat(ask("Who wrote Gitanjali?"), s10, nullptr);
    expect(r2.is_object() && r2["kind"] == "recordings" && r2["not_found"] == true, "classified R, nothing found: the not-found sentence");
    g_classify_reply = nullptr;
    sve_task_set_done(ctx, tid, 1);
}

static void run_api_chat(sve_ctx *ctx) {
    auto *saved = g_sve_model_reply;
    g_sve_model_reply = chat_model;
    sve_profile_set(ctx, R"({"name":"Lakshmi","lang":"te"})");   // the profile is Telugu; the questions decide
    json pieces = json::parse(R"([{"start_sec":0,"text":"The plumber said the kitchen pipe must be replaced soon."},
        {"start_sec":30,"text":"Ravi will send the quotation for the kitchen pipe by Friday evening."}])");
    make_note(ctx, pieces, "Kitchen repair");
    auto chat = [&](const json &msgs, ChatSink &sink, const char *hint = nullptr) {
        g_chat_prompt.clear();
        return take(sve_assistant_chat(ctx, msgs.dump().c_str(), hint, chat_text, &sink, nullptr, nullptr));
    };
    const json q1 = {{"role", "user"}, {"text", "What did the plumber say about the kitchen pipe?"}};

    g_chat_reply = "<|channel>thought\nThe material has it.<channel|>The kitchen pipe must be replaced soon [S1]. "
                   "Ravi will send the quotation by Friday evening [S1].";
    ChatSink s1;
    json a = chat(json::array({q1}), s1);
    printf("--- chat (no model)\n%s\n", a.dump(2).c_str());
    expect(a.is_object() && a["kind"] == "recordings" && a["lang"] == "en" && a["not_found"] == false && a["medicine"] == false &&
           !a["sources"].empty(), "chat: a recordings answer in English to an English question (profile Telugu)");
    expect(a.is_object() && s1.text == a["answer"].get<std::string>() && s1.pieces >= 2,
           "chat: the streamed pieces join to the answer, a sentence at a time");
    expect(s1.text.find("[S") == std::string::npos && s1.text.find("thought") == std::string::npos,
           "chat: no labels or thinking streamed");
    expect(g_chat_prompt.find("in English") != std::string::npos && g_chat_prompt.find("Conversation so far") == std::string::npos,
           "chat: the prompt asks for English; no history for a first question");

    // A follow-up: earlier turns in the prompt, sources kept as text, a medicine answer left out; found through the
    // earlier question's words.
    ChatSink s2;
    json msgs = json::array({{{"role", "user"}, {"text", "Which tablet was it?"}},
        {{"role", "assistant"}, {"text", "Take Dolo 650 twice."}, {"medicine", true}}, q1,
        {{"role", "assistant"}, {"text", "The pipe must be replaced soon."}, {"sources", json::array({{{"title", "Kitchen repair"}, {"start_sec", 30}}})}},
        {{"role", "user"}, {"text", "And when?"}}});
    json b = chat(msgs, s2);
    expect(b.is_object() && b["kind"] == "recordings" && b["not_found"] == false, "chat: a short follow-up is answered from the earlier question's words");
    expect(g_chat_prompt.find("Person: What did the plumber say") != std::string::npos &&
           g_chat_prompt.find("(Sources: Kitchen repair at 0:30)") != std::string::npos &&
           g_chat_prompt.find("Dolo") == std::string::npos && g_chat_prompt.find("Question: And when?") != std::string::npos,
           "chat: history in the prompt with its sources as text; the medicine answer is not");

    // Telugu question: answered in Telugu (here: nothing in the recordings), the not-found sentence never streamed.
    ChatSink s3;
    json c = chat(json::array({{{"role", "user"}, {"text", "వంటగది పైపు గురించి ప్లంబర్ ఏమన్నారు?"}}}), s3);
    expect(c.is_object() && c["lang"] == "te" && c["not_found"] == true && s3.pieces == 0, "chat: a Telugu question gets Telugu; not-found not streamed");
    ChatSink s3b;
    json c2 = chat(json::array({{{"role", "user"}, {"text", "?"}}}), s3b, "te");
    expect(c2.is_object() && c2["lang"] == "te", "chat: no letters: the hint's language");

    // Cancel through on_text: NULL.
    ChatSink s4;
    s4.stop_after = 1;
    expect(sve_assistant_chat(ctx, json::array({q1}).dump().c_str(), nullptr, chat_text, &s4, nullptr, nullptr) == nullptr && s4.pieces == 1,
           "chat: on_text returning non-zero cancels");

    // Medicine: answered in code, nothing streamed, no model call.
    ChatSink s5;
    json d = chat(json::array({{{"role", "user"}, {"text", "Which tablets do I take?"}}}), s5);
    expect(d.is_object() && d["kind"] == "medicine" && d["medicine"] == true && s5.pieces == 0 && g_chat_prompt.empty(),
           "chat: a medicine question is answered in code, never streamed");

    // The model drifts into a medicine: that sentence is never shown; the answer is the cited words verbatim.
    g_chat_reply = "The kitchen pipe must be replaced soon [S1]. Take 5 mg after it [S1].";
    ChatSink s6;
    json e = chat(json::array({q1}), s6);
    expect(e.is_object() && e["kind"] == "medicine" && e["medicine"] == true && s6.text == "The kitchen pipe must be replaced soon." &&
           e["answer"].get<std::string>().find("5 mg") == std::string::npos,
           "chat: a medicine sentence from the model is held back; the answer is quoted");

    g_chat_reply = "NOT_FOUND";
    ChatSink s7;
    json f = chat(json::array({q1}), s7);
    expect(f.is_object() && f["not_found"] == false && s7.pieces == 0 && f["lang"] == "en" && contains(f["answer"], "kitchen pipe") &&
           f["sources"].size() == 1, "chat: NOT_FOUND is never streamed; something scores, so the closest passage word for word");

    // Held at a medicine sentence: generation stops there (the sentence's label is in, a later one is not).
    g_chat_reply = "The kitchen pipe must be replaced soon [S1]. Take 5 mg after it [S1]. Ravi sends the quote [S2].";
    ChatSink s7c;
    json held = chat(json::array({q1}), s7c);
    expect(held.is_object() && held["medicine"] == true && held["sources"].size() == 1 && s7c.text == "The kitchen pipe must be replaced soon.",
           "chat: held at the medicine sentence, generation stops there");
    run_api_route(ctx, chat);

    // The insight: a health sentence is dropped, not only a medicine one (the model's hook stands in).
    {
        sve_setting_set(ctx, "insight.last_date", "");
        for (const char *bad : {"You keep mentioning a fever and a sore throat. Rest and see how it goes.",
                                "Take your tablets on time.", "జ్వరం గురించి మళ్ళీ చెప్పారు."}) {
            g_chat_reply = bad;
            json dropped = take(sve_insight_generate(ctx, 0, nullptr, nullptr));
            expect(dropped.is_null(), (std::string("insight dropped: ") + bad).c_str());
        }
        char *date = sve_setting_get(ctx, "insight.last_date");
        expect(!date || !*date, "a dropped insight does not use up the day");
        sve_free_string(date);
        g_chat_reply = "The kitchen pipe keeps coming up. Ravi's quote is due on Friday.";
        json kept = take(sve_insight_generate(ctx, 0, nullptr, nullptr));
        expect(kept.is_object() && kept["text"] == g_chat_reply, "an ordinary insight stays");
        if (kept.is_object()) sve_insight_dismiss(ctx, kept["id"].get<int64_t>());
        sve_setting_set(ctx, "insight.last_date", "");
    }

    ChatSink s8;
    expect(sve_assistant_chat(ctx, "[]", nullptr, chat_text, &s8, nullptr, nullptr) == nullptr &&
           sve_assistant_chat(ctx, R"([{"role":"assistant","text":"hi"}])", nullptr, nullptr, nullptr, nullptr, nullptr) == nullptr &&
           sve_assistant_chat(ctx, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr) == nullptr &&
           sve_assistant_chat(nullptr, "[]", nullptr, nullptr, nullptr, nullptr, nullptr) == nullptr,
           "chat: no question last -> NULL");
    g_sve_model_reply = nullptr;
    expect(sve_assistant_chat(ctx, json::array({q1}).dump().c_str(), nullptr, nullptr, nullptr, nullptr, nullptr) == nullptr,
           "chat: no model -> NULL");
    g_sve_model_reply = saved;
    sve_profile_set(ctx, R"({"name":"Lakshmi","lang":"en"})");
}

static int run_api(sve_ctx *ctx) {
    // Runtime CPU threads (the heat ladder): nothing to set without a model.
    expect(sve_set_threads(ctx, 2, 2) == -1 && sve_set_threads(nullptr, 2, 2) == -1, "set threads with no model: -1");
    // The audio encoder's threads: nothing to load again without a model and projector.
    expect(sve_set_audio_threads(ctx, 2) == -1 && sve_set_audio_threads(nullptr, 2) == -1 && sve_set_audio_threads(ctx, 0) == -1,
           "set audio threads with no model: -1");

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
    {   // N7 rev 1: the notes material marks pieces heard with conf < 0.80; no conf is not marked.
        char *m = sve_notes_material(R"([{"start_sec":0,"text":"Clear.","conf":0.9},{"start_sec":28,"text":"Mumble.","conf":0.6},{"start_sec":56,"text":"Old."}])");
        expect(m && std::string(m) == "Clear.\n[unclear] Mumble.\nOld.", "notes material marks the unclear piece only");
        sve_free_string(m);
        expect(sve_notes_material("{}") == nullptr && sve_notes_material(nullptr) == nullptr, "notes material needs an array");
    }
    json np = take(sve_note_pieces(ctx, note));
    expect(np.size() == 2 && np[1]["start_sec"] == 28, "pieces round trip");
    expect(take(sve_insight_latest(ctx)).is_null(), "no insight yet");
    expect(sve_extract(ctx, note, nullptr, nullptr) < 0, "extract without a model is an error");
    expect(sve_topic_summary(ctx, 12345, "en", nullptr, nullptr) == nullptr, "unknown topic -> NULL");
    run_api_fixes(ctx);
    run_api_direct(ctx);
    run_api_times(ctx);
    run_api_final(ctx);
    run_api_memories(ctx);
    run_api_chat(ctx);
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
        if (stored) stored->push_back({{"idx", (int)k}, {"first_piece", (int)secs[k].first}, {"last_piece", (int)secs[k].second - 1},
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

// Transcribes SVE_TEST_WAV into pieces, writes the three note kinds from the transcript, asks one question.
static int run_notes(sve_ctx *ctx, const char *wav, const char *lang, const char *question) {
    json pieces = take(sve_transcribe_audio_pieces(ctx, wav, nullptr, nullptr));
    if (!pieces.is_array() || pieces.empty()) { fprintf(stderr, "no pieces\n"); return 1; }
    {   // conf goes to stderr so stdout stays comparable across versions
        json shown = pieces;
        for (auto &p : shown) { fprintf(stderr, "conf %s\n", p.value("conf", json()).dump().c_str()); p.erase("conf"); }
        printf("--- pieces\n%s\n", shown.dump(2).c_str());
    }
    std::string transcript;
    for (const auto &p : pieces) transcript += p["text"].get<std::string>() + "\n";
    const char *names[] = {"short summary", "detailed notes", "revision questions"};
    for (int k = 0; k < 3; ++k) {
        char *o = sve_generate(ctx, transcript.c_str(), (sve_output_kind)k, lang, nullptr, nullptr);
        printf("--- %s\n%s\n", names[k], o ? o : "(NULL)");
        sve_free_string(o);
    }
    int64_t id = sve_note_create(ctx, "test note", lang, nullptr, transcript.c_str(), 60);
    sve_note_set_pieces(ctx, id, pieces.dump().c_str());
    json a = take(sve_assistant_ask(ctx, question, nullptr, nullptr));
    printf("--- ask %s\n%s\n", question, a.dump(2).c_str());
    return 0;
}

// sve_transcript_fits against what sve_generate does: the first model phase is "final" (whole transcript in one
// prompt) when it says 1 and "chunk" (summarised first) when it says 0. The callback cancels at once, so each
// generate call costs one prefill step.
static int first_phase_cancel(const char *phase, int, int, void *user) {
    auto *seen = static_cast<std::string *>(user);
    if (seen->empty()) *seen = phase;
    return 1;
}

static std::string first_phase(sve_ctx *ctx, const std::string &text, const char *lang) {
    std::string seen;
    char *o = sve_generate(ctx, text.c_str(), SVE_OUT_SHORT_SUMMARY, lang, first_phase_cancel, &seen);
    sve_free_string(o);
    return seen;
}

static std::string slurp(const char *path) {
    std::ifstream f(path);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

static int run_fits(sve_ctx *ctx, const char *lang) {
    const std::string fx = slurp(SVE_FIXTURE_DIR "/doctor_visit.txt");
    expect(!fx.empty(), "fixture read");
    expect(sve_transcript_fits(ctx, fx.c_str(), lang) == 1, "short fixture fits one prompt");
    expect(first_phase(ctx, fx, lang) == "final", "short fixture: sve_generate goes straight to the final prompt");
    expect(sve_transcript_fits(ctx, "", lang) == 1, "empty transcript fits (sve_generate returns \"\")");
    expect(sve_transcript_fits(ctx, nullptr, lang) == -1, "NULL transcript is -1");
    // Grow the fixture until it no longer fits; the last fitting and first non-fitting must match sve_generate.
    std::string text, last_fit, first_over;
    for (int i = 0; i < 400 && first_over.empty(); ++i) {
        std::string next = text + fx;
        if (sve_transcript_fits(ctx, next.c_str(), lang) == 1) text = next;
        else {
            // refine to the line: add the fixture's lines one at a time
            size_t pos = 0;
            while (pos < fx.size()) {
                size_t nl = fx.find('\n', pos);
                size_t end = nl == std::string::npos ? fx.size() : nl + 1;
                std::string cand = text + fx.substr(pos, end - pos);
                if (sve_transcript_fits(ctx, cand.c_str(), lang) == 1) text = cand;
                else { first_over = cand; break; }
                pos = end;
            }
        }
    }
    last_fit = text;
    printf("fits: last fitting %zu bytes, first over %zu bytes\n", last_fit.size(), first_over.size());
    expect(!first_over.empty(), "a long transcript does not fit");
    expect(first_phase(ctx, last_fit, lang) == "final", "largest fitting transcript: no chunking");
    expect(first_phase(ctx, first_over, lang) == "chunk", "one line more: sve_generate chunks it");
    std::string two_h;
    for (int i = 0; i < 50; ++i) two_h += fx;   // well past the budget
    expect(sve_transcript_fits(ctx, two_h.c_str(), lang) == 0, "50x the fixture does not fit");
    return 0;
}

// Memories on a copy of a notes database (SVE_DB; never the phone's original): writes the sections a note is
// missing with sve_section_summarize, then sve_memories_from_section for every due section. Drop reasons go
// to a table memory_drops(note_id, section_idx, reason, cand_json) that only this mode creates. Optional seeding
// before that: SVE_SEED_FIXTURE ("[t=SEC] text" file, an English note) and SVE_SEED_WAV (transcribed into a
// Telugu note). SVE_NOTES: "all" (default) or comma-separated note ids. CPU, 4 threads, as in the background.
// Per section: the (unchanged) summary call's seconds when it had to be written, and sve_memories_from_section's
// seconds (the memory-only call and the double-checks: the extra time memories cost) and whether its reply was cut.
static sqlite3 *g_drops = nullptr;
static std::map<std::string, int> g_drop_count;
static void smoke_drop(int64_t note, int idx, const char *reason, const char *cand) {
    ++g_drop_count[reason];
    sqlite3_stmt *st = nullptr;
    if (sqlite3_prepare_v2(g_drops, "INSERT INTO memory_drops(note_id,section_idx,reason,cand_json) VALUES(?,?,?,?)", -1, &st,
                           nullptr) != SQLITE_OK) return;
    sqlite3_bind_int64(st, 1, note);
    sqlite3_bind_int(st, 2, idx);
    sqlite3_bind_text(st, 3, reason, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 4, cand, -1, SQLITE_TRANSIENT);
    sqlite3_step(st);
    sqlite3_finalize(st);
}

static double seconds_since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

// Core log lines go to stderr as usual; a memory reply cut off at the cap is also counted for the report.
static int g_cut_replies = 0;
static void smoke_log(int, const char *msg, void *) {
    fprintf(stderr, "[sve] %s\n", msg);
    if (strstr(msg, "memories: reply cut off")) ++g_cut_replies;
}

static int run_memories(sve_ctx *ctx, const char *db_path) {
    if (sqlite3_open(db_path, &g_drops) != SQLITE_OK) { fprintf(stderr, "cannot open %s\n", db_path); return 1; }
    sqlite3_busy_timeout(g_drops, 5000);
    sqlite3_exec(g_drops, "CREATE TABLE IF NOT EXISTS memory_drops(note_id INTEGER, section_idx INTEGER, reason TEXT, cand_json TEXT)",
                 nullptr, nullptr, nullptr);
    if (const char *fx = getenv("SVE_SEED_FIXTURE")) {
        json pieces = read_fixture(fx);
        int64_t id = make_note(ctx, pieces, "Doctor visit (fixture)");
        printf("seeded note %lld from %s (%zu pieces)\n", (long long)id, fx, pieces.size());
    }
    if (const char *wav = getenv("SVE_SEED_WAV")) {
        json pieces = take(sve_transcribe_audio_pieces(ctx, wav, nullptr, nullptr));
        if (!pieces.is_array() || pieces.empty()) { fprintf(stderr, "no pieces from %s\n", wav); return 1; }
        std::string transcript;
        for (const auto &p : pieces) transcript += p["text"].get<std::string>() + "\n";
        std::ifstream f(wav, std::ios::binary | std::ios::ate);
        int64_t dur = f.tellg() > 44 ? (int64_t)((f.tellg() - (std::streamoff)44) / 32000) : 0;
        int64_t id = sve_note_create(ctx, "Telugu reference", "te", nullptr, transcript.c_str(), dur);
        sve_note_set_pieces(ctx, id, pieces.dump().c_str());
        printf("seeded note %lld from %s (%zu pieces, %lld s)\n", (long long)id, wav, pieces.size(), (long long)dur);
    }
    std::vector<int64_t> ids;
    const std::string which = getenv("SVE_NOTES") ? getenv("SVE_NOTES") : "all";
    if (which == "all") {
        for (const auto &n : take(sve_notes_list_json(ctx, "", 100000))) ids.push_back(n["id"].get<int64_t>());
        std::reverse(ids.begin(), ids.end());  // oldest first
    } else {
        for (size_t at = 0; at < which.size();) {
            size_t c = which.find(',', at);
            ids.push_back(atoll(which.substr(at, c == std::string::npos ? std::string::npos : c - at).c_str()));
            at = c == std::string::npos ? which.size() : c + 1;
        }
    }
    int total_shown = 0, total_queued = 0, total_dropped = 0;
    for (int64_t id : ids) {
        auto note = ctx->store->get(id);
        json pieces = ctx->store->pieces(id);
        if (!note || pieces.empty()) { printf("=== note %lld: no pieces, skipped\n", (long long)id); continue; }
        const std::string lang = note->lang == "te" ? "te" : "en";
        printf("=== note %lld \"%s\" (%s, %zu pieces)\n", (long long)id, note->title.c_str(), lang.c_str(), pieces.size());
        json secs = ctx->store->sections(id);
        std::map<int64_t, std::string> timing;  // idx -> summary timing text
        if (secs.empty()) {
            int64_t end = std::max<int64_t>(note->duration_seconds, pieces.back()["start_sec"].get<int64_t>() + 1);
            json ranges = take(sve_plan_sections(pieces.dump().c_str(), end, 1));
            for (size_t k = 0; k < ranges.size(); ++k) {
                size_t a = ranges[k][0], b = ranges[k][1];
                json arr = json::array();
                sve::SectionPieces sp;
                for (size_t i = a; i < b; ++i) {
                    arr.push_back({{"start_sec", pieces[i]["start_sec"]}, {"text", pieces[i]["text"]}});
                    if (pieces[i]["text"].get<std::string>().find_first_not_of(" \t\r\n") != std::string::npos)
                        sp.push_back({pieces[i]["start_sec"].get<int64_t>(), pieces[i]["text"].get<std::string>()});
                }
                auto t0 = std::chrono::steady_clock::now();
                json sm = take(sve_section_summarize(ctx, arr.dump().c_str(), lang.c_str(), nullptr, nullptr));
                char b2[96];
                snprintf(b2, sizeof b2, "summary %.1f s (unchanged call)", seconds_since(t0));
                timing[(int64_t)k] = b2;
                printf("section %zu summary: %s\n", k, sm.dump().c_str());
                expect(sm.is_object() && sm.contains("title"), "section summary written");
                if (sm.value("fallback", false)) printf("(fallback used)\n");
                int64_t s0 = pieces[a]["start_sec"], s1 = b < pieces.size() ? pieces[b]["start_sec"].get<int64_t>() : end;
                secs.push_back({{"idx", (int)k}, {"first_piece", (int)a}, {"last_piece", (int)b - 1}, {"start_sec", s0}, {"end_sec", s1},
                                {"summary", sm}});
            }
            expect(sve_note_set_sections(ctx, id, secs.dump().c_str()) == 0, "sections stored");
        }
        g_drop_count.clear();
        for (const auto &idx : take(sve_memory_sections_due(ctx, id))) {
            auto t0 = std::chrono::steady_clock::now();
            const int cut_before = g_cut_replies;
            int n = sve_memories_from_section(ctx, id, idx.get<int>(), nullptr, nullptr);
            double s = seconds_since(t0);
            auto it = timing.find(idx.get<int64_t>());
            printf("--- section %d: %d stored; memories %.1f s, reply %s%s%s\n", idx.get<int>(), n, s,
                   g_cut_replies > cut_before ? "cut off (whole items kept)" : "complete", it == timing.end() ? "" : "; ",
                   it == timing.end() ? "" : it->second.c_str());
            expect(n >= 0, "memories from section");
        }
        int shown = 0, queued = 0, dropped = 0;
        json mems = take(sve_memories_json(ctx, (R"({"note_id":)" + std::to_string(id) + "}").c_str()));
        for (const auto &m : mems) {
            (m["queue"] == 1 ? queued : shown)++;
            printf("  [%s %s%s] %s\n      quote: %s\n", m["queue"] == 1 ? "queued" : "shown", m["type"].get<std::string>().c_str(),
                   m["health"] == 1 ? ", health" : "", m["text"].get<std::string>().c_str(),
                   m["sources"].empty() ? "" : m["sources"][0]["quote"].get<std::string>().c_str());
        }
        std::string reasons;
        for (const auto &r : g_drop_count) { dropped += r.second; reasons += (reasons.empty() ? "" : " ") + r.first + ":" + std::to_string(r.second); }
        printf("memories: %d shown, %d queued, %d dropped (%s)\n", shown, queued, dropped, reasons.c_str());
        total_shown += shown; total_queued += queued; total_dropped += dropped;
    }
    printf("TOTAL memories: %d shown, %d queued, %d dropped\n", total_shown, total_queued, total_dropped);
    sqlite3_close(g_drops);
    return 0;
}

// ---------------------------------------------------------------- meaning search (Task 11)

// Pieces (note_id, start_sec) of sve_search_json, at most n, in its order.
static json search_pieces_of(sve_ctx *ctx, const std::string &q, size_t n) {
    json out = json::array();
    char *s = sve_search_json(ctx, q.c_str(), 60);
    json hits = s ? json::parse(s, nullptr, false) : json::array();
    sve_free_string(s);
    if (!hits.is_array()) return out;
    for (const auto &h : hits) {
        if (out.size() >= n) break;
        if (h.value("kind", "") == "piece") out.push_back({h["note_id"], h["start_sec"]});
    }
    return out;
}

static double peak_rss_mb() {
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    return ru.ru_maxrss / (1024.0 * 1024.0);  // bytes on macOS
}

// Embeds every pending piece and memory (CPU, 4 threads); fills the time per item and the count.
static bool embed_all(sve_ctx *ctx, double &sec_per_item, int &count) {
    count = 0;
    auto t0 = std::chrono::steady_clock::now();
    for (;;) {
        int n = sve_embed_pending(ctx, 64, nullptr, nullptr);
        // -1: no embedder, an error, or a batch where nothing could be embedded (broken embedder): stop.
        if (n < 0) { fprintf(stderr, "sve_embed_pending failed (%d); stopped\n", n); return false; }
        if (n == 0) break;
        count += n;
        printf("embedded %d\n", count);
        fflush(stdout);
    }
    double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    sec_per_item = count ? sec / count : 0;
    return true;
}

// embed: SVE_DB (a copy), SVE_EMBEDDER. search: also SVE_QUESTION ("a|b" for several): top 5 pieces, keyword only and
// keyword + meaning. gate: SVE_QUESTIONS (jsonl), SVE_RESULTS (out); SVE_GEMMA optional, loaded too for the memory
// figure. Writes the results file tools/memory/meaning_gate.py run --results scores.
static int run_meaning(sve_ctx *ctx, const char *mode, const char *db) {
    const char *emb = getenv("SVE_EMBEDDER");
    if (!emb) { fprintf(stderr, "set SVE_EMBEDDER (an EmbeddingGemma GGUF)\n"); return 2; }
    std::vector<std::string> questions;
    std::vector<json> rows;
    if (!strcmp(mode, "search")) {
        const char *q = getenv("SVE_QUESTION");
        std::string all = q ? q : "";
        for (size_t a = 0; a <= all.size();) {
            size_t b = all.find('|', a);
            if (b == std::string::npos) b = all.size();
            if (b > a) questions.push_back(all.substr(a, b - a));
            a = b + 1;
        }
        if (questions.empty()) { fprintf(stderr, "set SVE_QUESTION\n"); return 2; }
    } else if (!strcmp(mode, "gate")) {
        const char *qf = getenv("SVE_QUESTIONS");
        if (!qf || !getenv("SVE_RESULTS")) { fprintf(stderr, "set SVE_QUESTIONS and SVE_RESULTS\n"); return 2; }
        std::ifstream in(qf);
        for (std::string line; std::getline(in, line);) {
            json j = json::parse(line, nullptr, false);
            if (j.is_object() && j.contains("q")) { questions.push_back(j["q"].get<std::string>()); rows.push_back(j); }
        }
        if (questions.empty()) { fprintf(stderr, "no questions in %s\n", qf); return 2; }
    }
    // Keyword only first, before any embedder is loaded: today's search.
    std::vector<json> keyword;
    for (const auto &q : questions) keyword.push_back(search_pieces_of(ctx, q, 5));
    if (const char *g = getenv("SVE_GEMMA"); g && !strcmp(mode, "gate"))
        if (sve_load_llm(ctx, g, 8192, 4, 0) != 0) { fprintf(stderr, "sve_load_llm failed\n"); return 1; }
    if (sve_load_embedder(ctx, emb, 4) != 0) { fprintf(stderr, "sve_load_embedder failed\n"); return 1; }
    double spp = 0;
    int count = 0;
    if (!embed_all(ctx, spp, count)) return 1;
    sqlite3 *raw = nullptr;
    int64_t index_bytes = 0, vectors = 0;
    if (sqlite3_open_v2(db, &raw, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK) {
        sqlite3_stmt *st = nullptr;
        if (sqlite3_prepare_v2(raw, "SELECT COUNT(*), COALESCE(SUM(length(vec)),0) FROM embeddings", -1, &st, nullptr) == SQLITE_OK &&
            sqlite3_step(st) == SQLITE_ROW) {
            vectors = sqlite3_column_int64(st, 0);
            index_bytes = sqlite3_column_int64(st, 1);
        }
        sqlite3_finalize(st);
    }
    sqlite3_close(raw);
    printf("embedded %d now (%.3f s each), %lld vectors, %lld bytes of vectors, peak memory %.0f MB\n", count, spp,
           (long long)vectors, (long long)index_bytes, peak_rss_mb());
    if (!strcmp(mode, "embed")) return 0;
    std::vector<json> hybrid;
    for (const auto &q : questions) hybrid.push_back(search_pieces_of(ctx, q, 5));
    if (!strcmp(mode, "search")) {
        for (size_t i = 0; i < questions.size(); i++)
            printf("Q: %s\n  keyword: %s\n  hybrid:  %s\n", questions[i].c_str(), keyword[i].dump().c_str(), hybrid[i].dump().c_str());
        return 0;
    }
    std::ofstream out(getenv("SVE_RESULTS"));
    const char *slash = strrchr(emb, '/');
    const char *qsha = getenv("SVE_QUESTIONS_SHA256");  // meaning_gate.py run sets it
    out << json({{"meta", {{"embedder", slash ? slash + 1 : emb}, {"where", "mac"}, {"questions_sha256", qsha ? qsha : ""}, {"embed_sec_per_item", spp},
                           {"embedded", count}, {"vectors", vectors}, {"index_bytes", index_bytes},
                           {"peak_rss_mb", peak_rss_mb()}, {"with_gemma", getenv("SVE_GEMMA") != nullptr}}}})
               .dump()
        << "\n";
    for (size_t i = 0; i < questions.size(); i++)
        out << json({{"i", i}, {"q", questions[i]}, {"keyword", keyword[i]}, {"hybrid", hybrid[i]}}).dump() << "\n";
    printf("wrote %zu results to %s\n", questions.size(), getenv("SVE_RESULTS"));
    return 0;
}


// The orchestrator's acceptance test (review 2026-11-25, "the Priya example"): one sentence, every output right but
// the map's relationship, which says the budget was approved while the recording says approval is pending. The check
// is interrupted (the host cancels at its first model call), the app is closed and opened again (a new context on
// the same file), and the check runs again: only then does the note get its verdict, with the wrong relationship
// flagged (meaning), its memory queued for review, and the relations review showing why and where.
static void run_priya(void) {
    char path[] = "/tmp/sve_priya_XXXXXX";
    int fd = mkstemp(path);
    if (fd >= 0) close(fd);
    remove(path);
    sve_ctx *ctx = sve_init(path);
    expect(ctx != nullptr, "priya: context on a file");
    if (!ctx) return;
    const json pieces = json::array({{{"start_sec", 0}, {"text", "Ravi: Priya will review the budget on Friday. Approval is still pending."}}});
    const int64_t note = make_note(ctx, pieces, "Budget");
    sve_note_set_output(ctx, note, SVE_OUT_SHORT_SUMMARY, "Budget approval is pending. Priya will review it on Friday.");
    sve_note_set_output(ctx, note, SVE_OUT_DETAILED_NOTES, "## Action items\n- [ ] Review the budget — Priya — Friday\n");
    sve_note_set_output(ctx, note, SVE_OUT_REVISION_QUESTIONS, "Q1. What will Priya do on Friday?\nA1. Review the budget.");
    const int64_t wrong = ctx->store->add_memory_item({{"type", "decision"}, {"text", "Priya approved the budget"}, {"who", "Priya"}});
    const int64_t right = ctx->store->add_memory_item({{"type", "promise"}, {"text", "Priya will review the budget on Friday"}, {"who", "Priya"}});
    ctx->store->add_memory_source(wrong, note, 0, "Approval is still pending.", std::nullopt);
    ctx->store->add_memory_source(right, note, 0, "Priya will review the budget on Friday.", std::nullopt);
    expect(sve_relations_refresh(ctx, note) >= 0, "priya: the map's relationships derived");

    // The judge: "no" to every check (so only code and the evidence decide what passes).
    g_sve_model_reply = [](const char *phase, const char *) -> const char * { return !strncmp(phase, "verify", 6) ? "no" : "Yes"; };
    const std::string key = "verify." + std::to_string(note);
    // 1. Interrupted at its first model call: nothing marked, nothing stored, the check stays owed.
    int asked = 0;
    json r1 = take(sve_verify_note(ctx, note, [](const char *, int, int, void *u) { return ++*static_cast<int *>(u) >= 1 ? 1 : 0; }, &asked));
    expect(r1.is_object() && r1.value("cancelled", false), "priya: the interrupted check reports cancelled");
    expect(!ctx->store->setting_get(key), "priya: an interrupted check stores no verdict");
    expect(take(sve_memory_get(ctx, wrong)).value("queue", 0) == 0, "priya: nothing flagged by the interrupted check");
    sve_free(ctx);

    // 2. The app opens again: the owed check runs on the stored outputs, without writing them again.
    ctx = sve_init(path);
    expect(ctx != nullptr, "priya: reopened");
    if (!ctx) { g_sve_model_reply = nullptr; return; }
    json r2 = take(sve_verify_note(ctx, note, nullptr, nullptr));
    expect(r2.is_object() && !r2.value("cancelled", false), "priya: the resumed check finishes");
    bool flagged = false, right_flagged = false;
    for (const auto &f : r2.value("relation_flags", json::array())) {
        if (f.value("memory_id", (int64_t)0) == wrong && f.value("check", std::string()) == "meaning") flagged = true;
        if (f.value("memory_id", (int64_t)0) == right) right_flagged = true;
    }
    expect(flagged, "priya: \"Priya approved the budget\" is flagged: the recording says approval is pending");
    expect(!right_flagged, "priya: \"Priya will review the budget on Friday\" passes");
    bool parts_ok = true;
    for (const auto &kv : r2.value("parts", json::object()).items()) parts_ok = parts_ok && !kv.value().value("please_check", true);
    expect(parts_ok, "priya: summary, notes and questions pass (reviewing is not approving)");
    expect(ctx->store->setting_get(key).has_value(), "priya: the verdict is stored only now");
    expect(take(sve_memory_get(ctx, wrong)).value("queue", 0) == 1, "priya: the wrong relationship's memory waits for review");
    bool evidence = false;
    for (const auto &e : r2.value("evidence", json::array()))
        evidence = evidence || (e.value("part", std::string()) == "relation" && !e.value("ok", true) && e.value("start_sec", (int64_t)-1) == 0);
    expect(evidence, "priya: the flagged relationship carries where the recording says it");
    // 3. The relations review shows the line with why and where.
    bool shown = false;
    for (const auto &rv : take(sve_relation_reviews_json(ctx)))
        if (rv.value("note_id", (int64_t)0) == note)
            for (const auto &d : rv.value("decisions", json::array()))
                shown = shown || (d.value("text", std::string()) == "Priya approved the budget" &&
                                  d.value("check", std::string()) == "meaning" && d.value("check_sec", (int64_t)-1) == 0);
    expect(shown, "priya: the relations review flags the line (meaning, at 0:00)");
    g_sve_model_reply = nullptr;
    sve_free(ctx);
    remove(path);
}

int main(int argc, char **argv) {
    const char *mode = argc > 1 ? argv[1] : "transcribe";
    const char *gguf = getenv("SVE_GEMMA"), *mmproj = getenv("SVE_MMPROJ"), *wav = getenv("SVE_TEST_WAV");
    const bool meaning = !strcmp(mode, "embed") || !strcmp(mode, "search") || !strcmp(mode, "gate");
    const bool needs_db = !strcmp(mode, "memories") || meaning;
    const char *db = needs_db ? getenv("SVE_DB") : nullptr;
    if (needs_db && !db) { fprintf(stderr, "set SVE_DB (a copy of a notes.sqlite, never the original)\n"); return 2; }
    sve_ctx *ctx = sve_init(db ? db : ":memory:");
    if (!ctx) { fprintf(stderr, "sve_init failed\n"); return 1; }
    if (meaning) {
        int rc = run_meaning(ctx, mode, db);
        sve_free(ctx);
        return rc;
    }
    if (!strcmp(mode, "api")) {
        run_api(ctx);
        run_priya();
        // No context or no model: nothing to load again. (The flag itself needs a failed GPU run: checked on the phone.)
        expect(sve_llm_needs_reload(nullptr) == 0, "needs_reload is off with no context");
        expect(sve_llm_needs_reload(ctx) == 0, "needs_reload is off with no model");
        sve_free(ctx);
        printf("%s (%d missed)\n", fails ? "API FAIL" : "API OK", fails);
        return fails ? 1 : 0;
    }
    if (!gguf) { fprintf(stderr, "set SVE_GEMMA\n"); return 2; }
    if (const char *g = getenv("SVE_GREEDY")) g_sve_force_greedy = *g && strcmp(g, "0") != 0;
    if (const char *p = getenv("SVE_TOKEN_LOG"))
        if (!(g_sve_token_log = fopen(p, "a"))) { fprintf(stderr, "cannot open SVE_TOKEN_LOG %s: %s\n", p, strerror(errno)); return 1; }
    if (!strcmp(mode, "memories")) {
        if (sve_load_llm(ctx, gguf, 8192, 4, 0) != 0) { fprintf(stderr, "sve_load_llm failed\n"); return 1; }
        if (getenv("SVE_SEED_WAV") && (!mmproj || sve_load_audio(ctx, mmproj) != 0)) { fprintf(stderr, "SVE_SEED_WAV needs SVE_MMPROJ\n"); return 1; }
        g_sve_memory_drop = smoke_drop;
        sve_set_log(smoke_log, nullptr);
        int rc = run_memories(ctx, db);
        printf("%s (%d missed)\n", fails || rc ? "MEMORIES FAIL" : "MEMORIES OK", fails);
        sve_free(ctx);
        return rc || fails ? 1 : 0;
    }
    if (!strcmp(mode, "threads")) {
        // The heat ladder's knob: CPU threads change at run time (decode, batch) and a run still works after.
        if (sve_load_llm(ctx, gguf, 2048, 4, 0) != 0) { fprintf(stderr, "sve_load_llm failed\n"); return 1; }
        llama_context *lc = ctx->llm->ctx;
        const int hw = (int)std::thread::hardware_concurrency();
        expect(llama_n_threads(lc) == 4 && llama_n_threads_batch(lc) == sve::batch_threads(hw, 4), "load: 4 decode, the batch rule");
        expect(sve_set_threads(ctx, 2, 2) == 0 && llama_n_threads(lc) == 2 && llama_n_threads_batch(lc) == 2, "minimal: 2 and 2");
        char *t = sve_suggest_title(ctx, "We talked about the garden, the tomatoes and when to water them.", "en");
        expect(t && *t, "a run on 2 threads still answers");
        if (t) { printf("title on 2 threads: %s\n", t); sve_free_string(t); }
        expect(sve_set_threads(ctx, 4, 0) == 0 && llama_n_threads(lc) == 4 && llama_n_threads_batch(lc) == sve::batch_threads(hw, 4),
               "full: back to 4 and the batch rule");
        printf("%s (%d missed)\n", fails ? "THREADS FAIL" : "THREADS OK", fails);
        sve_free(ctx);
        return fails ? 1 : 0;
    }
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
    } else if (!strcmp(mode, "notes")) {
        if (!mmproj || !wav) { fprintf(stderr, "set SVE_MMPROJ and SVE_TEST_WAV\n"); return 2; }
        if (sve_load_audio(ctx, mmproj) != 0) { fprintf(stderr, "sve_load_audio failed\n"); return 1; }
        const char *lang = getenv("SVE_LANG"), *q = getenv("SVE_QUESTION");
        if (int rc = run_notes(ctx, wav, lang ? lang : "te", q ? q : "What are the main points of this recording?")) return rc;
    } else if (!strcmp(mode, "fits")) {
        const char *lang = getenv("SVE_LANG");
        run_fits(ctx, lang ? lang : "te");
        printf("%s (%d missed)\n", fails ? "FITS FAIL" : "FITS OK", fails);
        if (fails) { sve_free(ctx); return 1; }
    } else { fprintf(stderr, "unknown mode %s\n", mode); return 2; }
    if (g_sve_token_log) fclose(g_sve_token_log);
    sve_free(ctx);
    return 0;
}
