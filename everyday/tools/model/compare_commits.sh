#!/bin/bash
# Notes parity between two versions of the core: builds the smoke test at a base commit (in a git worktree)
# and at this checkout, runs the named items on the same weights with greedy decoding (SVE_GREEDY=1) and the
# masking of compare_outputs.sh, and prints "<item>: IDENTICAL" or the first difference, then PARITY: PASS|FAIL.
#
#   compare_commits.sh <base commit> <item>...
#
# Items: transcribe-<clip> (a WAV in TESTSET), transcribe-all (every WAV in TESTSET), notes-te, notes-en,
#        extract, longnotes, fits (see compare_lib.sh).
# Env: SVE_GEMMA (weights, required), SVE_MMPROJ (audio projector, required), TESTSET (default
#      tools/model/testset), WORK (output dir, default $TMPDIR/sve-commits; base/ and head/ hold each side's output).
# PASS needs every item to exit 0 with non-empty output on both sides, and to be identical.
# "head" is this checkout's working tree: commit first to compare a commit.
set -eu
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
[ $# -ge 2 ] || { sed -n 2,13p "$0"; exit 2; }
BASE="$1"; shift
ITEMS=("$@")
TESTSET="${TESTSET:-$ROOT/tools/model/testset}"
WORK="${WORK:-${TMPDIR:-/tmp}/sve-commits}"
: "${SVE_GEMMA:?set SVE_GEMMA}"
: "${SVE_MMPROJ:?set SVE_MMPROJ}"
export SVE_MMPROJ SVE_GREEDY=1
. "$ROOT/tools/model/compare_lib.sh"   # run_item, check_run, compare_dirs

base_sha="$(git -C "$ROOT" rev-parse --verify "$BASE^{commit}")"
mkdir -p "$WORK"
SRC="$WORK/base-src"
git -C "$ROOT" worktree remove --force "$SRC" >/dev/null 2>&1 || rm -rf "$SRC"
git -C "$ROOT" worktree add --detach "$SRC" "$base_sha" >/dev/null 2>&1
trap 'git -C "$ROOT" worktree remove --force "$SRC" >/dev/null 2>&1 || true' EXIT

# side <name> <smoke_gemma.sh>: a fresh binary for that side's sources, then every item.
side() {
    local name="$1" item
    SMOKE="$2"
    export SVE_SMOKE_BIN="$WORK/smoke_$name"
    rm -f "$SVE_SMOKE_BIN"
    rm -rf "${WORK:?}/$name"; mkdir -p "$WORK/$name"
    for item in "${ITEMS[@]}"; do run_item "$SVE_GEMMA" "$WORK/$name" "" "$item" || exit 2; done
}

dirty=""; [ -z "$(git -C "$ROOT" status --porcelain --untracked-files=no)" ] || dirty=", uncommitted changes"
echo "base $(git -C "$ROOT" rev-parse --short "$base_sha")" >&2
side base "$SRC/core/tests/smoke_gemma.sh"
echo "head $(git -C "$ROOT" rev-parse --short HEAD)$dirty" >&2
side head "$ROOT/core/tests/smoke_gemma.sh"

failed=0
check_run "$WORK/base" || failed=1
check_run "$WORK/head" "$WORK/base" || failed=1
compare_dirs "$WORK/base" "$WORK/head" base head
echo "$((COMPARED - DIFFERENT))/$COMPARED items identical"
[ "$failed" -eq 0 ] || echo "some runs failed (listed above): identical failures do not count"
if [ "$DIFFERENT" -eq 0 ] && [ "$failed" -eq 0 ] && [ "$COMPARED" -gt 0 ]; then echo "PARITY: PASS"; else echo "PARITY: FAIL"; exit 1; fi
