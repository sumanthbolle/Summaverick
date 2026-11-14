# Summary first Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A note's title, summary, notes and questions are written before its reminders and tasks (extract), so the
summary never waits behind them; extract runs right after the notes, and a cut-off extract runs again by itself.

**Architecture:** In `ProcessingPipeline.generateOutputs` the extract block moves from before the sections to just
after the "notes ready" notice, before the sections-after, memories and insight. A cut-off or stopped extract is
put on the existing find-reminders queue (`requeueFindReminders`), which the resume loop and the overnight task
already run. The lock-screen progress loses the reminders step before the notes and gains an extract span after
them. Pure rules live in `ModelLogic.swift` and are checked with a Foundation-only swiftc check.

**Tech Stack:** Swift 6 / SwiftUI iOS app, C++17 core (unchanged), swiftc checks in the scratchpad.

**Spec:** `docs/superpowers/specs/2026-11-14-shared-core-second-brain-design.md` (owner approved 2026-11-14).
This plan is part 1 of its order of building ("Builder order: summary first, extract after (goal 5). Measure on a
10-minute recording."). Parts 2–6 get their own plans.

## Global Constraints

- **No model on the Mac** (owner's battery rule): checks load no model. Timing is measured only on the iPhone 16e
  (device `00008140-001668892E22801C`).
- **No install, no push without the owner.** Commit on `feature/personal-assistant`. Installing kills the app:
  confirm the owner isn't recording first.
- **Yes gate unchanged:** extract still only creates suggestions that wait for the person's Yes.
- **Medicine rule unchanged.** No new user-facing strings (voice guide `docs/superpowers/2026-11-09-voice-guide.md`).
- **Targets (spec goal 6 and builder-check answer 1):** a 10-minute recording has its summary within 2 minutes of
  Stop with the app in front, within 6 minutes with the phone locked. Measured and reported, both.
- **Verification for every code task:**
  - `bash /private/tmp/claude-501/-Users-sumanth-bolle-Downloads-summaverick-everyday-ios/0a2e198b-c172-4f76-84bc-6ed75626fdb9/scratchpad/order_check/run.sh` prints `OK`;
  - `bash /private/tmp/claude-501/-Users-sumanth-bolle-Downloads-summaverick-everyday-ios/0a2e198b-c172-4f76-84bc-6ed75626fdb9/scratchpad/shruti_check/run.sh` still prints its OK line;
  - `bash core/tests/run.sh` ends with `0 failed`;
  - `cd ios && xcodebuild -project SummaverickEveryday.xcodeproj -scheme SummaverickEveryday -destination 'generic/platform=iOS' -derivedDataPath /private/tmp/claude-501/-Users-sumanth-bolle-Downloads-summaverick-everyday-ios/0a2e198b-c172-4f76-84bc-6ed75626fdb9/scratchpad/dd-dev -allowProvisioningUpdates build` prints `** BUILD SUCCEEDED **`.

## Review Focus

1. **iOS cuts the run off during extract after the notes.** The notes stay; extract is queued again and runs later
   (resume loop or overnight); no suggestion is created twice (the core stores suggestions only when the whole
   extract finishes, `sve_assistant.cpp:476-511`). Pinned by `ExtractAfterNotes` checks in Task 1.
2. **A resumed run whose notes are stored but which was never extracted** reaches extract once, after skipping the
   stored outputs. Pinned in Task 1 (`.run` when not extracted) and checked on the device in Task 3.
3. **A note already extracted** (older note, refill of parts, regenerate) is not extracted again. Task 1 `.skip`.
4. **A recording starts while extract would run after the notes.** Extract steps aside and is queued (LiveFirst),
   like the find-reminders tap. Task 1 `.queue`.
5. **Lock-screen progress never goes backwards or stands still** across notes, extract, sections, memories, insight.
   Task 1 checks the after-notes spans are increasing and inside 0…1.

---

### Task 1: Rules for extract after the notes

**Files:**
- Modify: `ios/SummaverickEveryday/Engine/ModelLogic.swift` (`NotesOrder` at about line 598, `WaitingNotes.extracts` at about line 254)
- Create: `scratchpad/order_check/main.swift`, `scratchpad/order_check/run.sh` (scratchpad path as in Global Constraints)

**Interfaces:**
- Produces:
  - `enum ExtractAfterNotes { enum Action: Equatable { case skip, run, queue }; static func action(alreadyExtracted: Bool, recordingLive: Bool) -> Action }` — `skip` when already extracted; else `queue` when a live recording is running; else `run`.
  - `static func afterStop(stopped: Bool) -> Bool` on `ExtractAfterNotes`: true (queue again) when the extract threw because the run was stopped or cut off; false for a real failure (logged only, as today).
  - `NotesOrder.afterNotes(_:)` returns `(extract: ClosedRange<Double>, sections: ClosedRange<Double>?, memories: ClosedRange<Double>, insight: Double)`. Values: `.sectionsFirst` → extract `0...0.3`, sections `nil`, memories `0.3...0.7`, insight `0.7`; `.notesFirst` → extract `0...0.25`, sections `0.25...0.5`, memories `0.5...0.8`, insight `0.8`.
  - `WaitingNotes.extracts(alreadyExtracted:)` is removed (replaced by `ExtractAfterNotes.action`); the find-reminders tap path does not use it.

- [ ] **Step 1: Write the failing check** in `order_check/main.swift`, run with `ModelLogic.swift` and `TranscriptSidecar.swift` like `shruti_check/run.sh`:

```swift
func check(_ c: Bool, _ m: String) { if !c { print("FAIL", m); exit(1) } }
check(ExtractAfterNotes.action(alreadyExtracted: true, recordingLive: false) == .skip, "extracted: skip")
check(ExtractAfterNotes.action(alreadyExtracted: true, recordingLive: true) == .skip, "extracted while live: skip")
check(ExtractAfterNotes.action(alreadyExtracted: false, recordingLive: true) == .queue, "live: queue")
check(ExtractAfterNotes.action(alreadyExtracted: false, recordingLive: false) == .run, "not extracted: run")
check(ExtractAfterNotes.afterStop(stopped: true), "stopped: queued again")
check(!ExtractAfterNotes.afterStop(stopped: false), "real failure: not queued")
for o in [NotesOrder.Order.sectionsFirst, .notesFirst] {
    let a = NotesOrder.afterNotes(o)
    var marks = [a.extract.lowerBound, a.extract.upperBound]
    if let s = a.sections { marks += [s.lowerBound, s.upperBound] }
    marks += [a.memories.lowerBound, a.memories.upperBound, a.insight]
    check(marks.first == 0 && zip(marks, marks.dropFirst()).allSatisfy { $0 <= $1 } && marks.last! < 1, "spans in order: \(o)")
}
print("OK")
```

- [ ] **Step 2: Run it and see it fail** (does not compile: `ExtractAfterNotes` missing, `extract` not a member).
- [ ] **Step 3: Implement** `ExtractAfterNotes` and the new `NotesOrder.afterNotes` in `ModelLogic.swift` with the values above; update the `NotesOrder` doc comment (reminders now come after the notes, owner 2026-11-14, note 41). Remove `WaitingNotes.extracts`.
- [ ] **Step 4: Run** `order_check/run.sh` → `OK`; `shruti_check/run.sh` → its OK line. The app build is expected to fail until Task 2 (callers of `afterNotes` and `WaitingNotes.extracts`); do not build here.
- [ ] **Step 5: Commit** `ModelLogic.swift` with message `feat: rules for extract after the notes`. (The check lives in the scratchpad, not the repo.)

### Task 2: Extract after the notes in the pipeline

**Files:**
- Modify: `ios/SummaverickEveryday/Engine/ProcessingPipeline.swift` (`notesSteps` / `sectionsStep` / `firstOutputStep` at about 1362; `generateOutputs` 1366–1584; `writeAllInOnePass` progress at about 1534)
- Modify: `docs/ARCHITECTURE.md` (the notes-stage order)

**Interfaces:**
- Consumes: `ExtractAfterNotes.action(alreadyExtracted:recordingLive:)`, `ExtractAfterNotes.afterStop(stopped:)`, `NotesOrder.afterNotes(_:).extract` (Task 1); existing `requeueFindReminders(_:)`, `engine.noteExtracted(_:)`, `engine.extract(noteID:shouldStop:)`, `onBackend`, `ContinuedWork.shared.update(afterNotes:)`, `LiveTranscriber.shared.liveRunning`.
- Produces: no new API.

- [ ] **Step 1: Steps before the notes.** `notesSteps = 2 + OutputKind.allCases.count` (sections, title, outputs), `sectionsStep = 0`, `firstOutputStep = 2`. Every `NotesProgress.fraction(outputsDone: N, …)` in `generateOutputs` and `writeAllInOnePass` that counted the reminders step moves down by one (after sections 2 → 1, after title 3 → 2, one-pass start 2 → 1). The pieces backfill stays at the top.
- [ ] **Step 2: Remove the extract block** before the sections (the `if WaitingNotes.extracts…` branch and the progress update after it), keeping the abort check.
- [ ] **Step 3: Extract after the notes.** Right after `notesStored = true` and before the sections-after: take `after = NotesOrder.afterNotes(…)` (moved up from below), then per `ExtractAfterNotes.action(alreadyExtracted: await engine.noteExtracted(id), recordingLive: LiveTranscriber.shared.liveRunning)`:
  - `.skip`: log `[sve] note %lld: reminders and tasks already extracted; not extracted again`.
  - `.queue`: `requeueFindReminders(id)` and log `[sve] note %lld: reminders and tasks wait for the recording`.
  - `.run`: `stage = .extracting`; `ContinuedWork.shared.update(afterNotes: after.extract.lowerBound)`; run `engine.extract` on `onBackend` with `shouldStop: { ContinuedWork.beat(); return abort.isSet }`; log `[sve] note %lld: extract after the notes added %d suggestions in %.0f s`; on a throw log `[sve] extract failed: %@` and, if `ExtractAfterNotes.afterStop(stopped: abort.isSet)`, `requeueFindReminders(id)`. Post `.assistantChanged` either way; then `ContinuedWork.shared.update(afterNotes: after.extract.upperBound)`. A failure never touches the notes and never returns early: sections, memories and insight follow as now (they stop themselves on the abort flag).
- [ ] **Step 4: ARCHITECTURE.md.** In the notes-stage description, the order becomes: sections (stepped path) / one pass, title, summary, notes, questions, then reminders and tasks, then sections-after (notes first), memories, insight. One line on why (note 41: extract ran 47 min before the summary).
- [ ] **Step 5: Verify** with the four commands in Global Constraints. Also `grep -n "WaitingNotes.extracts" ios -r` finds nothing.
- [ ] **Step 6: Commit** `ProcessingPipeline.swift` and `docs/ARCHITECTURE.md`, message `feat: summary first, reminders and tasks after the notes`.

### Task 3: Measure on the phone (owner present)

No code. The controller does this with the owner.

- [ ] **Step 1:** Ask the owner to install; confirm they are not recording. Build at HEAD into `scratchpad/dd-install`, install, and launch with the console captured to `scratchpad/t9/app.log`.
- [ ] **Step 2:** The owner records exactly 10 minutes with the app in front and keeps it in front after Stop. From the log: Stop, transcript ready, `notes in one pass` (or the stepped lines), summary ready, notes ready, `extract after the notes added … in … s`, memories, insight. Report Stop → summary against 2 minutes.
- [ ] **Step 3:** Once the phone is back at heat level full, the owner records exactly 10 minutes, presses Stop and locks the phone. Same timings; report Stop → summary against 6 minutes, and whether any step was cut off and queued.
- [ ] **Step 4:** Write the numbers in the memory file `performance-is-the-measure.md` and report to the owner: where the time went (transcript, one pass or steps, heat rests), against both targets. If a target is missed, report the cause; the next step is the owner's (spec part 3, the faster model).
