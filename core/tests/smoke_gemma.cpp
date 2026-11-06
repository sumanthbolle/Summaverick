// On-device-model smoke test. Env: SVE_GEMMA, SVE_MMPROJ, SVE_TEST_WAV, SVE_FIXTURE.
// Arg: mode
//   transcribe  transcribe SVE_TEST_WAV
//   extract     doctor-visit fixture: extract, list pending suggestions, two Ask questions
//   api         assistant C API without a model (accept/reject, medicines, medicine Ask route)
#include "sve_core.h"
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
