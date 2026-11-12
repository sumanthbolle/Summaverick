#!/bin/bash
# Builds each core/tests/test_*.cpp with the pure core sources and runs it.
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT

SRCS=()
for n in sve_store sve_medcheck sve_textmatch sve_extract sve_context sve_learn sve_suggest sve_audio sve_sections sve_health sve_memory sve_brainmap sve_consolidate sve_chat sve_assemble sve_embed sve_whisper_text; do
    [ -f "$ROOT/core/src/$n.cpp" ] && SRCS+=("$ROOT/core/src/$n.cpp")
done

total_pass=0; total_fail=0; rc=0
for t in "$ROOT"/core/tests/test_*.cpp; do
    [ -e "$t" ] || continue
    bin="$OUT/$(basename "$t" .cpp)"
    if ! clang++ -std=c++17 -Wall -Wextra -DSVE_TEST_MAIN -I "$ROOT/core/include" -I "$ROOT/core/src" -I "$ROOT/core/tests" \
            "$t" ${SRCS[@]+"${SRCS[@]}"} -lsqlite3 -o "$bin"; then
        echo "BUILD FAILED: $t"; rc=1; continue
    fi
    output="$("$bin")"; code=$?
    echo "$output"
    [ $code -ne 0 ] && rc=1
    p=$(echo "$output" | tail -1 | awk '{print $1}'); f=$(echo "$output" | tail -1 | awk '{print $3}')
    total_pass=$((total_pass + ${p:-0})); total_fail=$((total_fail + ${f:-0}))
done
echo "TOTAL: $total_pass passed, $total_fail failed"
exit $rc
