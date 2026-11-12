// sve — command-line front end for the Summaverick Everyday core.
// On the device, the same binary runs behind the web UI; on a laptop it is the quickest way to test
// models and prompts without touching the phone app.
//
//   sve transcribe --asr ggml-base.bin --lang hi recording.wav            > transcript.txt
//   sve notes --llm qwen3-0.6b-q4.gguf --lang en --kind detailed transcript.txt
//   sve ask   --llm qwen3-0.6b-q4.gguf "what was the homework?" transcript.txt
//   sve store --db notes.db add --title "Biology" --lang en transcript.txt
//   sve store --db notes.db list [query]
//   sve store --db notes.db get <id>
#include "sve_core.h"
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

static std::string slurp(const std::string &path) {
    std::ifstream f(path);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static int progress(const char *phase, int done, int total, void *) {
    if (done >= 0) fprintf(stderr, "\r[%s %d/%d]   ", phase, done, total);
    return 0; // never cancel
}

static void usage() {
    fprintf(stderr,
        "usage:\n"
        "  sve transcribe --asr MODEL [--lang xx] FILE.wav\n"
        "  sve notes --llm MODEL.gguf [--lang xx] [--kind short|detailed|revision|all] [--threads N] [--ctx N] TRANSCRIPT.txt\n"
        "  sve ask --llm MODEL.gguf [--lang xx] \"question\" TRANSCRIPT.txt\n"
        "  sve title --llm MODEL.gguf TRANSCRIPT.txt\n"
        "  sve store --db notes.db add --title T [--lang xx] TRANSCRIPT.txt\n"
        "  sve store --db notes.db list [query]\n"
        "  sve store --db notes.db get ID\n");
}

int main(int argc, char **argv) {
    if (argc < 2) { usage(); return 1; }
    std::string cmd = argv[1];
    std::string llm, asr, db = ":memory:", lang = "en", kind = "all", title = "Untitled";
    int threads = 0, nctx = 0;
    std::vector<std::string> pos;
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](std::string &dst) { if (i + 1 < argc) dst = argv[++i]; };
        if (a == "--llm") next(llm);
        else if (a == "--asr") next(asr);
        else if (a == "--db") next(db);
        else if (a == "--lang") next(lang);
        else if (a == "--kind") next(kind);
        else if (a == "--title") next(title);
        else if (a == "--threads") { std::string s; next(s); threads = atoi(s.c_str()); }
        else if (a == "--ctx") { std::string s; next(s); nctx = atoi(s.c_str()); }
        else pos.push_back(a);
    }

    sve_ctx *ctx = sve_init(db.c_str());
    if (!ctx) { fprintf(stderr, "init failed\n"); return 2; }

    auto need_llm = [&]() {
        if (llm.empty()) { fprintf(stderr, "--llm required\n"); exit(1); }
        if (sve_load_llm(ctx, llm.c_str(), nctx, threads, 0) != 0) { fprintf(stderr, "model load failed\n"); exit(3); }
    };

    if (cmd == "transcribe") {
#ifdef SVE_WITH_WHISPER
        if (asr.empty() || pos.empty()) { usage(); return 1; }
        if (sve_load_asr(ctx, asr.c_str(), 0, threads) != 0) { fprintf(stderr, "asr load failed\n"); return 3; }
        char *t = sve_transcribe_wav(ctx, pos[0].c_str(), lang == "auto" ? "auto" : lang.c_str(), progress, nullptr);
        fprintf(stderr, "\n");
        if (!t) { fprintf(stderr, "transcription failed\n"); return 4; }
        printf("%s", t);
        sve_free_string(t);
#else
        fprintf(stderr, "built without whisper (SVE_WITH_WHISPER=OFF)\n");
        return 1;
#endif
    } else if (cmd == "notes") {
        if (pos.empty()) { usage(); return 1; }
        need_llm();
        std::string transcript = slurp(pos[0]);
        struct { const char *name; sve_output_kind k; } kinds[] = {
            {"short", SVE_OUT_SHORT_SUMMARY}, {"detailed", SVE_OUT_DETAILED_NOTES}, {"revision", SVE_OUT_REVISION_QUESTIONS}};
        for (auto &kk : kinds) {
            if (kind != "all" && kind != kk.name) continue;
            char *out = sve_generate(ctx, transcript.c_str(), kk.k, lang.c_str(), progress, nullptr);
            fprintf(stderr, "\n");
            printf("===== %s =====\n%s\n\n", kk.name, out ? out : "(failed)");
            sve_free_string(out);
        }
    } else if (cmd == "ask") {
        if (pos.size() < 2) { usage(); return 1; }
        need_llm();
        std::string transcript = slurp(pos[1]);
        char *out = sve_ask(ctx, transcript.c_str(), pos[0].c_str(), lang.c_str(), progress, nullptr);
        fprintf(stderr, "\n");
        printf("%s\n", out ? out : "(failed)");
        sve_free_string(out);
    } else if (cmd == "title") {
        if (pos.empty()) { usage(); return 1; }
        need_llm();
        std::string transcript = slurp(pos[0]);
        char *out = sve_suggest_title(ctx, transcript.c_str(), lang.c_str());
        printf("%s\n", out ? out : "(failed)");
        sve_free_string(out);
    } else if (cmd == "store") {
        if (pos.empty()) { usage(); return 1; }
        std::string sub = pos[0];
        if (sub == "add" && pos.size() >= 2) {
            std::string transcript = slurp(pos[1]);
            int64_t id = sve_note_create(ctx, title.c_str(), lang.c_str(), nullptr, transcript.c_str(), 0);
            printf("%lld\n", (long long)id);
        } else if (sub == "list") {
            char *j = sve_notes_list_json(ctx, pos.size() > 1 ? pos[1].c_str() : nullptr, 50);
            printf("%s\n", j ? j : "[]");
            sve_free_string(j);
        } else if (sub == "get" && pos.size() >= 2) {
            char *j = sve_note_get_json(ctx, atoll(pos[1].c_str()));
            printf("%s\n", j ? j : "null");
            sve_free_string(j);
        } else { usage(); return 1; }
    } else { usage(); return 1; }

    sve_free(ctx);
    return 0;
}
