# Memory accuracy test

Measures whether the memories the app shows are right, and whether low transcript confidence picks out
pieces that were heard wrong. Python 3 standard library only. Everything private (databases, recordings,
sheets, marks) lives under `tools/memory/private/`, which git ignores.

## 1. Get the data (work on copies)

Copy `Library/Application Support/Summaverick/notes.sqlite` (with `-wal` and `-shm`) and the `recordings/`
folder from the phone with `xcrun devicectl device copy from ...`, as in `tools/model/testset/README.md`.
The stored `audio_path` is a phone path; the tool matches recordings by file name, so just point `--audio`
at the folder holding the `.m4a` files.

## 2. Export the sheet

    export SVE_GEMMA=/path/to/gemma.gguf        # SVE_MMPROJ is not needed here
    python3 tools/memory/testset.py export --db tools/memory/private/notes.sqlite \
        --audio tools/memory/private/recordings --out tools/memory/private/run1 [--target 100] [--pieces 60]

It copies the database (the original is never opened for writing), then takes notes newest first and runs
`core/tests/smoke_gemma.sh memories` on each (on the copy) until at least `--target` memories are shown.
Shown means: stored, queued or not, not a medicine one, not already reviewed as wrong. Models run on the CPU,
so a long recording takes a while. `--skip-run` skips the smoke and uses the memories already in the copy.

Output in `--out`: `sheet.html` (open it in Safari or Chrome), `audio/` (the recordings it uses),
`notes.sqlite` (the finished copy; this is what `score` reads) and its sha256, printed.

The smoke only reads transcript pieces, so no WAVs are needed; the audio is for playback in the sheet.

### Marking (30-40 minutes)

- Part 1: each memory has its recording, played from the quote. Mark Right or Wrong; for Wrong choose
  why (wrong words heard, wrong meaning, wrong person, wrong date or number). Each recording has a box for
  memories it said that are missing, one per line.
- Part 2: sampled transcript pieces, evenly across conf deciles (null conf has its own group). Mark
  heard right or heard wrong.
- Keys: J/K next or previous card, R right, W wrong, 1-4 the reason, H/G heard right/wrong.
- Marks are saved in the browser as you go, so a reload loses nothing. Press Save marks to download `marks.json`.

### Next round

Pass `--previous-marks marks.json` to `export`. A memory whose text and quote are identical to a marked one
keeps that mark (shown as carried over; change it if needed). Pieces carry over by identical text.

## 3. Score

    python3 tools/memory/testset.py score --db tools/memory/private/run1/notes.sqlite --marks marks.json

It refuses a `marks.json` made for a different database (sha256 check). It prints:

- `SHOWN-CORRECT: <right>/<marked> = <p>%  GATE: PASS|FAIL (>= 95%)`; passing also needs 100 marked memories.
  Then right/marked per type, and the count of wrong memories per reason.
- `MISSED (reported)`: non-empty lines in the missed-memories boxes.
- The confidence gate: AUC (Mann-Whitney, ties count half) of low conf picking out "heard wrong" pieces;
  the best threshold t among the decile boundaries of the marked pieces (a piece is flagged when conf < t),
  preferring a t that flags at least 50% of wrong and at most 20% of right pieces; then
  `CONF-GATE: PASS|FAIL`. Passing needs AUC >= 0.70, such a t, at least 60 marked pieces and at least 8 wrong.
  Pieces with null conf are counted separately and left out.

## Self-test

    python3 tools/memory/testset.py selftest

Builds a tiny database (3 shown memories plus a medicine one and a wrong-reviewed one, 4 pieces), scores
fake marks and checks `SHOWN-CORRECT: 2/3 = 66.7%  GATE: FAIL`, the missed count, AUC 1.000, the tie rule
in AUC, and the null-conf decile sampling.
