# Fast Notes (Shruti transcripts, one-pass notes) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** After Stop, Shruti writes the transcript in about a fifth of the recording's length; Gemma writes the
title, summary, notes and questions in one pass when the transcript fits one prompt. Gemma still fills any piece
Shruti fails on.

**Architecture:**
- **Shruti** (sherpa-onnx C API, CPU) runs from Swift in a new `ShrutiRecognizer`. The core plans the pieces with
  its existing pause cuts (`sve::plan_group`) and reads the samples; Swift decodes them.
- **The transcript loop** reuses the Whisper build's machinery in `TranscriptRun` (cursor saved after each piece,
  pending pieces, `fillPending` by Gemma). The Whisper call site in `ProcessingPipeline.transcribe` is replaced by
  Shruti. Whisper's code stays compiled but is no longer called from the pipeline.
- **One-pass notes:** a new core call writes all four parts in one generation with fixed headers; a pure parser
  splits them. Any failure falls back to the stepped path, which stays unchanged.

**Tech Stack:**
- C++17 core (llama.cpp, SQLite); tests via `bash core/tests/run.sh`.
- SwiftUI iOS app; sherpa-onnx 2.0.1 iOS xcframework (k2-fsa, Apache-2.0, onnxruntime static).
- Foundation-only swiftc checks in the scratchpad harness style (`scratchpad/whisper_check/run.sh`).

**Spec:** `docs/superpowers/specs/2026-11-13-fast-notes-shruti-design.md` (approved 2026-11-13).

**Spike source:** branch `spike/shruti-telugu`, commits 63e5a55 and 9a9a74b. Take
`ios/get-sherpa-onnx-xcframework.sh`, the sherpa-onnx parts of `ios/make_xcodeproj.py` and `.gitignore`, and the
recognizer setup in `ShrutiBench.swift` (`makeRecognizer`, `decode`). Do not take the bench itself
(`ShrutiBench.run`, `debugShrutiBench`, `debugFreeModelsForBench`, `sve_shruti_bench.*`).

## Global Constraints

- **No model on the Mac** (owner's battery rule): core tests and swiftc checks load no model. Timing and quality
  are measured only on the iPhone 16e (device `00008140-001668892E22801C`).
- **No install, no push without the owner.** Commit on `feature/personal-assistant`. Installing kills the app:
  before every install, confirm the owner isn't recording.
- **Voice:** `docs/superpowers/2026-11-09-voice-guide.md`. No AI wording; no "Shruti", "model" or "AI" in the
  person's view. This plan adds no user-facing strings.
- **Medicine rule unchanged:** no medicine or health answer is written from transcripts.
- **Shruti files** (from Hugging Face, udgamlabs Shruti ONNX repo, folder `te/sherpa-onnx/`; Apache-2.0):

  | File | Bytes | SHA-256 |
  |---|---|---|
  | preprocess.onnx | 912310 | 859b1bac985a6c0a9bff5b0ee520779e5ed14adbdcbe9188c664d3e918fb2203 |
  | encode.onnx | 19637087 | 2bd36f1170a42b0a842c81a1e8118811b166488a686223d0a195b57e7fb71642 |
  | uncached_decode.onnx | 421454 | bb50aaeffa579692be00448ec51ba24b73589864fe7e2c42cf3f1c6cf991b510 |
  | cached_decode.onnx | 32801767 | 695e071dc484120457cec7cabf6d78addafe7b7d63ba64301af3a278c3744701 |
  | tokens.txt | 120752 | aead59cb98514cdc031c51ca0ed3dd2375e85fe81c05844bdbbf4f4b20505e32 |

  A verified copy is in `scratchpad/shruti/model-te/`.
- **Recognizer settings** (from the spike): Moonshine config, `greedy_search`, sample rate 16000, feature dim 80,
  provider `cpu`.
- **Pieces:** `sve::plan_group` (at most 28 s, cut at the quietest point between 4 and 10 s), the same as
  `TranscriptSidecar.pieceSeconds`.
- **Threads:** 4 at heat level full or eased, 2 at minimal.
- **Verification for every task:**
  - `bash core/tests/run.sh` ends with `0 failed`;
  - `cd ios && xcodebuild -project SummaverickEveryday.xcodeproj -scheme SummaverickEveryday -destination 'generic/platform=iOS' -derivedDataPath /private/tmp/claude-501/-Users-sumanth-bolle-Downloads-summaverick-everyday-ios/0a2e198b-c172-4f76-84bc-6ed75626fdb9/scratchpad/dd-dev -allowProvisioningUpdates build` prints `** BUILD SUCCEEDED **`.

**Rulings against the spec:**
1. **Acknowledgements.** The app has no acknowledgements screen. The Apache-2.0 notice ships as a bundled `NOTICE`
   file next to the Shruti files and a credit in `docs/ARCHITECTURE.md`. Cost if wrong: one small screen later.
2. **Sections stay.** The one-pass path replaces the title and the summary/notes/questions steps, not the section
   summaries: chapters, memories and Ask read them. Cost if wrong: the 8-minute aim may be missed by the section
   time, which device check 2 measures and reports.
3. **Heat rests for Shruti.** The ladder's 20 s floor at minimal would make a 5 s Shruti piece wait 20 s, four
   times its work. Shruti pieces rest as long as they worked, with no floor; paused still pauses. The ladder for
   Gemma is unchanged.
4. **Live Shruti ships off.** It is behind `LiveShruti.isOn` (default false) until device check 4 passes and the
   owner says to turn it on (spec §3).
5. **Whisper stays in the build, uncalled.** Removing whisper.cpp from the build is a separate cleanup for the
   owner to ask for.

## Review Focus

1. **Shruti files missing or wrong size** (a build made without running the fetch script): no crash; every piece
   is Gemma's, as today. Test in Task 2 (`ShrutiFiles.verdict`) and Task 3 (`FirstPassFlow` with not ready).
2. **A piece Shruti returns "" for while there's speech:** it becomes a pending piece and Gemma fills it. Silence
   gives "" and no pending piece. Test in Task 2 (`ShrutiPiece.needsGemma`).
3. **The app killed mid-transcript:** the next run continues from the saved cursor, and pieces already done aren't
   redone. Covered by the existing sidecar save after each piece; Task 3's flow test checks the cursor rule.
4. **Gemma's one-pass answer missing a header or a part empty:** nothing is stored from it and the stepped path
   writes the notes. Test in Task 4 (`split_all`).
5. **Gemma loaded while Shruti is loaded** (Ask during a transcript): Shruti is unloaded first, and the transcript
   goes on with Shruti loading again (under 1 s). Test in Task 3 (`FirstPassFlow` after an unload).

---

### Task 1: Core: plan the next piece and read its samples

**Files:**
- Create: `core/src/sve_pieces.cpp`; Modify: `core/include/sve_core.h` (declarations, always present)
- Modify: `core/tests/run.sh` (add `sve_pieces` to the SRCS list); Create: `core/tests/test_pieces.cpp`
- Modify: `ios/make_xcodeproj.py` if new core files must be listed

**Interfaces:**
- Produces (C, in `sve_core.h`):
  - `int sve_plan_next_piece(const char *wav_path, int64_t start_sample, int is_final, int64_t *piece_end);`
    returns 0 piece (sets `*piece_end`, the group's `end_sample`), 1 wait, 2 end, -1 unreadable WAV. Uses
    `sve::plan_group` on the WAV's samples.
  - `int64_t sve_read_samples(const char *wav_path, int64_t start, int64_t end, float **out);` mono floats in
    [-1, 1), returns the count (free with `sve_free_samples`), -1 when not a 16 kHz 16-bit WAV.
  - `void sve_free_samples(float *p);`
  - Both read a WAV still being written (the size read at call time), as `sve_whisper_next_piece` does.

- [ ] **Step 1: Write `test_pieces.cpp`** with a WAV written by the test into a temp file:
  - 60 s of a 440 Hz tone with 1 s silences every 8 s, `is_final=1`, start 0: returns 0 and
    `0 < piece_end <= 28*16000`.
  - Looping from the returned ends reaches 2 (end) and the pieces cover the file with no gap.
  - 20 s file, `is_final=0`: returns 1 (wait).
  - 3 s file, `is_final=1`: one piece to the end, then 2 (Review Focus: short recording).
  - All-zero 30 s file, `is_final=1`: pieces then end, no endless loop.
  - `sve_read_samples` on `[16000, 32000)` returns 16000 samples; a text file returns -1.
- [ ] **Step 2: Run** `bash core/tests/run.sh`. Expected: `test_pieces` fails to link.
- [ ] **Step 3: Implement** `sve_pieces.cpp` reusing the WAV reader already used by `sve_whisper_next_piece`
  (move it to a shared helper if it is file-local).
- [ ] **Step 4: Run** the core tests and the device build. Expected: `0 failed`, `** BUILD SUCCEEDED **`.
- [ ] **Step 5: Commit.** `feat(core): plan the next piece of a WAV and read its samples`

### Task 2: Shruti in the app (framework, files, recognizer, pure rules)

**Files:**
- Create: `ios/get-sherpa-onnx-xcframework.sh` (from the spike, comments no longer say spike-only)
- Create: `ios/get-shruti-model.sh`: downloads the five files to `ios/SummaverickEveryday/Resources/Shruti/`,
  checks each size and SHA-256 from Global Constraints, fails loudly on a mismatch. `--from <dir>` copies from a
  local folder instead (for `scratchpad/shruti/model-te/`).
- Create: `ios/SummaverickEveryday/Resources/Shruti/NOTICE` (Apache-2.0, credit to udgamlabs, link to the
  Hugging Face collection; committed). The `.onnx` and `tokens.txt` files are gitignored.
- Modify: `ios/make_xcodeproj.py` (link and embed `Frameworks/SherpaOnnxC.xcframework` as in the spike; add
  `Resources/Shruti` as a folder resource), `.gitignore`, `ios/SummaverickEveryday/Bridging.h`
  (`#include <sherpa-onnx/c-api/c-api.h>` as the spike does, and the Task 1 declarations come via `sve_core.h`)
- Create: `ios/SummaverickEveryday/Engine/ShrutiRecognizer.swift`
- Modify: `ios/SummaverickEveryday/Engine/ModelLogic.swift` (pure enums below)
- Test: new swiftc check `scratchpad/shruti_check/` (`run.sh` compiles `ModelLogic.swift`,
  `TranscriptSidecar.swift`, `main.swift`, like `whisper_check`)

**Interfaces:**
- Produces (pure, `ModelLogic.swift`):
  - `enum ShrutiFiles { static let names: [String]; static let bytes: [String: Int64]; static func verdict(sizes: [String: Int64?]) -> Verdict }`
    with `enum Verdict { case ok, missing, bad }`: any nil size is missing; any size not equal to `bytes` is bad.
  - `enum ShrutiThreads { static func count(_ level: HeatLadder.Level) -> Int }`: 4 for full and eased, 2 for
    minimal and paused.
  - `enum ShrutiPiece { static func needsGemma(text: String, peak: Float) -> Bool }`: true when the trimmed text is
    empty and `peak >= 0.02`.
  - `HeatLadder.lightRestSeconds(_ l: Level, lastWork: Double) -> Double`: full 0, eased `min(5, lastWork)`,
    minimal `min(minimalRestCap, lastWork)`, paused `.infinity` (ruling 3).
- Produces (`ShrutiRecognizer.swift`, a `final class` with a serial queue, `static let shared`):
  - `var ready: Bool` (`ShrutiFiles.verdict` on the bundle's files is `.ok`, checked once per launch)
  - `func load(threads: Int) throws` (no-op when loaded with the same threads; logs load seconds)
  - `func unload()`
  - `func decode(_ samples: [Float]) throws -> String` (the spike's decode: all-silent input gives "")
  - Every log line starts `[sve] shruti`.

- [ ] **Step 1: Write `shruti_check/main.swift`:** `verdict` with all sizes right is `.ok`, one nil is `.missing`,
  one off by a byte is `.bad`; `ShrutiThreads.count(.full) == 4`, `(.minimal) == 2`;
  `needsGemma(text: "", peak: 0.3) == true`, `(text: "", peak: 0.001) == false`, `(text: "ok", peak: 0.3) == false`;
  `lightRestSeconds(.minimal, lastWork: 5) == 5`, `(.eased, lastWork: 2) == 2`, `(.full, lastWork: 9) == 0`.
  Prints `shruti_check OK`.
- [ ] **Step 2: Run** `sh scratchpad/shruti_check/run.sh`. Expected: compile errors on `ShrutiFiles`.
- [ ] **Step 3: Implement** the pure enums, the scripts, the project changes and `ShrutiRecognizer`. Run
  `ios/get-sherpa-onnx-xcframework.sh`, `ios/get-shruti-model.sh --from <scratchpad>/shruti/model-te`, then
  `python3 ios/make_xcodeproj.py`.
- [ ] **Step 4: Run** the check, the core tests and the device build. Expected: `shruti_check OK`, `0 failed`,
  `** BUILD SUCCEEDED **`; the built `.app` contains `Shruti/encode.onnx` (`ls` the product).
- [ ] **Step 5: Commit.** `feat(ios): Shruti speech files and recognizer in the app`

### Task 3: Shruti writes the transcript after Stop

**Files:**
- Modify: `ios/SummaverickEveryday/Engine/LiveTranscriber.swift` (`TranscriptRun.runShruti`)
- Modify: `ios/SummaverickEveryday/Engine/ProcessingPipeline.swift` (`transcribe`, about lines 1005–1092; the
  debug re-transcribe hook near line 3017)
- Modify: `ios/SummaverickEveryday/Engine/ModelManager.swift` (`loadIntoCore(backend:)` unloads Shruti first)
- Modify: `ios/SummaverickEveryday/Engine/ModelLogic.swift` (rename `WhisperFlow` to `FirstPassFlow`, add
  `ShrutiChoice`)
- Test: extend `scratchpad/shruti_check/main.swift`; update `scratchpad/whisper_check/main.swift` for the rename

**Interfaces:**
- Consumes: Task 1's C calls; Task 2's `ShrutiRecognizer`, `ShrutiThreads`, `ShrutiPiece`, `lightRestSeconds`;
  the existing `appendPending`, `pendingIndices`, `fillPending`.
- Produces:
  - `enum ShrutiChoice { static func use(shrutiReady: Bool, heatPaused: Bool) -> Bool }`: true when ready and not
    paused. No background check: Shruti is CPU only and runs locked.
  - `enum FirstPassFlow { enum Step: Equatable { case shruti, gemmaRemainder, fillPending, done }; static func next(shruti: Bool, cursorAtEnd: Bool, pending: Int) -> Step }`
    with the same rules as today's `WhisperFlow.next`.
  - `TranscriptRun.runShruti(audioPath: String, isFinal: Bool, shouldStop: @escaping () -> Bool, threads: () -> Int, beforePiece: () async -> Bool, onPiece: (TranscriptSidecar) -> Void) async throws -> (TranscriptSidecar, Stop)`:
    the `runWhisper` loop shape. Per piece: `sve_plan_next_piece`; `.wait` and `.end` return; read the samples,
    `ShrutiRecognizer.shared.load(threads:)`, decode; `needsGemma` gives `appendPending`, else `append` with
    `conf: nil`; save; `onPiece`. Logs `[sve] shruti piece N: X s audio in Y ms, Z chars`.
  - `ProcessingPipeline.transcribe` loops on `FirstPassFlow.next`. The heat gate before each Shruti piece uses
    `lightRestSeconds` with the last piece's work time (add a `light: Bool` parameter to `heatGate`). A thrown
    Shruti error logs and sets the note to `.gemmaRemainder` from the saved cursor. `.done` unloads Shruti.
  - The debug hook becomes `SVE_RETRANSCRIBE=<id,id>`: for each note, clears its sidecar copy in tmp and runs the
    Shruti path on its audio, writing `tmp/retranscribe-<id>.txt` and one timing line. DEBUG only.

- [ ] **Step 1: Extend the check:** `FirstPassFlow.next(shruti: true, cursorAtEnd: false, pending: 0) == .shruti`;
  `(false, false, 0) == .gemmaRemainder` (Review Focus 1); `(true, true, 2) == .fillPending`;
  `(true, true, 0) == .done`; `ShrutiChoice.use(shrutiReady: true, heatPaused: true) == false`;
  `use(true, false) == true`; `use(false, false) == false`.
- [ ] **Step 2: Run** both checks. Expected: `FirstPassFlow` undefined.
- [ ] **Step 3: Implement.** Keep the log line `[sve] note %lld transcript: %@` for each flow step.
- [ ] **Step 4: Run** the checks, the core tests and the device build. Expected: `shruti_check OK`,
  `whisper_check OK`, `0 failed`, `** BUILD SUCCEEDED **`.
- [ ] **Step 5: Commit.** `feat(ios): Shruti writes the transcript after Stop, Gemma fills what it misses`

### Task 4: One-pass notes

**Files:**
- Modify: `core/src/sve_prompts.h` (the one-pass prompt), `core/src/sve_core.cpp` or `sve_llm.cpp` (the call),
  `core/include/sve_core.h`
- Create: `core/src/sve_onepass.cpp`, `core/src/sve_onepass.h` (pure parser); Create: `core/tests/test_onepass.cpp`;
  Modify: `core/tests/run.sh`
- Modify: `ios/SummaverickEveryday/Engine/CoreEngine.swift` (`writeAll`),
  `ios/SummaverickEveryday/Engine/ProcessingPipeline.swift` (`generateOutputs`, about lines 1404–1453)

**Interfaces:**
- Produces:
  - `namespace sve { struct AllParts { std::string title, summary, notes, questions; }; std::optional<AllParts> split_all(const std::string &text); }`:
    the text holds the lines `### TITLE`, `### SUMMARY`, `### NOTES`, `### QUESTIONS` in that order, each
    followed by its part. Text before the first header is ignored. Any header missing, out of order, or followed
    by an empty part gives `nullopt`. Parts are trimmed.
  - `int sve_all_fits(sve_ctx *ctx, const char *transcript);`: 1 when the transcript plus an output budget of
    1100 tokens fits the same context budget `sve_generate` uses, 0 otherwise, -1 with no model.
  - `char *sve_generate_all(sve_ctx *ctx, const char *transcript, const char *lang, <the same progress and stop callbacks as sve_generate>);`
    returns JSON `{"title":…,"summary":…,"notes":…,"questions":…}` or NULL when `split_all` fails. The prompt
    asks for the same content and language rules as the three existing output prompts, under the four headers.
  - `CoreEngine.writeAll(transcript: String, lang: String, shouldStop: @escaping () -> Bool, progress: @escaping (Double) -> Void) async throws -> (title: String, summary: String, notes: String, questions: String)?`
    and `CoreEngine.allFits(_ transcript: String) async -> Bool`.
  - In `generateOutputs`, after the sections: when `allFits`, all three outputs are still to write, the note is
    not a refill, and `setTitle` is true, call `writeAll`. On success store the title and the three outputs, post
    the summary-ready and notes-ready notices as the stepped path does, and log
    `[sve] note %lld: notes in one pass, %.0f s`. On nil, log `[sve] note %lld: one pass did not split; notes in steps`
    and run the stepped path unchanged.
  - DEBUG only: the environment `SVE_ONEPASS=0` skips the one pass (for device check 3).

- [ ] **Step 1: Write `test_onepass.cpp`:** a full text gives all four parts trimmed; preamble before
  `### TITLE` is ignored; missing `### QUESTIONS` gives nullopt; an empty `### NOTES` part gives nullopt; headers
  out of order give nullopt; Telugu text inside a part survives byte for byte.
- [ ] **Step 2: Run** `bash core/tests/run.sh`. Expected: `test_onepass` fails to link.
- [ ] **Step 3: Implement** the parser, the prompt, the two C calls and the Swift wiring.
- [ ] **Step 4: Run** the core tests and the device build. Expected: `0 failed`, `** BUILD SUCCEEDED **`.
- [ ] **Step 5: Commit.** `feat: one-pass notes for recordings that fit one prompt`

### Task 5: Live Shruti while recording (off by default)

**Files:**
- Modify: `ios/SummaverickEveryday/Engine/ModelLogic.swift` (`enum LiveShruti`)
- Modify: `ios/SummaverickEveryday/Engine/LiveTranscriber.swift` (the live loop near line 403)
- Test: extend `scratchpad/shruti_check/main.swift`

**Interfaces:**
- Consumes: Task 3's `runShruti(isFinal: false, …)`.
- Produces:
  - `enum LiveShruti { static let key = "live.shruti"; static func isOn(shrutiReady: Bool, defaults: UserDefaults = .standard, environment: [String: String] = ProcessInfo.processInfo.environment) -> Bool }`:
    false unless `shrutiReady`; then `SVE_LIVE_SHRUTI` "1"/"0" wins, else the defaults key, else false.
  - When on, the live session runs `runShruti(isFinal: false)` instead of Gemma's `run`; `.wait` waits for more
    audio as the Gemma live loop does. Live pieces are final; after Stop only the rest is transcribed. Gemma is
    never loaded by the live path when Shruti is on.
  - A heat log line every 5 minutes of recording: `[sve] live shruti: thermal %ld, %d pieces, lag %.0f s`.

- [ ] **Step 1: Extend the check:** not ready gives false whatever the switches; ready and env "1" gives true;
  ready, no env, no key gives false; ready, env "0", key true gives false.
- [ ] **Step 2: Run** the check. Expected: `LiveShruti` undefined.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** the checks, the core tests and the device build. Expected: all pass.
- [ ] **Step 5: Commit.** `feat(ios): Shruti can transcribe while recording (off until checked on the phone)`

### Task 7: Notes first, sections and memories after (owner, 2026-11-13)

Owner, 2026-11-13: "yes, defer sections and memories, go ahead" (after note 39 took 16 min from Stop to notes,
5 min of it section summaries). This overrides ruling 2 for notes that take the one pass.

**Files:**
- Modify: `ios/SummaverickEveryday/Engine/ProcessingPipeline.swift` (`generateOutputs`)
- Modify: `ios/SummaverickEveryday/Engine/ModelLogic.swift` (pure `enum NotesOrder`)
- Test: extend `scratchpad/shruti_check/main.swift`

**Interfaces:**
- Consumes: Task 4's one-pass gate inputs (`allFits`, all three outputs still to write, `setTitle`, `!refill`,
  `onePassAllowed`) and `writeAll`.
- Produces:
  - `enum NotesOrder { enum Order: Equatable { case notesFirst, sectionsFirst }; static func order(onePassEligible: Bool, sectionsStored: Bool) -> Order }`:
    `.notesFirst` when eligible and the note has no stored sections yet; else `.sectionsFirst` (today's order).
  - With `.notesFirst`, `generateOutputs` runs: extract (unchanged, first), the one pass, store and the
    summary-ready and notes-ready notices, Live Activity ready, `notesStored = true`; then the section summaries
    (`writeSections` and what follows it today: chapters, `markRewrittenSections`), then memories and insight, then done.
  - If the one pass fails on the `.notesFirst` path, the sections are written and the stepped path runs exactly as
    today (the note never ends with no notes).
  - A cut-off or expiry after the notes are stored leaves sections and memories due, picked up by the existing
    resume / idle / overnight work (the same path that finishes memories today).
  - Log `[sve] note %lld: notes first, sections after` when `.notesFirst` is taken.

- [ ] **Step 1: Extend the check:** `order(onePassEligible: true, sectionsStored: false) == .notesFirst`;
  `(true, true) == .sectionsFirst`; `(false, false) == .sectionsFirst`.
- [ ] **Step 2: Run** the check. Expected: `NotesOrder` undefined.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** the checks, the core tests and the device build. Expected: all pass.
- [ ] **Step 5: Commit.** `feat(ios): notes first, sections and memories after, for one-pass notes`

### Task 6: Device checks on the iPhone 16e (owner present)

**Files:** fixes where a check fails; `docs/ARCHITECTURE.md` (Shruti section, history entry, credit);
memory file `telugu-asr-gemma4.md` (results).

- [ ] **Step 1:** Ask the owner to stop any recording; install the build.
- [ ] **Step 2 (spec check 1):** `SVE_RETRANSCRIBE=22,25,34,38`. Record per note: audio seconds, transcript
  seconds, pieces, pending pieces, thermal at start and end. Copy the texts to the scratchpad for the owner to read.
- [ ] **Step 3 (spec check 2):** the owner records 10 minutes. Record: Stop to transcript, transcript to summary,
  to notes, total rest seconds (sum of `resting` lines). Report against the aims (2 min, 8 min).
- [ ] **Step 4 (spec check 3):** notes 22 and 38 written by one pass and by steps (a DEBUG env
  `SVE_ONEPASS=0` forces steps); the owner compares topics and question counts.
- [ ] **Step 5 (spec check 4):** with `SVE_LIVE_SHRUTI=1`, a 30-minute recording: thermal every 5 min and
  transcript time at Stop. Report; the owner decides whether live goes on by default.
- [ ] **Step 6 (spec check 5):** a build without the Shruti files (`Resources/Shruti` emptied): a short recording
  gets a Gemma transcript and notes.
- [ ] **Step 7:** Update `docs/ARCHITECTURE.md` and the memory file. Commit.
  `docs: Shruti transcripts and one-pass notes, with the phone results`
