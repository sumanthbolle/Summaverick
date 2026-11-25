// Times note generation on the Mac for one note: token counts of the transcript, and the one-pass generation.
#include "sve_core.h"
#include "sve_internal.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
static int ntok = 0;
static void log(int, const char *, void *) {}
static int prog(const char *phase, int done, int total, void *) { ntok = done; (void)phase; (void)total; return 0; }
int main(int argc, char **argv) {
    sve_set_log(log, nullptr);
    sve_ctx *c = sve_init(argv[1]);
    const int nctx = atoi(argv[4]);
    if (!c || sve_load_llm(c, argv[2], nctx, 4, 0) != 0) { fprintf(stderr, "load failed\n"); return 1; }  // CPU, 4 threads: like the phone in the background
    long long id = atoll(argv[3]);
    char *nj = sve_note_get_json(c, id);
    std::string s = nj ? nj : "";
    sve_free_string(nj);
    auto j = nlohmann::json::parse(s);
    std::string tr = j.value("transcript", "");
    printf("transcript chars %zu, tokens %d, all_fits %d\n", tr.size(), c->llm->count_tokens(tr), sve_all_fits(c, tr.c_str()));
    auto t0 = std::chrono::steady_clock::now();
    char *all = sve_generate_all(c, tr.c_str(), "en", prog, nullptr);
    double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::string a = all ? all : "";
    printf("one pass: %.1f s, ok %d, out chars %zu, out tokens %d\n", sec, all != nullptr, a.size(), all ? c->llm->count_tokens(a) : 0);
    sve_free_string(all);
}
