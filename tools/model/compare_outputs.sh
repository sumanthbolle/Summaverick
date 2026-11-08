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

REF="$TESTSET/te-reference.wav"
EN_CLIP="$(ls "$TESTSET"/*.wav | grep -v te-reference | sort | sed -n 2p)"
[ -f "$REF" ] || { echo "missing $REF"; exit 2; }
[ -n "$EN_CLIP" ] || { echo "need at least two WAVs besides te-reference.wav in $TESTSET"; exit 2; }

# run_all <weights> <outdir> [token log]
run_all() {
    local w="$1" out="$2" log="${3:-}"
    rm -rf "$out"; mkdir -p "$out"
    run() {   # run <item> <mode> [ENV=value ...]
        local item="$1" mode="$2"; shift 2
        echo "  $item" >&2
        local rc=0
        env SVE_GEMMA="$w" ${log:+"SVE_TOKEN_LOG=$log"} "$@" bash "$ROOT/core/tests/smoke_gemma.sh" "$mode" \
            >"$out/$item.raw" 2>"$out/$item.err" || rc=$?
        echo "(exit status $rc)" >>"$out/$item.raw"
        sed -E -e '/^---/s/[0-9]+\.[0-9]+ s/<t> s/g' -e 's/("[a-z_]+_at": )[0-9]+/\1<time>/g' "$out/$item.raw" >"$out/$item.txt"
    }
    for wav in "$TESTSET"/*.wav; do
        run "transcribe-$(basename "$wav" .wav)" transcribe SVE_TEST_WAV="$wav"
    done
    run notes-te notes SVE_TEST_WAV="$REF" SVE_LANG=te SVE_QUESTION="ఈ రికార్డింగ్ లో ముఖ్యమైన విషయాలు ఏమిటి?"
    run notes-en notes SVE_TEST_WAV="$EN_CLIP" SVE_LANG=en SVE_QUESTION="What are the main points of this recording?"
    run extract extract
    run longnotes longnotes SVE_TEST_WAV="$REF" SVE_LANG=te
}

# check_run <outdir> [<reference outdir>]: prints "<item>: FAILED (<why>)" for every item whose run exited
# nonzero, or printed nothing besides the exit-status line, or (with a reference dir) is missing.
# Returns 1 if any item failed.
check_run() {
    local out="$1" ref="${2:-}" bad=0 f item
    if [ -n "$ref" ]; then
        for f in "$ref"/*.raw; do
            item="$(basename "$f" .raw)"
            [ -f "$out/$item.raw" ] || { echo "$item: FAILED (no output in $(basename "$out"))"; bad=1; }
        done
    fi
    for f in "$out"/*.raw; do
        [ -e "$f" ] || { echo "$(basename "$out"): FAILED (no items ran)"; return 1; }
        item="$(basename "$f" .raw)"
        local last; last="$(tail -n 1 "$f")"
        if [ "$last" != "(exit status 0)" ]; then
            echo "$item: FAILED in $(basename "$out") ($last; see $out/$item.err)"; bad=1
        elif [ -z "$(sed '$d' "$f" | tr -d '[:space:]')" ]; then
            echo "$item: FAILED in $(basename "$out") (empty output; see $out/$item.err)"; bad=1
        fi
    done
    return $bad
}

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
    n=0; bad=0; failed=0
    check_run "$WORK/full" || failed=1
    check_run "$WORK/trimmed" "$WORK/full" || failed=1
    for f in "$WORK/full"/*.txt; do
        item="$(basename "$f" .txt)"; g="$WORK/trimmed/$item.txt"; n=$((n + 1))
        if cmp -s "$f" "$g"; then
            echo "$item: IDENTICAL"
        else
            bad=$((bad + 1))
            line="$(cmp "$f" "$g" 2>&1 | sed -nE 's/.* line ([0-9]+).*/\1/p')"
            [ -n "$line" ] || line=1
            echo "$item: DIFFERS at line $line"
            echo "    full:    $(sed -n "${line}p" "$f" | cut -c1-200)"
            echo "    trimmed: $(sed -n "${line}p" "$g" | cut -c1-200)"
        fi
    done
    echo "$((n - bad))/$n items identical"
    [ "$failed" -eq 0 ] || echo "some runs failed (listed above): identical failures do not count"
    if [ "$bad" -eq 0 ] && [ "$failed" -eq 0 ] && [ "$n" -gt 0 ]; then echo "GATE: PASS"; else echo "GATE: FAIL"; exit 1; fi
    ;;
*) sed -n 2,11p "$0"; exit 2 ;;
esac
