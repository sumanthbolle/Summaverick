#!/bin/bash
# Shared by compare_outputs.sh and compare_commits.sh (sourced, not run). The caller sets ROOT and TESTSET
# and exports SVE_MMPROJ, SVE_GREEDY=1 and SVE_SMOKE_BIN (the smoke binary, built once per path).
# SMOKE: the smoke_gemma.sh that runs the items (default: this checkout's).
#
# Item names: transcribe-<clip> (a WAV in TESTSET), transcribe-all (every WAV), notes-te / notes-en (pieces,
# three note kinds, one chat question), extract (doctor-visit fixture), longnotes (CPU), fits.
# Each item writes <outdir>/<item>.raw (stdout plus "(exit status N)"), .err (stderr) and .txt (.raw masked:
# "<n.n> s" timings on lines starting with "---", and the wall-clock values of "*_at" JSON keys).

# Sets REF (the Telugu reference clip) and EN_CLIP (the second other WAV, sorted) for the notes items.
find_clips() {
    REF="$TESTSET/te-reference.wav"
    EN_CLIP="$(ls "$TESTSET"/*.wav | grep -v te-reference | sort | sed -n 2p)"
    [ -f "$REF" ] || { echo "missing $REF"; return 2; }
    [ -n "$EN_CLIP" ] || { echo "need at least two WAVs besides te-reference.wav in $TESTSET"; return 2; }
}

# run_one <weights> <outdir> <token log or ""> <item> <mode> [ENV=value ...]
run_one() {
    local w="$1" out="$2" log="$3" item="$4" mode="$5"; shift 5
    echo "  $item" >&2
    local rc=0
    env SVE_GEMMA="$w" ${log:+"SVE_TOKEN_LOG=$log"} "$@" bash "${SMOKE:-$ROOT/core/tests/smoke_gemma.sh}" "$mode" \
        >"$out/$item.raw" 2>"$out/$item.err" || rc=$?
    echo "(exit status $rc)" >>"$out/$item.raw"
    sed -E -e '/^---/s/[0-9]+\.[0-9]+ s/<t> s/g' -e 's/("[a-z_]+_at": )[0-9]+/\1<time>/g' "$out/$item.raw" >"$out/$item.txt"
}

# run_item <weights> <outdir> <token log or ""> <item name>
run_item() {
    local w="$1" out="$2" log="$3" item="$4" wav
    case "$item" in
        transcribe-all)
            for wav in "$TESTSET"/*.wav; do
                run_one "$w" "$out" "$log" "transcribe-$(basename "$wav" .wav)" transcribe SVE_TEST_WAV="$wav"
            done ;;
        transcribe-*)
            wav="$TESTSET/${item#transcribe-}.wav"
            [ -f "$wav" ] || { echo "no clip $wav for $item" >&2; return 2; }
            run_one "$w" "$out" "$log" "$item" transcribe SVE_TEST_WAV="$wav" ;;
        notes-te)
            find_clips >&2 || return 2
            run_one "$w" "$out" "$log" notes-te notes SVE_TEST_WAV="$REF" SVE_LANG=te SVE_QUESTION="ఈ రికార్డింగ్ లో ముఖ్యమైన విషయాలు ఏమిటి?" ;;
        notes-en)
            find_clips >&2 || return 2
            run_one "$w" "$out" "$log" notes-en notes SVE_TEST_WAV="$EN_CLIP" SVE_LANG=en SVE_QUESTION="What are the main points of this recording?" ;;
        extract) run_one "$w" "$out" "$log" extract extract ;;
        longnotes)
            find_clips >&2 || return 2
            run_one "$w" "$out" "$log" longnotes longnotes SVE_TEST_WAV="$REF" SVE_LANG=te ;;
        fits) run_one "$w" "$out" "$log" fits fits ;;
        *) echo "unknown item $item" >&2; return 2 ;;
    esac
}

# run_all <weights> <outdir> [token log]: every clip, then notes-te, notes-en, extract, longnotes.
run_all() {
    local w="$1" out="$2" log="${3:-}" item
    rm -rf "$out"; mkdir -p "$out"
    for item in transcribe-all notes-te notes-en extract longnotes; do run_item "$w" "$out" "$log" "$item"; done
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

# compare_dirs <dir a> <dir b> <label a> <label b>: "<item>: IDENTICAL" or "<item>: DIFFERS at line N" with
# both lines, for every item of dir a. Sets COMPARED (items) and DIFFERENT (items that differ).
compare_dirs() {
    local a="$1" b="$2" la="$3" lb="$4" f item g line
    COMPARED=0; DIFFERENT=0
    for f in "$a"/*.txt; do
        [ -e "$f" ] || continue
        item="$(basename "$f" .txt)"; g="$b/$item.txt"; COMPARED=$((COMPARED + 1))
        if cmp -s "$f" "$g"; then
            echo "$item: IDENTICAL"
        else
            DIFFERENT=$((DIFFERENT + 1))
            line="$(cmp "$f" "$g" 2>&1 | sed -nE 's/.* line ([0-9]+).*/\1/p')"
            [ -n "$line" ] || line=1
            echo "$item: DIFFERS at line $line"
            printf '    %-9s%s\n' "$la:" "$(sed -n "${line}p" "$f" | cut -c1-200)"
            printf '    %-9s%s\n' "$lb:" "$(sed -n "${line}p" "$g" 2>/dev/null | cut -c1-200)"
        fi
    done
}
