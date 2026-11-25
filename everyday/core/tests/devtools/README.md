# Desktop dev tools (not part of run.sh)

Small programs used on the Mac against Homebrew llama.cpp and the shipped model (Gemma 4 E4B Q4_K_M) or against a
copy of the phone's database. Built by hand; `run.sh` only builds `core/tests/test_*.cpp`.

Pull the phone's database (iPhone 16e, app id `summaverick.everyday`):

    for f in notes.sqlite notes.sqlite-wal notes.sqlite-shm; do
      xcrun devicectl device copy from --device 00008140-001668892E22801C --domain-type appDataContainer \
        --domain-identifier summaverick.everyday --source "Library/Application Support/Summaverick/$f" --destination "/tmp/phone/$f"
    done

Same for `activity.log` (the processing activity log, JSON lines).

Build a model tool (from the repo root):

    BREW=/opt/homebrew; mkdir -p /tmp/shim; ln -sf $BREW/include /tmp/shim/llama
    clang++ -std=c++17 -O2 -w -I/tmp/shim -I$BREW/include -Icore/include -Icore/src core/tests/devtools/chat_eval.cpp \
      core/src/sve_*.cpp -L$BREW/lib -lllama -lmtmd -lggml -lggml-base -lsqlite3 -Wl,-rpath,$BREW/lib -o /tmp/chat_eval

Store-only tools (map_from_db, vocab_dryrun, clean_dryrun) need the test sources list of run.sh instead of llama.

- chat_eval.cpp: `chat_eval <model.gguf> <fixture.txt> <db>` — Ask "What do I need to do?", the follow-up "Draft a
  message asking him for an update", and the to-do pass on one conversation. Fixtures: core/tests/fixtures/proposal_*.txt.
- verify_eval.cpp: `verify_eval <model.gguf> <fixture.txt> <db> [lang] [--dump]` — a short note as the phone runs it
  (one section, one-pass notes, action items, memories), then the checking agent (sve_verify_note). Prints what the
  first check found ("BEFORE") and what is left after the retries ("AFTER") per output; --dump adds every check/retry
  event and the outputs before and after. SVE_VOCAB="A,B" saves words first (core/tests/fixtures/noisy_meeting.txt
  says them as speech, as note 18 did).
- notes_timing.cpp: one-pass notes on a note of a phone DB copy, CPU 4 threads, token counts and seconds.
- memories_rerun.cpp: reruns the memory pass on notes of a DB copy and prints every dropped candidate and why
  (set DROPS=<file> to save them).
- map_from_db.cpp: builds the knowledge graph from a DB copy (with the one-time people clean-ups).
- vocab_dryrun.cpp / clean_dryrun.cpp: what saved-word correction / piece clean-up would change on a DB copy.

Extraction smoke (meeting): `SVE_EXTRACT_DUMP=1 SVE_FIXTURE=core/tests/fixtures/team_meeting.txt bash core/tests/smoke_gemma.sh extract`
(SVE_GEMMA, SVE_MMPROJ set as in the README). Sampling is temperature 0.3, so judge a prompt change over 2-3 runs.
