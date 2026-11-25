# Telugu Transcripts with Whisper Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** After Stop, Whisper (Telugu fine-tune) writes the transcript in pieces cut at pauses. Pieces it isn't
sure are Telugu are written by Gemma. If Whisper is missing, everything is exactly as today.

**Architecture:**
- **Pure parts in the core:**
  - pause cuts and grouping into pieces of about 28 s or less;
  - the repeat cleanup;
  - the Telugu-or-Gemma choice.
- **Whisper decoding:** the core's existing ASR slot in `sve_ctx` (`ctx->whisper`, `SVE_WITH_WHISPER`) is
  extended and turned on for iOS. It returns pieces in the same JSON shape as `sve_transcribe_next_piece`, so the
  sidecar cursor (saved after every piece, never redone) keeps working unchanged.
- **Unsure pieces:** each one is saved as a *pending* sidecar piece. When Whisper is done and freed, Gemma fills
  the pending pieces through a new exact-span call. Then the notes are written as today.

**Tech Stack:**
- C++17 core with llama.cpp, whisper.cpp (MIT) and SQLite; tests via `bash core/tests/run.sh`.
- SwiftUI iOS app; Foundation-only swiftc checks in the scratchpad harness style (`steps_check/run.sh`).

**Spec:** `docs/superpowers/specs/2026-11-12-whisper-telugu-transcripts-design.md` (approved 2026-11-12).

**Source of the whisper.cpp files:** branch `spike/whisper-telugu`, commit 029a2de. Take them as they are:
- `core/include/whisper.h`
- `core/src/whisper.cpp`, `core/src/whisper-arch.h`, `core/src/whisper-logging.h`, `core/src/ggml-cpp.h`
- `core/src/aneforge/whisper-aneforge.h`
- the stubs at the top of `core/src/sve_whisper_bench.cpp`

The bench itself (`sve_whisper_bench.*`, `WhisperBench.swift`, the `App.swift` hook) is **not** taken.

## Global Constraints

- **No model on the Mac** (the owner's battery rule):
  - Core tests use no model file.
  - Timing and quality are measured only on the iPhone 16e (device `00008140-001668892E22801C`).
- **No install, no push without the owner.** Commit on `feature/personal-assistant`.
  - Installing kills the app, so before every install, confirm the owner isn't recording.
- **Voice:** follow `docs/superpowers/2026-11-09-voice-guide.md`. No AI wording. This plan adds no user-facing
  strings. If one turns out to be needed, it goes in `Localizable.xcstrings` with English and Telugu.
- **One model at a time:** Whisper (about 1.1 GB) and Gemma (about 4.9 GB) are never loaded together.
- **Decoding settings (spec §2.3):**
  - greedy decoding;
  - `no_timestamps = true`;
  - `temperature_inc = 0`;
  - `no_context = true`;
  - `audio_ctx = min(1500, samples*50/16000 + 64)`;
  - `language = "te"`.
- **Pieces at pauses:**
  - 20 ms frames (320 samples), loudness smoothed over ±7 frames;
  - each span is 4–10 s and ends at the quietest frame;
  - a group of spans is at most 28 s, the same as `TranscriptSidecar.pieceSeconds`.
- **Repeat cleanup:** a word or 2–3-word phrase repeated more than 3 times in a row becomes one. A piece that is
  nothing but such a run becomes "".
- **Model file:**
  - `ggml-te-medium-q8_0.bin`, 823369779 bytes;
  - SHA-256 `0f71156c1c043ae209d1ef1405e5d88df1c1ab7c13a4e9b7640df477a698c3ad`;
  - from vasista22/whisper-telugu-medium (Apache-2.0);
  - hosted on the owner's Hugging Face (spec, owner's answer 1).
- **whisper.cpp at -O3:** `whisper.cpp` is always compiled with the spike's `FAST_CPP` flags in
  `ios/make_xcodeproj.py`. Debug -O0 is about 10× slower.
- **Verification for every task:**
  - `bash core/tests/run.sh` ends with `0 failed`;
  - the device build `cd ios && xcodebuild -project SummaverickEveryday.xcodeproj -scheme SummaverickEveryday -destination 'generic/platform=iOS' -derivedDataPath /private/tmp/claude-501/-Users-sumanth-bolle-Downloads-summaverick-everyday-ios/0a2e198b-c172-4f76-84bc-6ed75626fdb9/scratchpad/dd-dev -allowProvisioningUpdates build` prints `** BUILD SUCCEEDED **`.

**Rulings against the spec:**
1. **The step list (spec §5).** Engine v2's `jobs` table was never built (its own plan ruled steps are derived from
   stored results). So the step list here is the sidecar: one Whisper call per group, saved as it ends.
   - Cost if wrong: none for the owner. The resume rule is the same.
2. **The core API (spec call 5).** Whisper reuses `sve_ctx`'s existing ASR slot instead of a new context type.
   - Why: the slot, `sve_load_asr` and `sve_unload_asr` already exist.
   - Cost if wrong: a rename.
3. **Mixed groups.** A group where any span is unsure goes to Gemma whole (at most 28 s).
   - Why: the transcript stays in order, and the pending pieces stay simple.
   - Cost: a few seconds of Gemma on mixed groups.
4. **The swap rule (spec call 4).** Whisper loads in about 4–5 s (measured 3.8–4.9 s), so the more-than-30-s rule
   never fires. Only the swap times are logged.

## Review Focus

1. **A recording that is all silence or music:** every group gives "" text and the cursor reaches the end. There's
   no stuck loop and no pending piece. Test in Task 1 (`plan_group` on zeros) and Task 3 (sidecar appends "").
2. **Ask tapped mid-transcript:** Gemma loads for Ask (Whisper freed), and the transcript continues with Whisper
   afterwards from the saved cursor. Pieces already done aren't redone. Test in Task 4 (the `WhisperFlow` check).
3. **The app killed between Whisper finishing and Gemma filling the pending pieces:** on the next run the pending
   pieces are found and filled, and the notes are still written. Test in Task 3 (`pendingIndices` survives a
   save and load).
4. **A truncated or wrong model file** (download cut off, disk full): it's never loaded. It's deleted and fetched
   again, and transcripts use Gemma meanwhile. Test in Task 5.
5. **A recording under 4 s, or a tail under 4 s:** one short group, not a zero-length span or a crash. Test in
   Task 1.

---

### Task 1: Pure Whisper helpers in the core (cuts, groups, cleanup, choice)

**Files:**
- Create: `core/src/sve_whisper_text.h`, `core/src/sve_whisper_text.cpp`
- Modify: `core/tests/run.sh` (add `sve_whisper_text` to the SRCS list)
- Test: `core/tests/test_whisper_text.cpp`

**Interfaces:**
- Produces (namespace `sve`):
  - `struct Span { int64_t start, end; };` (samples, 16 kHz)
  - `struct Group { enum Status { Piece, Wait, End } status; std::vector<Span> spans; int64_t end_sample; };`
  - `Group plan_group(const float *pcm, int64_t n_samples, int64_t start_sample, bool is_final);`
  - `std::string collapse_repeats(const std::string &text, int max_repeat = 3);`
  - `enum class LangChoice { Telugu, Gemma };`
  - `LangChoice lang_choice(float p_te, float p_en, float min_te = 0.6f, float margin = 0.3f);`

- [ ] **Step 1: Write the failing tests** in `test_whisper_text.cpp`, using `check.h` like the other tests:
  - **Zeros:** `plan_group` over 60 s of zeros from 0, final → `Piece`; every span is 4–10 s; the spans are
    contiguous from 0; `end_sample` ≤ 28·16000.
  - **Quietest point:** 30 s of a 0.5-amplitude 200 Hz tone with silence at 6.0–6.4 s → the first span ends
    within 6.0–6.4 s (±320 samples).
  - **Not enough audio yet:** not final, 20 s available after start → `Wait`.
  - **Nothing left:** final, 0.3 s left → `End`.
  - **Short recording:** final, 2.5 s total → `Piece` with one span [0, 40000).
  - **Short tail:** final, 31 s total → the second group is one span of about 3 s, with no zero-length span.
  - `collapse_repeats("టిక్ టిక్ టిక్ టిక్ టిక్ ఇది") == "టిక్ ఇది"`
  - `collapse_repeats("a b a b a b a b c") == "a b c"`
  - `collapse_repeats("one one one two") == "one one one two"` (3 repeats is allowed)
  - `collapse_repeats("టిక్ టిక్ టిక్ టిక్ టిక్") == ""`
  - `lang_choice(0.9f, 0.05f) == Telugu`
  - `lang_choice(0.55f, 0.4f) == Gemma`
  - `lang_choice(0.7f, 0.45f) == Gemma`

- [ ] **Step 2: Run it to make sure it fails.** Run `bash core/tests/run.sh`. Expected: `BUILD FAILED: …test_whisper_text.cpp`.

- [ ] **Step 3: Implement the helpers in `sve_whisper_text.cpp`.**
  - The cut rule is the spike's (029a2de, `sve_whisper_bench.cpp` lines for `en`/`sm`/`best`).
  - Groups keep adding spans while the total is ≤ 28 s.
  - Not final: `Wait` when fewer than (28+3) s are available after start, the same rule as Gemma.
  - Final: `End` when less than 0.5 s is left.
  - `collapse_repeats` works on whitespace-split words, for phrase lengths 1–3. The result is joined with single
    spaces.

- [ ] **Step 4: Run the tests and make sure they pass.** Run `bash core/tests/run.sh`. Expected: it ends with
  `0 failed`, and the new test's cases are counted.

- [ ] **Step 5: Commit.** Message: `feat(core): pause cuts, groups, repeat cleanup and language choice for Whisper`

### Task 2: whisper.cpp in the iOS core, the Whisper piece call, and Gemma's exact-span call

**Files:**
- Add from the spike (029a2de): `core/include/whisper.h`, `core/src/whisper.cpp`, `core/src/whisper-arch.h`,
  `core/src/whisper-logging.h`, `core/src/ggml-cpp.h`, `core/src/aneforge/whisper-aneforge.h`.
- Create: `core/src/sve_whisper_stubs.cpp`, holding the three ANEForge stubs from the spike.
- Modify:
  - `ios/make_xcodeproj.py`: `FAST_FILES = ("whisper.cpp",)`, the llama headers search path as in the spike, and
    `GCC_PREPROCESSOR_DEFINITIONS` += `SVE_WITH_WHISPER=1`. Then regenerate the project.
  - `core/include/sve_core.h`, `core/src/sve_core.cpp`.
  - `core/cli/sve_cli.cpp:75`, to pass `use_gpu = 0`.
- Test: `core/tests/test_whisper_text.cpp`, adding null-safety cases for the new calls compiled without
  `SVE_WITH_WHISPER`.

**Interfaces:**
- Consumes: Task 1's `plan_group`, `collapse_repeats` and `lang_choice`.
- Produces (C, in `sve_core.h`):
  - `int sve_load_asr(sve_ctx *ctx, const char *whisper_model_path, int use_gpu, int n_threads);` (it gains
    `use_gpu`)
  - `void sve_unload_asr(sve_ctx *ctx);` (unchanged)
  - `int sve_asr_loaded(sve_ctx *ctx);` (1 or 0)
  - `char *sve_whisper_next_piece(sve_ctx *ctx, const char *wav_path, int64_t start_sample, int is_final, sve_progress_cb cb, void *user);`
    It returns JSON with the same statuses and semantics as `sve_transcribe_next_piece`:
    - `{"status":"piece","start_sec":N,"end_sample":N,"text":"…","conf":F,"needs_gemma":B}`
    - `{"status":"wait"}`
    - `{"status":"end"}`
    - NULL on failure or cancel (a progress callback returning non-zero cancels, as now).
  - `char *sve_transcribe_span(sve_ctx *ctx, const char *wav_path, int64_t start_sample, int64_t end_sample, const char *prev_text, sve_progress_cb cb, void *user);`
    This is Gemma, on exactly [start, end) (at most 28 s). It returns `{"text":"…","conf":F}`, or NULL.
  - Without `SVE_WITH_WHISPER`, `sve_whisper_next_piece` returns NULL and `sve_asr_loaded` returns 0.

- [ ] **Step 1: Write the failing tests:**
  - `sve_whisper_next_piece(nullptr, "x.wav", 0, 1, nullptr, nullptr) == nullptr`
  - `sve_asr_loaded(nullptr) == 0`
  - `sve_transcribe_span(ctx_without_model, fixture_wav, 0, 16000, nullptr, nullptr, nullptr) == nullptr`

  These are added to `run.sh`'s SRCS only if `sve_core.cpp` already builds there. Otherwise the cases go in the
  existing core test that links `sve_core`, and the test file name goes in the commit message.

- [ ] **Step 2: Run it to make sure it fails.** Run `bash core/tests/run.sh`. Expected: build failure on the
  undeclared functions.

- [ ] **Step 3: Implement.**
  - **`sve_whisper_next_piece`:**
    1. Read the frames on disk with the same WAV rules as `sve_transcribe_next_piece`, then call `plan_group`.
    2. Per span: `whisper_pcm_to_mel`, then `whisper_lang_auto_detect` over the te and en ids, then
       `lang_choice`.
    3. If any span is `Gemma`, return `needs_gemma:true` with text "" and the group's `end_sample`, without
       decoding.
    4. Otherwise decode each span with the Global Constraints settings and join the span texts with spaces.
    5. Apply `collapse_repeats`.
    6. `conf` is the mean token probability over text tokens (NaN when none), and `start_sec` is
       `start_sample/16000`.
  - **`sve_transcribe_span`:** factor the "transcribe these samples with the prompt, vocabulary line and
    prev_text" core out of `sve_transcribe_next_piece`, so both calls share it, and call it on the exact span.
  - **`sve_load_asr`:** set `use_gpu` and `flash_attn = true`.

- [ ] **Step 4: Run the tests and make sure they pass.** Run `bash core/tests/run.sh` (`0 failed`) and the device
  build (`** BUILD SUCCEEDED **`).

- [ ] **Step 5: Commit.** Message: `feat(core): whisper.cpp on iOS; Whisper next-piece and Gemma exact-span calls`

### Task 3: Pending pieces in the sidecar, and when to use Whisper

**Files:**
- Modify: `ios/SummaverickEveryday/Engine/TranscriptSidecar.swift` (`struct Piece`, `append`, `previousText`)
- Modify: `ios/SummaverickEveryday/Engine/ModelLogic.swift`, adding `enum WhisperChoice`
- Test: a new swiftc check `scratchpad/whisper_check/` (`run.sh` compiles `TranscriptSidecar.swift`,
  `ModelLogic.swift` and `main.swift`, the same harness as `steps_check`)

**Interfaces:**
- Produces:
  - `TranscriptSidecar.Piece` gains `var pending: Bool?` and `var end_sample: Int64?`. Both are absent in old
    sidecars, which decode as before.
  - `mutating func appendPending(startSec: Int, endSample: Int64)` adds a piece with text "", `pending = true` and
    `end_sample`, and moves the cursor.
  - `var pendingIndices: [Int]` lists the pieces with `pending == true`.
  - `mutating func fill(_ index: Int, text: String, conf: Double?)` sets the text and clears `pending`. If the text
    is "", the piece is removed.
  - `previousText` skips pending pieces.
  - `TranscriptPiece` conversion (`TranscriptRun.pieces`) leaves pending pieces out.
  - `enum WhisperChoice { static let cpuFasterThanAudio = false; static func use(whisperReady: Bool, backend: ModelBackend) -> Bool }`
    returns true only when `whisperReady && (backend == .gpu || cpuFasterThanAudio)` (spec §4).
    `ModelBackend` must be visible to the check; if it isn't in `ModelLogic.swift`, use a `Bool onGPU` parameter
    instead.

- [ ] **Step 1: Write the swiftc check `main.swift`:**
  - `appendPending(startSec: 28, endSample: 896000)` moves the cursor to 896000, and `pendingIndices == [i]`.
  - Save, then load from a temporary file: `pendingIndices` is unchanged (Review Focus 3).
  - An old sidecar JSON without the new keys decodes, and `pendingIndices == []`.
  - `fill(i, text: "Hello", conf: 0.9)` gives `pendingIndices == []`, and the text is "Hello".
  - `fill(i, text: "", conf: nil)` removes the piece.
  - `previousText` is never a pending piece's text.
  - `append(startSec: 0, endSample: 448000, text: "")` moves the cursor and adds no piece (Review Focus 1).
  - `WhisperChoice.use(whisperReady: true, backend: .cpu) == false`
  - `WhisperChoice.use(whisperReady: true, backend: .gpu) == true`
  - `WhisperChoice.use(whisperReady: false, backend: .gpu) == false`

  It prints `whisper_check OK`.

- [ ] **Step 2: Run it to make sure it fails.** Run `sh scratchpad/whisper_check/run.sh`. Expected: compile errors
  on `appendPending`.

- [ ] **Step 3: Implement** the members above. Encoding omits `pending` and `end_sample` when nil.

- [ ] **Step 4: Run the check and the device build.** Expected: `whisper_check OK` and `** BUILD SUCCEEDED **`.

- [ ] **Step 5: Commit.** Message: `feat(ios): pending transcript pieces for Gemma, and when Whisper is used`

### Task 4: The pipeline (Whisper first, then Gemma fills, one model at a time)

**Files:**
- Modify:
  - `ios/SummaverickEveryday/Engine/CoreEngine.swift`
  - `ios/SummaverickEveryday/Engine/LiveTranscriber.swift` (`TranscriptRun`)
  - `ios/SummaverickEveryday/Engine/ProcessingPipeline.swift:975-997` (`transcribe`)
  - `ios/SummaverickEveryday/Engine/ModelLogic.swift`, adding `enum WhisperFlow`
- Test: extend `scratchpad/whisper_check/main.swift`

**Interfaces:**
- Consumes:
  - Task 2's C calls;
  - Task 3's `appendPending`, `pendingIndices`, `fill` and `WhisperChoice.use`;
  - Task 5's `ModelManager.whisperReady: Bool` and `ModelManager.whisperPath: String`. Until Task 5 lands, these
    are a stub that returns false, so the pipeline behaves as today.
- Produces:
  - `CoreEngine.loadWhisper(path: String, backend: ModelBackend) throws`. It calls `unloadModel()` first, and its
    log line includes the load time.
  - `CoreEngine.unloadWhisper()`
  - `CoreEngine.loadModel(...)` calls `unloadWhisper()` first. This is the one-model rule, enforced in one place
    and logged as `[sve] model swap: whisper→gemma %.1fs`.
  - `CoreEngine.whisperNextPiece(path:startSample:isFinal:shouldStop:) async throws -> WhisperPiece`, where
    `enum WhisperPiece { case piece(startSec: Int, endSample: Int64, text: String, conf: Double?, needsGemma: Bool), wait, end }`.
  - `CoreEngine.transcribeSpan(path:start:end:previousText:backend:shouldStop:) async throws -> (String, Double?)`
  - `TranscriptRun.runWhisper(audioPath:shouldStop:beforePiece:onPiece:) async throws -> (TranscriptSidecar, Stop)`.
    It has the same loop shape as `run`:
    - the heat gate before each piece;
    - the cursor saved after each piece;
    - `loadWhisper` again before a piece if Whisper was freed (Ask, Review Focus 2).
  - `TranscriptRun.fillPending(audioPath:shouldStop:backend:) async throws -> TranscriptSidecar`. Gemma fills each
    pending piece with `transcribeSpan`, saving after each.
  - `enum WhisperFlow { static func next(whisperReady: Bool, onGPU: Bool, cursorAtEnd: Bool, pending: Int) -> Step }`,
    where `enum Step { case whisper, gemmaRemainder, fillPending, done }`.

- [ ] **Step 1: Extend the swiftc check:**
  - `next(whisperReady: true, onGPU: true, cursorAtEnd: false, pending: 0) == .whisper`
  - `next(true, false, false, 0) == .gemmaRemainder` (in the background, Gemma's CPU path continues from the
    cursor)
  - `next(true, true, true, 2) == .fillPending`
  - `next(false, true, false, 0) == .gemmaRemainder` (Whisper missing means today's path)
  - `next(true, true, true, 0) == .done`

- [ ] **Step 2: Run it to make sure it fails.** Run `sh scratchpad/whisper_check/run.sh`. Expected: `WhisperFlow`
  undefined.

- [ ] **Step 3: Implement.** `ProcessingPipeline.transcribe` loops on `WhisperFlow.next` until `.done`:
  - `.whisper` runs `runWhisper`. Its `onPiece` reports progress as today.
  - `.gemmaRemainder` runs the existing `TranscriptRun.run`.
  - `.fillPending` frees Whisper, loads Gemma and runs `fillPending`.

  A `.yielded` stop throws `CancellationError`, as today. If Whisper fails to load, log it and use
  `.gemmaRemainder` for this note.

- [ ] **Step 4: Run the check and the device build.** Expected: `whisper_check OK` and `** BUILD SUCCEEDED **`.

- [ ] **Step 5: Commit.** Message: `feat(ios): Whisper writes the transcript first, Gemma fills what it isn't sure of`

### Task 5: Downloading and checking the Whisper model

**Files:**
- Modify: `ios/SummaverickEveryday/Engine/ModelManager.swift`
- Modify: `ios/SummaverickEveryday/Engine/ModelLogic.swift`, adding `enum WhisperFile`
- Create: `tools/model/whisper-te-README.md`, the model card the owner uploads with the file
- Test: extend `scratchpad/whisper_check/main.swift`

**Interfaces:**
- Produces:
  - `ModelManager.whisperPart: ModelPart`, with fileName `ggml-te-medium-q8_0.bin` and URL
    `WhisperHost.url`.
  - `ModelManager.whisperReady: Bool`, `ModelManager.whisperPath: String`.
  - `WhisperFile.expectedBytes: Int64 = 823369779`
  - `WhisperFile.sha256 = "0f71156c1c043ae209d1ef1405e5d88df1c1ab7c13a4e9b7640df477a698c3ad"`
  - `WhisperFile.verdict(size: Int64?, sha: String?) -> Verdict`, where `enum Verdict { case ok, missing, bad }`.
    Wrong size or wrong SHA is `.bad`, and nil size is `.missing`.

- [ ] **Step 1: Ask the owner for the Hugging Face repo URL.** They upload `ggml-te-medium-q8_0.bin` from
  `scratchpad/push/` together with `tools/model/whisper-te-README.md`. Write the README in this step: it carries
  the Apache-2.0 notice, credits vasista22/whisper-telugu-medium, and says "converted to ggml q8_0 for
  whisper.cpp". Set `WhisperHost.url` to `https://huggingface.co/<owner-repo>/resolve/main/ggml-te-medium-q8_0.bin`.

- [ ] **Step 2: Extend the swiftc check:**
  - `verdict(size: 823369779, sha: WhisperFile.sha256) == .ok`
  - `verdict(size: 823369778, sha: WhisperFile.sha256) == .bad`
  - `verdict(size: 823369779, sha: "00") == .bad`
  - `verdict(size: nil, sha: nil) == .missing`

  Run it to make sure it fails.

- [ ] **Step 3: Implement.**
  - **Download:** the Whisper part downloads after Gemma's parts are present, in the same background session. It
    doesn't count toward Gemma's `.ready`.
  - **Sizes:** `partSizes` and the no-space check include it, so the setup screen shows one total.
  - **Checking:** the SHA-256 is computed once after a download finishes (CryptoKit, streamed in 8 MB reads, off
    the main actor). A file whose verdict is `.ok` is remembered in UserDefaults `whisper.verified` = its size and
    modification date. `.bad` deletes the file and downloads again (Review Focus 4).
  - `whisperReady` is true only for a remembered `.ok` file whose size and date still match.

- [ ] **Step 4: Run the check and the device build.** Expected: `whisper_check OK` and `** BUILD SUCCEEDED **`.

- [ ] **Step 5: Commit.** Message: `feat(ios): Whisper model download with size and checksum check`

### Task 6: Device checks on the iPhone 16e (owner present)

**Files:** the debug-only re-transcribe hook (Step 2) in `ProcessingPipeline.swift`, plus fixes where a check
fails. Results go in the spec under a new section, "Device results", and in memory `telugu-asr-gemma4.md`.

- [ ] **Step 1: Install.** Ask the owner for the go-ahead, and confirm they aren't recording. Then install the
  dd-dev build. If the Hugging Face upload isn't done yet, copy the model into the app's
  `Library/Application Support/<models dir>` with `devicectl device copy to`.
- [ ] **Step 2: Language detection (spec check 1).** Re-transcribe notes 22 and 25 through a debug-only
  environment variable `SVE_WHISPER_RETRANSCRIBE=<note id>`, behind `#if DEBUG`. It writes to a copy of the
  sidecar and never to the note.
  - Count per note the groups with `needs_gemma`: note 25 should be ≤ 5%, and note 22 ≥ 95%.
  - If the counts miss, tune `min_te` and `margin` in `lang_choice`, then repeat Task 1's tests.
- [ ] **Step 3: A 30-minute Telugu class, app open, warm (spec check 2).**
  - Log the time from Stop until the transcript is done, plus the swap lines. Gates: 14 min hot, 12 min warm.
  - The owner reads the transcript against Gemma's.
  - Set the Whisper "[unclear]" threshold from the pieces they mark wrong.
- [ ] **Step 4: Mixed, ticking and missing model (spec checks 3, 4 and 7).**
  - English parts come out in English letters.
  - No line repeats a word more than 3 times.
  - With the model file renamed, the note is written by Gemma as today.
- [ ] **Step 5: Memory and the locked phone (spec checks 5 and 6).**
  - Memory: the log never shows both models loaded at once.
  - Locked phone: time Whisper on the CPU over 2 minutes of audio. If it beats real time, set
    `WhisperChoice.cpuFasterThanAudio = true` and rerun Task 3's check.
- [ ] **Step 6: Commit** the results. Message: `docs: Whisper device results`
