// On-device-model smoke test. Env: SVE_GEMMA, SVE_MMPROJ, SVE_TEST_WAV, SVE_FIXTURE.
// Arg: mode
//   transcribe  transcribe SVE_TEST_WAV
//   extract     doctor-visit fixture: extract, list pending suggestions, two Ask questions
//   api         assistant C API without a model (accept/reject, medicines, medicine Ask route)
#include "sve_core.h"
#include "sve_internal.h"  // api mode adds model-like suggestions straight to the store
#include "third_party/nlohmann/json.hpp"
#include <cstdio>
#include <cstdlib>
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

    // Update -> old reminder off, new reminder proposed.
    json upd = {{"kind", "profile"}, {"note_id", note}, {"start_sec", 0}, {"quote", "Take Ramipril 10 mg"}, {"check_status", "ok"},
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
    if (sve_load_llm(ctx, gguf, 8192, 0, 99) != 0) { fprintf(stderr, "sve_load_llm failed\n"); return 1; }
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
