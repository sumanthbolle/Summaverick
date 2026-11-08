#!/bin/bash
# Runs the test set through Gemma weights with greedy decoding and compares two weights files.
#
#   compare_outputs.sh collect <full.gguf> <tokens.txt>     run the full weights, write the token ids they used
#   compare_outputs.sh compare <full.gguf> <trimmed.gguf>   run both, print IDENTICAL or the first difference
#                                                           per item, then GATE: PASS|FAIL
# Env: SVE_MMPROJ (audio projector, required), TESTSET (dir of 16 kHz mono WAVs, default tools/model/testset;
#      te-reference.wav in it is the Telugu reference clip), WORK (output dir, default $TMPDIR/sve-compare).
# Items: transcribe-<clip> for every WAV; notes-te / notes-en (pieces, three note kinds, one chat question);
#        extract (doctor-visit fixture: suggestions, two questions, topic summary, insight); longnotes (CPU).
# GATE: PASS needs every item to exit 0 with non-empty output in both runs, and to be identical.
# Every generation is greedy (SVE_GREEDY=1). Masked before comparing: "<n.n> s" timings on lines starting
# with "---", and the wall-clock values of "*_at" JSON keys (created_at differs between any two runs).
set -eu
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
TESTSET="${TESTSET:-$ROOT/tools/model/testset}"
WORK="${WORK:-${TMPDIR:-/tmp}/sve-compare}"
: "${SVE_MMPROJ:?set SVE_MMPROJ}"
export SVE_MMPROJ SVE_GREEDY=1 SVE_SMOKE_BIN="$WORK/smoke_gemma"
mkdir -p "$WORK"
rm -f "$SVE_SMOKE_BIN"   # rebuild once per invocation so the binary matches the sources

. "$ROOT/tools/model/compare_lib.sh"   # run_all, check_run, compare_dirs
find_clips || exit 2

case "${1:-}" in
collect)
    [ $# -eq 3 ] || { sed -n 4,5p "$0"; exit 2; }
    log="$WORK/tokens.log"; rm -f "$log"
    echo "running the full weights on $(ls "$TESTSET"/*.wav | wc -l | tr -d ' ') clips + 4 text items" >&2
    run_all "$2" "$WORK/full" "$log"
    check_run "$WORK/full" || { echo "collect FAILED: fix the items above before trimming"; exit 1; }
    awk '{print $2}' "$log" | sort -n -u >"$3"
    echo "token ids: $(wc -l <"$3" | tr -d ' ') distinct ($(grep -c '^o' "$log") generated, $(grep -c '^i' "$log") prompt) -> $3"
    ;;
compare)
    [ $# -eq 3 ] || { sed -n 4,5p "$0"; exit 2; }
    echo "full weights" >&2;    run_all "$2" "$WORK/full"
    echo "trimmed weights" >&2; run_all "$3" "$WORK/trimmed"
    failed=0
    check_run "$WORK/full" || failed=1
    check_run "$WORK/trimmed" "$WORK/full" || failed=1
    compare_dirs "$WORK/full" "$WORK/trimmed" full trimmed
    n=$COMPARED; bad=$DIFFERENT
    echo "$((n - bad))/$n items identical"
    [ "$failed" -eq 0 ] || echo "some runs failed (listed above): identical failures do not count"
    if [ "$bad" -eq 0 ] && [ "$failed" -eq 0 ] && [ "$n" -gt 0 ]; then echo "GATE: PASS"; else echo "GATE: FAIL"; exit 1; fi
    ;;
*) sed -n 2,11p "$0"; exit 2 ;;
esac
