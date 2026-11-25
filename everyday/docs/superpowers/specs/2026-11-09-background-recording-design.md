# Record and finish without opening the app — design

Owner request (2026-11-09): "I wanted transcribing to work in backend and notify user the progress and when the
recording is ready. why user should need to open the app continuously and also users should have a shortcut to
start voice recording, so it is not always required to open the app instead they can just automate using the
shortcut and this works in the backend."

## What we saw

The owner's phone (DB copied read-only 2026-11-09). The 81-minute and 36-minute recordings of 2026-11-08 ended with
full transcripts, 16 and 7 sections, and all three outputs, but the owner saw them stall. In code:
- After Stop, work runs in a `BGContinuedProcessingTask` (ContinuedWork.swift). When iOS ends it
  (`expirationHandler`), the pipeline aborts and shows "Paused when the phone locked. It will continue when you open
  Summaverick." (ProcessingPipeline.continuedWorkExpired). Nothing else resumes it unless overnight mode was accepted.
- Progress is reported per stage. During long stages (catch-up transcription of a long backlog, 16 section
  summaries, each output) the fraction can stand still for minutes. iOS may end continued-processing tasks that show
  no progress. This is a hypothesis; the device check confirms it with logs.
- There is no "notes ready" notification, and there is no way to start a recording without opening the app.

## Goals

1. A recording's work finishes without the person opening the app, unless iOS refuses every way of running it.
2. The person sees progress without opening the app, and is told when the notes are ready (tap opens the note).
3. The person can start and stop a recording without opening the app: Shortcuts, Siri, the Action button, and a
   Control Center / Lock Screen control. Each starts recording in the background.

## Non-goals and anti-goals

- No cloud, no push server: everything stays on the phone (standing constraint).
- No silent-audio tricks or other ways around iOS background rules (App Store rejection risk).
- Never record without the person's action: a shortcut run is the person's action. Recording consent must already
  be acknowledged; if not, the shortcut opens the app at the consent screen.
- Notifications must not spam: at most one per state change per recording (paused / ready / failed), with no
  per-percent notifications. Progress lives in the Live Activity and the continued-processing UI.

## Design

- **Notifications (NotesNotifier).** A local notification when notes finish while the app is not active:
  "Notes ready: <title>". Tapping it opens the note, through the existing `pendingRoute` path. When background work is
  cut off: "<title>: paused at 60%. It will finish while charging, or tap to finish now." With the same
  UNUserNotification permission as reminders. A failure gets its own notification with the failure line.
- **Automatic finish after a cut-off.** On expiry the interrupted note is put on the overnight list automatically,
  without the "Finish overnight?" offer, and a `BGProcessingTaskRequest` is submitted (OvernightWork). The ready
  notification fires when it finishes. Opening the app still resumes at once.
- **Progress heartbeat.** Every long stage reports sub-progress at least every 30 s, by piece, section, and generated
  tokens for the outputs. The continued-processing task then never stands still, and the Live Activity stays current.
- **Recording Live Activity (widget extension, iOS 18+).** While recording: elapsed time and a Stop button. After
  Stop, the same activity shows the stage line and percent while the app runs, and ends with "Notes ready" (or
  "Paused — tap to finish"). The iPhone 16e shows it on the Lock Screen and in the banner (no Dynamic Island).
- **Start without opening the app (iOS 18+).** `StartRecordingIntent: AudioRecordingIntent` and `StopRecordingIntent:
  LiveActivityIntent`, an `AppShortcutsProvider` ("Start recording in Summaverick", "Stop recording in Summaverick"),
  and a `ControlWidget` record toggle. Recording runs through a headless `RecordingController` shared by RecordView
  and the intents, so the app and a shortcut behave the same. Stop from the shortcut or Live Activity hands the audio
  to `ProcessingPipeline.recordingStopped` as the Stop button does.
- **What iOS allows after a background Stop is an open question.** Submitting a `BGContinuedProcessingTask` may be
  refused when the app is not in front. The code does not guess: it tries the continued-processing task first, and
  if `submit` throws it falls back to the automatic BGProcessingTask, with the Live Activity and a notification
  saying "Tap to finish now". The device check records which path iOS 27 takes.
- `requiresExternalPower` for the automatic BGProcessingTask is `false`, so iOS may run it while idle on battery. The
  device check reports whether it ran without a charger.
- iOS 17 devices keep today's behaviour (deployment target 17.0); the new parts are availability-gated.

## Done when (device, owner's iPhone 16e, iOS 27)

- An 80-minute recording, locked after Stop, finishes with no app open (via continued processing or the automatic
  BGProcessingTask), and the ready notification arrives. Logs show each path taken.
- A Shortcut / Action button / Control Center run starts recording with the phone locked; the Live Activity shows the
  time; Stop from the Live Activity produces notes and a ready notification.
- No new failure banners; recordings made in the app work as before.

## Left to the builder

Exact copy beyond the strings above (en + te), the Live Activity layout (follow the app's style), and how the widget
extension target is added in make_xcodeproj.py.
