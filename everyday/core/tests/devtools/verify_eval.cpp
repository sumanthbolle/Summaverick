// The checking agent (sve_verify_note) on a fixture, as the phone runs a short note: one section, the one-pass notes
// (else the three outputs one by one), the action items, the memories, then the check. Prints what the first check
// found (attempt 0, "before") and what is left after the retries ("after"), per part, with the seconds taken.
//   verify_eval <model.gguf> <fixture.txt> <db> [lang] [--dump]   (SVE_VOCAB="A,B": saved words first)
#include "sve_core.h"
#include "third_party/nlohmann/json.hpp"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <regex>
#include <string>
#include <unistd.h>
using json = nlohmann::json;
static void quiet(int, const char *, void *) {}
static void loud(int, const char *m, void *) { fprintf(stderr, "  [log] %s\n", m); }
static json take(char *p) { json j = p ? json::parse(p, nullptr, false) : json(); if (p) sve_free_string(p); return j; }
static std::string str(char *p) { std::string s = p ? p : ""; if (p) sve_free_string(p); return s; }
static double now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "verify_eval <model> <fixture> <db> [lang] [--dump]\n"); return 2; }
    const std::string lang = argc > 4 && argv[4][0] != '-' ? argv[4] : "en";
    bool dump = false;
    for (int i = 4; i < argc; ++i) dump = dump || !strcmp(argv[i], "--dump");
    sve_set_log(getenv("SVE_LOG") ? loud : quiet, nullptr);
    std::remove(argv[3]);
    sve_ctx *c = sve_init(argv[3]);
    if (!c || sve_load_llm(c, argv[1], 8192, 8, 99) != 0) return 1;
    sve_profile_set(c, (std::string(R"({"name":"Sumanth","lang":")") + lang + "\"}").c_str());
    // SVE_VOCAB="A,B": saved words (the noisy fixture's pieces say them as speech, as note 18 did).
    if (const char *v = getenv("SVE_VOCAB")) {
        std::string all = v, w;
        for (size_t i = 0; i <= all.size(); ++i) {
            if (i == all.size() || all[i] == ',') { if (!w.empty()) sve_vocab_add(c, w.c_str()); w.clear(); }
            else w.push_back(all[i]);
        }
    }
    std::ifstream in(argv[2]);
    json pieces = json::array();
    std::string line, transcript;
    std::regex re(R"(^\[t=(\d+)\]\s*(.*)$)");
    int last = 0;
    while (std::getline(in, line)) {
        std::smatch m;
        if (!std::regex_match(line, m, re)) continue;
        last = std::stoi(m[1]);
        pieces.push_back({{"start_sec", last}, {"text", m[2].str()}});
        transcript += m[2].str() + "\n";
    }
    int64_t id = sve_note_create(c, "Recording", lang.c_str(), nullptr, transcript.c_str(), last + 30);
    sve_note_set_pieces(c, id, pieces.dump().c_str());
    double t0 = now();
    json summary = take(sve_section_summarize(c, pieces.dump().c_str(), lang.c_str(), nullptr, nullptr));
    json secs = json::array({{{"idx", 0}, {"first_piece", 0}, {"last_piece", (int)pieces.size() - 1}, {"start_sec", 0},
                              {"end_sec", last + 30}, {"summary", summary}}});
    sve_note_set_sections(c, id, secs.dump().c_str());
    json all = take(sve_generate_all(c, transcript.c_str(), lang.c_str(), nullptr, nullptr));
    const char *how = "one pass";
    if (all.is_object()) {
        sve_note_set_title(c, id, all.value("title", "").c_str());
        sve_note_set_output(c, id, SVE_OUT_SHORT_SUMMARY, all.value("summary", "").c_str());
        sve_note_set_output(c, id, SVE_OUT_DETAILED_NOTES, all.value("notes", "").c_str());
        sve_note_set_output(c, id, SVE_OUT_REVISION_QUESTIONS, all.value("questions", "").c_str());
    } else {
        how = "three outputs";
        for (auto k : {SVE_OUT_SHORT_SUMMARY, SVE_OUT_DETAILED_NOTES, SVE_OUT_REVISION_QUESTIONS})
            sve_note_set_output(c, id, k, str(sve_generate(c, transcript.c_str(), k, lang.c_str(), nullptr, nullptr)).c_str());
    }
    double t1 = now();
    sve_extract(c, id, nullptr, nullptr);
    double t2 = now();
    sve_memories_from_section(c, id, 0, nullptr, nullptr);
    double t3 = now();
    json before_note = take(sve_note_get_json(c, id));
    json r = take(sve_verify_note(c, id, nullptr, nullptr));
    double t4 = now();
    if (!r.is_object()) { printf("verify failed\n"); _exit(1); }
    printf("notes: %s %.0fs, actions %.0fs, memories %.0fs, check %.0fs (%d yes/no calls, %d unmeasured)\n", how, t1 - t0,
           t2 - t1, t3 - t2, t4 - t3, r.value("judge_calls", 0), r.value("unjudged", 0));
    for (const char *part : {"summary", "notes", "questions"}) {
        int first_failed = -1, first_total = 0;
        for (const auto &e : r["events"])
            if (e.value("part", "") == part && e.value("action", "") == "check" && e.value("attempt", -1) == 0) {
                first_failed = e.value("failed", 0);
                first_total = e.value("total", 0);
            }
        const json p = r["parts"].value(part, json::object());
        std::string checks;
        for (const auto &x : p.value("checks", json::array())) checks += x.get<std::string>() + " ";
        printf("BEFORE %-9s %d failed of %d | AFTER retries=%d kept=%d please_check=%s %s\n", part, first_failed,
               first_total, p.value("retries", 0), p.value("chosen", 0), p.value("please_check", false) ? "YES" : "no",
               checks.c_str());
        for (const auto &it : p.value("items", json::array())) printf("    left: %s\n", it.get<std::string>().c_str());
    }
    printf("MEMORIES %d, fixed %d, flagged %d | ACTIONS %d, fixed %d, flagged %d\n", r.value("memories", 0),
           r.value("memories_fixed", 0), r.value("memories_flagged", 0), r.value("actions", 0), r.value("actions_fixed", 0),
           r.value("actions_flagged", 0));
    if (dump) {
        for (const auto &e : r["events"]) printf("  event %s\n", e.dump().c_str());
        json after = take(sve_note_get_json(c, id));
        for (const char *k : {"short_summary", "detailed_notes", "revision_questions"}) {
            const std::string b = before_note.value(k, ""), a = after.value(k, "");
            printf("--- %s%s\n%s\n", k, a == b ? " (unchanged)" : " (BEFORE)", b.c_str());
            if (a != b) printf("--- %s (AFTER)\n%s\n", k, a.c_str());
        }
    }
    fflush(stdout);
    _exit(0);
}
