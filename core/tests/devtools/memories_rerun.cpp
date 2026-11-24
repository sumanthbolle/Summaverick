#include "sve_core.h"
#include "sve_internal.h"
#include <cstdio>
#include <cstdlib>
#include <string>
static void log(int lvl, const char *m, void *) { if (lvl >= 0 && std::string(m) != ".") fprintf(stderr, "[log%d] %s\n", lvl, m); }
static void drop(int64_t n, int i, const char *r, const char *c) { fprintf(stderr, "DROP note %lld s%d %s :: %.260s\n", (long long)n, i, r, c); FILE *f = fopen(getenv("DROPS"), "a"); if (f) { fprintf(f, "%s\t%s\n", r, c); fclose(f); } }
int main(int argc, char **argv) {
    g_sve_memory_drop = drop;
    sve_set_log(log, nullptr);
    sve_ctx *c = sve_init(argv[1]);
    if (!c || sve_load_llm(c, argv[2], 8192, 8, 99) != 0) { fprintf(stderr, "load failed\n"); return 1; }
    for (int i = 3; i < argc; i++) {
        long long note = atoll(argv[i]);
        char *due = sve_memory_sections_due(c, note);
        fprintf(stderr, "note %lld due %s\n", note, due ? due : "null");
        std::string d = due ? due : "[]";
        sve_free_string(due);
        for (int idx = 0; idx < 12; idx++) {
            if (d.find(std::to_string(idx)) == std::string::npos) continue;
            int r = sve_memories_from_section(c, note, idx, nullptr, nullptr);
            fprintf(stderr, "note %lld section %d -> %d\n", note, idx, r);
        }
    }
    sve_free(c);
}
