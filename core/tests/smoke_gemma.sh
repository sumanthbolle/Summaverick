#!/bin/bash
# Builds core/tests/smoke_gemma.cpp against Homebrew llama.cpp + mtmd and runs it.
# Usage: smoke_gemma.sh [transcribe|live|sections|extract|api]   Env: SVE_GEMMA SVE_MMPROJ SVE_TEST_WAV [SVE_FIXTURE]
#   api needs no model; live feeds SVE_TEST_WAV as a growing recording (CPU); sections = live's pieces, ~5 min sections, one summary each (SVE_LANG, default te); extract uses core/tests/fixtures/doctor_visit.txt by default.
set -eu
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT="$(mktemp -d)"; trap 'rm -rf "$OUT"' EXIT
BREW="${BREW_PREFIX:-/opt/homebrew}"
mkdir -p "$OUT/shim"; ln -s "$BREW/include" "$OUT/shim/llama"   # core includes <llama/llama.h>
SRCS=("$ROOT/core/tests/smoke_gemma.cpp")
for f in "$ROOT"/core/src/sve_*.cpp; do SRCS+=("$f"); done
clang++ -std=c++17 -O2 -DSVE_FIXTURE_DIR="\"$ROOT/core/tests/fixtures\"" -I"$OUT/shim" -I"$BREW/include" -I"$ROOT/core/include" -I"$ROOT/core/src" \
    "${SRCS[@]}" -L"$BREW/lib" -lllama -lmtmd -lggml -lggml-base -lsqlite3 -Wl,-rpath,"$BREW/lib" -o "$OUT/smoke_gemma"
"$OUT/smoke_gemma" "${1:-transcribe}"
