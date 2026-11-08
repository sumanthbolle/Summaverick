#!/bin/bash
# Builds core/tests/smoke_gemma.cpp against Homebrew llama.cpp + mtmd and runs it.
# Usage: smoke_gemma.sh [transcribe|live|sections|longnotes|notes|fits|extract|api|memories]   Env: SVE_GEMMA SVE_MMPROJ SVE_TEST_WAV [SVE_FIXTURE SVE_LANG SVE_QUESTION SVE_GREEDY SVE_TOKEN_LOG]
#   api needs no model; fits needs only SVE_GEMMA; live feeds SVE_TEST_WAV as a growing recording (CPU); sections = live's pieces, ~5 min sections, one summary each (SVE_LANG, default te); longnotes = sections, then a synthetic ~2 h input (repeated summaries) -> notes x3 + title + chapters; extract uses core/tests/fixtures/doctor_visit.txt by default;
#   memories needs SVE_DB (a copy, never the phone original) [SVE_NOTES=all|id,id SVE_SEED_FIXTURE SVE_SEED_WAV].
set -eu
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT="$(mktemp -d)"; trap 'rm -rf "$OUT"' EXIT
BREW="${BREW_PREFIX:-/opt/homebrew}"
mkdir -p "$OUT/shim"; ln -s "$BREW/include" "$OUT/shim/llama"   # core includes <llama/llama.h>
BIN="${SVE_SMOKE_BIN:-$OUT/smoke_gemma}"   # SVE_SMOKE_BIN: build once to this path, reuse it while it exists
if [ ! -x "$BIN" ]; then
    SRCS=("$ROOT/core/tests/smoke_gemma.cpp")
    for f in "$ROOT"/core/src/sve_*.cpp; do SRCS+=("$f"); done
    clang++ -std=c++17 -O2 -DSVE_FIXTURE_DIR="\"$ROOT/core/tests/fixtures\"" -I"$OUT/shim" -I"$BREW/include" -I"$ROOT/core/include" -I"$ROOT/core/src" \
        "${SRCS[@]}" -L"$BREW/lib" -lllama -lmtmd -lggml -lggml-base -lsqlite3 -Wl,-rpath,"$BREW/lib" -o "$BIN"
fi
"$BIN" "${1:-transcribe}"
