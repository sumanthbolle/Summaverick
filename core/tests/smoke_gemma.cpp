// On-device-model smoke test. Env: SVE_GEMMA, SVE_MMPROJ, SVE_TEST_WAV. Arg: mode (transcribe).
#include "sve_core.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

static int progress(const char *phase, int done, int total, void *) {
    fprintf(stderr, "  [%s %d/%d]\n", phase ? phase : "?", done, total);
    return 0;
}

int main(int argc, char **argv) {
    const char *mode = argc > 1 ? argv[1] : "transcribe";
    const char *gguf = getenv("SVE_GEMMA"), *mmproj = getenv("SVE_MMPROJ"), *wav = getenv("SVE_TEST_WAV");
    if (!gguf) { fprintf(stderr, "set SVE_GEMMA\n"); return 2; }
    sve_ctx *ctx = sve_init(":memory:");
    if (!ctx) { fprintf(stderr, "sve_init failed\n"); return 1; }
    if (sve_load_llm(ctx, gguf, 8192, 0, 99) != 0) { fprintf(stderr, "sve_load_llm failed\n"); return 1; }
    if (!strcmp(mode, "transcribe")) {
        if (!mmproj || !wav) { fprintf(stderr, "set SVE_MMPROJ and SVE_TEST_WAV\n"); return 2; }
        if (sve_load_audio(ctx, mmproj) != 0) { fprintf(stderr, "sve_load_audio failed\n"); return 1; }
        char *t = sve_transcribe_audio(ctx, wav, progress, nullptr);
        if (!t || !*t) { fprintf(stderr, "empty transcript\n"); return 1; }
        printf("%s\n", t);
        sve_free_string(t);
    } else { fprintf(stderr, "unknown mode %s\n", mode); return 2; }
    sve_free(ctx);
    return 0;
}
