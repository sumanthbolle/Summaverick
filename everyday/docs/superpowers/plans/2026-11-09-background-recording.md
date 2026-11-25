# Record and finish without opening the app — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A recording finishes, notifies, shows progress and can be started and stopped without opening the app.

**Architecture:** iOS only, with no core change.
- A notifier sends local notifications and an automatic BGProcessingTask catches work that iOS cuts off.
- A progress heartbeat keeps the continued-processing task moving.
- A new widget extension holds the Live Activity and the Control Center control.
- App Intents (AudioRecordingIntent / LiveActivityIntent) drive a headless RecordingController that RecordView shares.

**Tech Stack:** SwiftUI, BackgroundTasks, UserNotifications, ActivityKit, WidgetKit, AppIntents (iOS 18+ parts availability-gated; deployment target stays 17.0).

**Spec:** `docs/superpowers/specs/2026-11-09-background-recording-design.md`

## Global Constraints

- Everything stays on the phone: no network, no push server.
- No silent-audio or other background-rule workarounds.
- Consent must already be acknowledged before a shortcut records; otherwise the intent opens the app at the consent screen.
- Notifications: at most one per state change per recording (paused / ready / failed), never one per percent.
- Every user-facing string is in Localizable.xcstrings with en + te, written as `json.dumps(d, ensure_ascii=False, indent=2)+"\n"`.
- New Swift files: `cd ios && python3 make_xcodeproj.py`. Device build plus `python3 ios/tools/check_strings.py <dd>` must print `STRINGS OK`.
- Recordings made in the app behave exactly as today.
- Memories (second-brain Task 6) keep yielding rules; nothing here sets a notes failure banner that did not exist.

## Review Focus

1. Stopping from the Live Activity while locked: if the continued-task submit throws, the fallback must still produce notes later (BGProcessingTask) and tell the person.
2. The app is killed mid-recording, from a shortcut start with no UI: the audio file and sidecar must survive and be processed as an interrupted recording, exactly like an in-app recording.
3. A shortcut started while a recording is already running must not start a second one; it is a no-op that says "Already recording".
4. A ready notification arriving while the app is in front must not show; in front, the app already shows the note.
5. The Live Activity must end on every path (ready, paused, failed, cancel, deleted note) and never be left orphaned on the lock screen.

---

### Task R1: Notifications, automatic finish, progress heartbeat (iOS engine)

**Files:**
- Create: `ios/SummaverickEveryday/Engine/NotesNotifier.swift`
- Modify: `Engine/ProcessingPipeline.swift` (continuedWorkExpired, the finish paths, sub-progress), `Engine/OvernightWork.swift` (auto-enrol an interrupted note, `requiresExternalPower = false` for auto runs), `Engine/ModelLogic.swift` (pure rules), `Engine/ReminderScheduler.swift` (route for a note notification, if the route type lives there), `Localizable.xcstrings`

**Interfaces:**
- Produces:
  - `NotesNotifier.notesReady(noteID:title:)`, `.paused(noteID:title:percent:)`, `.failed(noteID:title:line:)`. Each is a no-op while the app is active. Identifier `notes.<noteID>`, so a later state replaces the earlier one. Tapping opens the note through the existing pendingRoute path.
  - `NotesProgress.heartbeatDue(lastReport:now:) -> Bool`: due at ≥30 s.
- Strings (en/te): "Notes ready: %@", "%1$@: paused at %2$lld%%. It will finish while charging, or tap to finish now.", "%@: notes could not be written".

- [ ] **Step 1:** swiftc check `notify_check/main.swift` over ModelLogic covers:
  - the heartbeat rule;
  - `NotifyRule.shouldNotify(appActive:)`;
  - the rule that an expired run enrols the note for automatic finish.

  Paste the output.
- [ ] **Step 2: Implement.**
  - On continued-task expiry: put the note on the overnight list, submit the BGProcessingTask, and post `paused` with the current percent.
  - On notes finished (any path, including overnight and the automatic run) with the app not active: post `notesReady`.
  - Transcription catch-up, sections and outputs call `ContinuedWork.shared.update(fraction:)` at least every 30 s, using piece/section counts and the token progress the core callback already gives.
  - Log every path with `NSLog("[sve] bg: …")`.
- [ ] **Step 3:** Device build, `STRINGS OK`.
- [ ] **Step 4: Commit** `feat(ios): notes finish on their own after a cut-off, and say when they are ready`.

### Task R2: Widget extension with the recording Live Activity; shared RecordingController

**Files:**
- Create: `ios/SummaverickEveryday/Audio/RecordingController.swift` (headless start/stop/cancel; owns AudioRecorder plus the LiveTranscriber hand-off; @MainActor singleton), `ios/Shared/RecordingActivity.swift` (`ActivityAttributes`: `ContentState { phase: recording|processing|ready|paused|failed, startedAt, stageLine, percent, noteID }`, compiled into both targets), `ios/SummaverickWidgets/` (WidgetBundle, Live Activity view, Info.plist)
- Modify: `ios/make_xcodeproj.py` (adds the `SummaverickWidgets` app-extension target embedded in the app; `ios/Shared/*` goes into both targets), `Info.plist` (`NSSupportsLiveActivities` YES), `Views/RecordView.swift` (uses RecordingController; behaviour unchanged), `Engine/ProcessingPipeline.swift` (updates the activity's processing state and ends it on every path), `Localizable.xcstrings`

**Interfaces:**
- Produces:
  - `RecordingController.shared.start() async -> StartResult` (`.started`, `.alreadyRecording`, `.needsConsent`, `.micDenied`);
  - `stop() -> Bool`, which hands off to `ProcessingPipeline.recordingStopped` exactly as RecordView did;
  - `cancel()`;
  - `isRecording`, `elapsed`;
  - `RecordingActivity`, started on record start and moved to processing after Stop.
  - `LiveActivityLogic.state(for stage:) -> ContentState` (pure).
- Live Activity layout:
  - Recording: red dot, elapsed timer (`Text(timerInterval:)`) and a Stop button. The button stays inert in R2; it is wired to `StopRecordingIntent` in R3.
  - Processing: stage line and a progress bar.
  - Ready: "Notes ready" and the title; the activity ends with the `.after(+4h)` dismissal policy.
  - Paused/failed: their line.

- [ ] **Step 1:** swiftc check `activity_check` covers:
  - `LiveActivityLogic` maps every pipeline stage, including ready, paused and failed, to a state;
  - the end rules: every terminal stage ends the activity.

  Paste the output.
- [ ] **Step 2: Implement.** RecordView becomes a view over RecordingController, so the in-app flow is byte-for-byte the same in behaviour.
- [ ] **Step 3:** Device build (both targets), `STRINGS OK`.
- [ ] **Step 4: Commit** `feat(ios): recording Live Activity; one recording controller for app and shortcuts`.

### Task R3: Start and stop without opening the app (App Intents, Siri, Action button, Control Center)

**Files:**
- Create: `ios/Shared/RecordingIntents.swift` (both targets):
  - `StartRecordingIntent: AudioRecordingIntent` (iOS 18+);
  - `StopRecordingIntent: LiveActivityIntent`;
  - `SummaverickShortcuts: AppShortcutsProvider` (app target only): phrases "Start recording in \(.applicationName)", "Stop recording in \(.applicationName)";
  - `ios/SummaverickWidgets/RecordControl.swift`: a `ControlWidget` toggle bound to the recording state.
- Modify: `ios/SummaverickWidgets` bundle, `Localizable.xcstrings`

**Interfaces:**
- `StartRecordingIntent.perform`:
  - `.started` returns a dialog "Recording";
  - `.alreadyRecording` returns "Already recording";
  - `.needsConsent` throws a `needsToContinueInForeground` that opens the consent screen;
  - `.micDenied` returns a dialog asking to allow the microphone in Settings.
- `StopRecordingIntent.perform`: `RecordingController.shared.stop()`. The continued-task submit inside `recordingStopped` may throw in the background; R1's fallback covers that.

- [ ] **Step 1:** swiftc check of the pure result → dialog mapping. Paste the output.
- [ ] **Step 2: Implement.** The Live Activity Stop button uses `Button(intent: StopRecordingIntent())`.
- [ ] **Step 3:** Device build, `STRINGS OK`. Install on the phone.
- [ ] **Step 4: Commit** `feat(ios): start and stop recording from Shortcuts, Siri, the Action button and Control Center`.

### Task R4: Device check with the owner (joins the Phase A test session)

- [ ] Run each case from the spec's "Done when", with `xcrun devicectl device process launch --console` logs (the `[sve] bg:` lines):
  1. An 80-minute recording (or the 81-minute note's Regenerate, for the sections and outputs part): Stop, lock, and wait. Record which path iOS took (continued task, expiry, automatic BGProcessingTask, on battery or not) and when the ready notification came.
  2. Action button / Shortcut start while locked: Live Activity, Stop from it, notes, ready notification.
  3. Control Center toggle start and stop.
  4. Already recording, and consent not given.
- [ ] Write the results to `docs/superpowers/2026-11-09-background-recording-device-check.md` and commit.
