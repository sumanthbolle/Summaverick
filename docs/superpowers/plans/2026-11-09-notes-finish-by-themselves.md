# Notes that finish by themselves — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** After Stop, a recording's notes always finish by themselves: no failure for an interruption, a notification per part, no lost taps, and live transcription first, all with Gemma 4 E4B.

**Architecture:**
- The iOS engine changes; the C++ core changes only in N5, for speed.
- Each run's rules are pure functions in `Engine/ModelLogic.swift`, checked with swiftc scripts.
- `ProcessingPipeline` and `ContinuedWork` apply those rules.

**Tech Stack:** Swift/SwiftUI, BackgroundTasks, UserNotifications, C++ core (llama.cpp), swiftc check scripts, `core/tests/run.sh`.

**Spec:** `docs/superpowers/specs/2026-11-09-notes-finish-by-themselves-design.md`

## Global Constraints

- **E4B stays the transcription model.** No smaller model, no cloud, no background GPU, no silent-audio workarounds.
- **Heat rules stay as they are:** `Heat.beforeRun` rests 30 s at serious and pauses at critical.
- **"Failed" means a real failure only:** no speech in the recording, or the audio gone with no transcript. Any interruption (expiry, cancel, heat, GPU cut off in the background, app killed) keeps the work and continues by itself.
- **Notifications:** at most one per stage per note, under the identifier `notes.<id>`, so a later one replaces the earlier. None while the app is active.
- **Strings:** every user-facing string is in `Localizable.xcstrings` with en and te, written as `json.dumps(d, ensure_ascii=False, indent=2)+"\n"`.
- **Device build:** run `python3 ios/tools/check_strings.py <dd>`; it must print `STRINGS OK`. After adding new Swift files, run `cd ios && python3 make_xcodeproj.py`.
- **Core:** `bash core/tests/run.sh` stays all passed, and `bash core/tests/smoke_gemma.sh api` prints `API OK (0 missed)`.
- **Logging:** log each background path with `NSLog("[sve] bg: …")`.
- **Notes and summaries:** the text and its quality stay unchanged, except through N5's accepted speed changes.
- **Commits:** list explicit paths only, ending with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.

## Review Focus

1. A Stop or tap while a continued-processing task is still held for finished work: the old task must end, never be reused at 100%, and the new work must get its own task.
2. A recording started while another note's notes are being written: that run must step aside at its next piece/section/output boundary and continue after, keeping everything done.
3. A Regenerate tapped while another note runs, then the app closed: it must run after, in the background, and notify; never be dropped silently.
4. Every `stage = .failed(...)` path reached by a cancel or cut-off (for example `transcription failed: CancellationError()` seen on the phone) must become a cut-off, not a failure.
5. A per-stage notification must not repeat (the same stage twice for one note) and must not show while the app is in front.

---

### Task N1: One background task per run

**Files:**
- Modify: `ios/SummaverickEveryday/Engine/ContinuedWork.swift`, `Engine/ProcessingPipeline.swift` (begin/finish call sites: `recordingStopped`, `personTapped`, `personRunEnded`, `finishContinuedWork`, `resumeInterrupted`), `Engine/ModelLogic.swift`

**Interfaces:**
- Produces:
  - `enum TaskLifecycle` in ModelLogic.
  - `static func onBegin(held: Bool, workLeft: Bool) -> BeginAction`, where `BeginAction` is `.keep` or `.finishThenSubmit`. A held task whose work is done (`workLeft == false`) is completed first; a new request is then submitted.
  - `static func finishes(working: Bool, personRunsPending: Int, queued: Int) -> Bool`: true when nothing runs and nothing waits.
  - `ContinuedWork.begin(key:subtitle:)` keeps its signature, but follows `onBegin`.
  - Progress never decreases inside one task: `startNote()` no longer resets the fraction to 0. It rescales: the new note's 0…1 maps onto the remaining room of the task.

- [ ] **Step 1:** Write the swiftc check `lifecycle_check/main.swift` over ModelLogic. It covers:
  - `onBegin(held: true, workLeft: false) == .finishThenSubmit`;
  - `onBegin(held: true, workLeft: true) == .keep`;
  - `finishes(working: false, personRunsPending: 0, queued: 0) == true`;
  - `finishes(..., queued: 1) == false`;
  - a rescale helper `TaskLifecycle.rescaled(taskFraction: 0.6, noteFraction: 0.5) == 0.8` (the new note uses the room left: 0.6 + 0.4 × 0.5). It is never below `taskFraction`.

  Run it, see it fail, implement, and see it pass. Paste the output.
- [ ] **Step 2: Wire it in.**
  - Find why the task sat at 1.000 for a minute after note 18 finished (log of 2026-11-09 12:55–12:56: heartbeats at 1.000 while a recording ran), and make it finish there.
  - When the work is all done, `finish(success: true)` is called at once.
  - A Stop or tap that finds a held, finished task completes it, then submits a new request.
  - Log `[sve] bg: task finished (work done)`, `[sve] bg: old task ended, new task asked for`.
- [ ] **Step 3:** Device build and `STRINGS OK`.
- [ ] **Step 4: Commit** `fix(ios): one background task per run; it ends when the work is done and never goes backwards`.

### Task N2: A person's tap is never lost

**Files:**
- Modify: `Engine/ProcessingPipeline.swift` (`personMayStart`, `regenerate`, `regenerateOutput`, `tryAgain`, `findReminders`, `continuedWorkExpired`, `resumeInterrupted`), `Engine/ModelLogic.swift`, the note screen that shows the busy line (`Views/NoteDetailView.swift`), `Localizable.xcstrings`

**Interfaces:**
- Produces:
  - `struct QueuedTap: Codable, Equatable { noteID: Int64; kind: Kind }`, where `Kind` is `.regenerate(output: Int?)` (an OutputKind raw value, or nil for all), `.generate` or `.findReminders`.
  - It is persisted in UserDefaults under the key `pipeline.queuedTaps`.
  - `ProcessingPipeline.queued: [QueuedTap]` is observable.
  - `enum TapQueue` in ModelLogic:
    - `static func enqueue(_ q: [QueuedTap], _ t: QueuedTap) -> [QueuedTap]`: no duplicate for the same note and kind; a later regenerate-all replaces a single-output regenerate of the same note.
    - `static func next(_ q: [QueuedTap]) -> QueuedTap?`: oldest first.
- Behaviour:
  - `personMayStart` no longer returns a 5-second busy notice. When another run is working, it enqueues the tap and returns false.
  - The note shows "Waiting: another note is being written" (en and te) while its tap is queued.
  - Queued taps run, one at a time, from the same loop as `resumeInterrupted`, before cut-off notes.
  - A regenerate cut off by expiry or the app being killed is enqueued again with the same request, so it resumes. The old text stays until the new text is complete; that is existing `RegenerateRules` behaviour, kept.
  - `NotesNotifier.regenerateStopped`, added in afc52f7, is replaced by the queue: drop the notifier method and its string. The next ready notification covers it.

- [ ] **Step 1:** Write the swiftc check `tapqueue_check`. It covers:
  - enqueue with no duplicates;
  - regenerate-all replacing a single regenerate;
  - oldest first;
  - Codable round-trip.

  Fail, implement, pass. Paste the output.
- [ ] **Step 2: Implement** the behaviour above. Log `[sve] bg: tap queued (note %lld, %@)` and `[sve] bg: queued tap runs (note %lld)`.
- [ ] **Step 3:** Device build and `STRINGS OK`.
- [ ] **Step 4: Commit** `feat(ios): taps made while another note runs wait their turn and run by themselves`.

### Task N3: Live transcription first

**Files:**
- Modify: `Engine/ProcessingPipeline.swift` (the notes run between pieces, sections and outputs; `resumeInterrupted`; memory and backfill guards), `Engine/LiveTranscriber.swift` (how a live piece waits for the model), `Engine/ModelLogic.swift`

**Interfaces:**
- Produces:
  - `enum LiveFirst` in ModelLogic: `static func yields(recording: Bool, runIsLive: Bool) -> Bool`, which is true for any non-live run while recording.
  - A non-live run checks `yields` at each boundary: each transcription piece, each section, each output.
  - When it yields, it parks:
    - the note goes back to `interrupted`, quietly, with no notice and no failure;
    - the run releases the model;
    - live transcription continues.
  - Parked notes resume after Stop through the normal loop.

- [ ] **Step 1:** Write the swiftc check `livefirst_check`, covering the `yields` truth table. Paste the output.
- [ ] **Step 2: Implement.**
  - Find how live pieces wait today while a notes run holds the model (2026-11-09 log: note 19 at 70% left at Stop while note 18 was being written).
  - Make the notes run step aside at the next boundary.
  - Log `[sve] note %lld steps aside for the recording`.
- [ ] **Step 3:** Device build and `STRINGS OK`.
- [ ] **Step 4: Commit** `feat(ios): while recording, live transcription comes before every other model run`.

### Task N4: No failure for an interruption; a notification per part

**Files:**
- Modify: `Engine/ProcessingPipeline.swift` (every `stage = .failed`, `generateOutputs` order and notifications, `cutOff`), `Engine/NotesNotifier.swift`, `Engine/ModelLogic.swift` (`NotifyRule`), `Localizable.xcstrings`

**Interfaces:**
- Produces:
  - `enum FailureKind` in ModelLogic: `static func classify(cancelled: Bool, aborted: Bool, cutOffByBackground: Bool, heat: Bool, noSpeech: Bool, audioGone: Bool) -> Outcome`, where `Outcome` is `.cutOff` or `.failed`.
    - `noSpeech` → `.failed`.
    - `audioGone` → `.failed`. It means no transcript and no audio file.
    - Any of `cancelled`, `aborted`, `cutOffByBackground` or `heat` → `.cutOff`.
    - Anything else is an unknown error: `.cutOff` once; the second time in a row for the same note it is `.failed`.
  - The counter for that rule is `failureStreak[noteID]`, which resets on any success.
  - `NotesNotifier.transcriptReady(noteID:title:)`, posting "Transcript ready: %@".
  - `NotesNotifier.summaryReady(noteID:title:)`, posting "Summary ready: %@".
  - The existing `notesReady` is posted after the detailed notes and the questions.
  - `NotifyRule.next(posted: Set<String>, stage: String) -> Bool`: one per stage per note per run.
  - The paused notice's text changes to "%@: paused at %lld%%. It continues by itself." (en and te). The old "tap to finish now" copy goes.
- Output order is `shortSummary`, then `detailedNotes`, then `revisionQuestions`, which is `OutputKind.allCases` already. The title is written with the summary. Each output is stored as soon as it is written, so the summary is readable before the notes.

- [ ] **Step 1:** Write the swiftc check `failure_check`. It covers:
  - the `classify` table, including the CancellationError case (`cancelled: true` → `.cutOff`);
  - the streak rule;
  - `NotifyRule.next` posting no repeats.

  Paste the output.
- [ ] **Step 2: Implement.** Route every `.failed` assignment through `classify`, then add the two notifications and the store-as-you-go.
- [ ] **Step 3:** Device build and `STRINGS OK`.
- [ ] **Step 4: Commit** `feat(ios): interruptions never fail; transcript, summary and notes each say when they are ready`.

### Task N5: A cheaper transcription piece (same accuracy)

**Files:**
- Modify: `core/src/sve_core.cpp`, `core/src/sve_llm.cpp` (timing logs, and the accepted changes).
- Measure with the scratch harness `/private/tmp/claude-501/-Users-sumanth-bolle-Downloads-summaverick-everyday-ios/0a2e198b-c172-4f76-84bc-6ed75626fdb9/scratchpad/asr_ab/` (`driver.cpp`, `analyze.py`, `results.json` with today's E4B outputs for 24 pieces).
- Create: `docs/superpowers/2026-11-09-piece-speed.md`

**Interfaces:**
- Produces:
  - Timing log per piece: `[sve] piece timing: encode %.1f s, prefill %.1f s (%d tok), decode %.1f s (%d tok)`. Opt-in: on when the env var `SVE_TIMING=1` is set.
  - Accepted changes, if any, behind no flag: they become the default.

- [ ] **Step 1:** Add the timing logs. Run `core/tests/run.sh`; all must pass.
- [ ] **Step 2:** Profile on the Mac CPU (4 threads) on the 24 pieces. Record the breakdown.
- [ ] **Step 3:** Try each candidate alone and measure its time and accuracy against `results.json` `e4b_mac`. Candidates:
  - (a) audio encoder threads, and whether the encoder can use more threads than decoding;
  - (b) previous-piece context length;
  - (c) output token cap per 28 s piece;
  - (d) mmproj quantisation (F16/Q8 → a smaller one, if a gemma4 audio mmproj exists for it).

  The accuracy gate is the critical-token differences (numbers, amounts, Latin words) per `analyze.py`. They must be no more than the E4B Mac-vs-phone baseline: 8 number tokens in 3 pieces.
- [ ] **Step 4:** Keep only the changes that pass, as the defaults. Write the table (time, accuracy, kept or not) to `docs/superpowers/2026-11-09-piece-speed.md`. Then run `core/tests/run.sh` and `smoke_gemma.sh api`.
- [ ] **Step 5: Commit** `perf(core): cheaper transcription pieces with the same accuracy` (or `docs:` only, if nothing passed).

### Task N6: Device check with the owner

- [ ] Install the build. Run each spec "Done when" case with `xcrun devicectl device process launch --console` logs:
  1. A long recording (or the 81-minute note's Regenerate): locked, cable in, and left alone.
  2. A Regenerate while another note runs.
  3. A recording made while another note is being written.

  Record the order of notifications and any cut-off.
- [ ] Write the results to `docs/superpowers/2026-11-09-notes-finish-device-check.md` and commit.
