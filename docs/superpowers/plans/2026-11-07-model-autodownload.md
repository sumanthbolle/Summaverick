# Model auto-download, Qwen removal, thermal and device checks — brief

Owner-approved in chat on 2026-11-07. Branch: `feature/personal-assistant`.

## Goal
The person never sees or picks an AI model. Gemma 4 E4B (weights + audio projector, about 5.5 GB) downloads by itself after install, reliably, and the app handles heat and phones that are too small.

## Decisions (binding)
1. **Remove Qwen from the app.** `ModelCatalog` keeps only the Gemma 4 E4B spec. On first launch of this version, if a Qwen file exists in the models folder, delete it (frees space) and select Gemma. The core keeps its non-audio code paths (shared with the future device); only the iOS app stops offering Qwen. Apple Speech is no longer used for transcription: remove its use from the pipeline and stop asking for speech-recognition permission (microphone permission only). Leave `SpeechTranscriber.swift` deleted or unused — prefer deleted, with all references removed. "Needs Gemma 4" states become "AI is still downloading" states (see 4).
2. **Hide the model.** Settings replaces the "AI model" section with a "Summaverick's AI" section: status (Downloading n% / Ready · 5.5 GB / Not downloaded), "Download again" (when missing or failed), "Use mobile data for the download" toggle, and "Remove to free space" (confirmation: notes stay, new recordings wait until it is downloaded again). No model names anywhere in the UI.
3. **Automatic, robust download.**
   - Background `URLSession` (identifier `com.summaverick.everyday.model`), `isDiscretionary = false`, `sessionSendsLaunchEvents = true`; handle `application(_:handleEventsForBackgroundURLSession:completionHandler:)` via a `UIApplicationDelegateAdaptor`.
   - Starts automatically on launch whenever any model file is missing, if the device is supported (5) and there is space (below). Wi-Fi only by default (`allowsCellularAccess = false`); the mobile-data toggle allows cellular.
   - Space check before starting: `volumeAvailableCapacityForImportantUsage` ≥ remaining bytes + 1 GB; otherwise show "Free up about N GB to download Summaverick's AI." and don't start.
   - Resume: keep resume data on failure/cancel (`NSURLSessionDownloadTaskResumeData`), retry automatically with backoff (30 s, 2 min, 10 min, then on next launch). Verify the finished file size equals the expected size (HTTP Content-Length, or the size recorded from the response); delete and retry on mismatch. Exclude from backup (already done).
   - Both files download; progress is combined by bytes (existing logic).
4. **First-run experience.**
   - Today shows a download card while the AI is not ready: "Getting Summaverick ready… n% · about X min left" (or "Waiting for Wi-Fi" / "Paused — no space" / "Download failed — Try again"), with the mobile-data toggle.
   - The profile setup sheet shows the same one-line status at the bottom.
   - Recording works before the download finishes. Recordings made then are saved and show "Waiting for Summaverick's AI to finish downloading". When the model becomes ready, all notes with audio and no transcript are processed automatically, one at a time (reuse the interrupted-resume mechanism; process only while the app is active).
   - Ask and assistant sections show "Summaverick is still getting ready" with progress instead of "Needs Gemma 4".
5. **Unsupported phones.** If `ProcessInfo.physicalMemory` < 5.5 GiB (i.e. under a 6 GB iPhone), don't download. Show once (and in Settings): "This iPhone doesn't have enough memory for Summaverick's AI. You can still record and play back; notes need an iPhone with 6 GB of memory or more." Recording still works.
6. **Heat.** Observe `ProcessInfo.thermalState`. Before starting any model run, and between pipeline stages, if the state is `.serious` or `.critical`: stop and mark the note interrupted with "Paused to let the phone cool down. It will continue automatically." During transcription, make the progress callback cancel when the state becomes `.serious`/`.critical` (treat the resulting failure as an interruption, not an error). Resume interrupted notes when the state returns to `.nominal`/`.fair` and the app is active (`thermalStateDidChangeNotification`).
7. **Strings:** every new string in `Localizable.xcstrings` with English and Telugu; `ios/tools/check_strings.py` must print STRINGS OK.
8. **Out of scope:** Apple-hosted Background Assets (needs a paid account and TestFlight — later), processing while recording (separate spike).

## Verify
- `bash core/tests/run.sh` passes.
- iOS build with signing: no new warnings.
- Pure logic you add (space check maths, retry schedule, RAM gate, "which notes to process when ready") lives in small functions you can check with a standalone `swiftc` script; include its output.
- List the device checks for the owner: fresh install downloads on Wi-Fi; airplane mode mid-download then back resumes; leaving the app keeps downloading; record before the model is ready, then the note processes automatically; Settings shows status and Remove/Download again works.
