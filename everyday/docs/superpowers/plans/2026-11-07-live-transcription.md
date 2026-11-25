# Live transcription while recording — brief

Owner-approved in chat on 2026-11-07. Branch: `feature/personal-assistant`. Builds on the model auto-download work (`2026-11-07-model-autodownload.md`, already merged into this branch).

## Why
Owner tested long recordings: transcription only starts after Stop, runs only with the screen on, and pauses every time the phone locks, so a session recorded for hours feels "never ending". Owner wants it done inline while recording, as the cloud voice assistants feel, with no cloud, and in the pocket.

## Measured facts (spike on iPhone 16e, iOS 27.0.1; commit on branch `spike/live-transcribe`, throwaway)
- Gemma 4 E4B, 28 s Telugu pieces. GPU (Metal) in front: 0.78–1.01× real time; heat reached "serious" within a minute.
- GPU in the background fails immediately (`kIOGPUCommandBufferCallbackErrorBackgroundExecutionNotPermitted`), even while recording. No iPhone gets background GPU.
- CPU (`n_gpu_layers = 0`, mtmd `use_gpu = false`), 4 threads: 1.00× in front, 1.14–1.25× while locked; app stays alive because the recording audio session is active; heat stayed nominal/fair; same transcript quality. 6 threads is slower (1.5×). Use 4.
- Model load takes about 5 s for either backend.

## Decisions (binding)
1. **Transcription always runs on the CPU** (live and catch-up), 4 threads. Notes, titles, extraction and Ask use the GPU while the app is active and not recording, otherwise the CPU. `CoreEngine.loadModel` takes a backend (`.gpu`/`.cpu`) and reloads only when it differs from what is loaded (the projector follows: `use_gpu` must match). Pick the backend right before each model run, never mid-run.
2. **Core: incremental transcription** (portable C++, logic stays in `core/`):
   - WAV reader tolerates a file still being written: if the `data` chunk size is 0, `0xFFFFFFFF` or larger than what is on disk, use the bytes on disk (whole 16-bit frames only).
   - Pure function that plans the next piece from a sample offset: returns `wait` (not final and fewer than 28 s + 3 s search window available after start), `end` (nothing left, or under 0.5 s left when final), or a piece `[start, end)` cut at the quietest point near 28 s, the same rule as today (when final, the last piece takes what is left). Unit-test it without a model.
   - New C API: `char *sve_transcribe_next_piece(sve_ctx*, const char *wav_path, int64_t start_sample, int is_final, const char *prev_text, sve_progress_cb, void *user)` returning JSON `{"status":"piece","start_sec":N,"end_sample":N,"text":"…"}` / `{"status":"wait"}` / `{"status":"end"}`, or NULL on failure/cancel. Same prompt, vocabulary line and previous-tail context as the current `transcribe_pieces`. Refactor `transcribe_pieces` to use the same planner and prompt builder so both paths cannot drift. Existing callers keep working.
3. **Progress lives next to the audio** in a sidecar `<audio>.live.json`: `{"cursor_sample":N,"pieces":[{"start_sec":N,"text":"…"}]}`, written atomically after every piece. Every transcription (live, after Stop, resume after interruption, notes recorded before the AI was ready, Generate notes) goes through one Swift loop that starts from the sidecar's cursor and appends. So an interruption never restarts from zero. When the transcript is complete and saved to the note, delete the sidecar. Delete it too wherever audio is deleted (cancel, note delete, retention, wipe).
4. **LiveTranscriber** (new, `Engine/`): starts when a recording starts (not for Ask voice questions), only if the AI is ready and the device is supported. Loop: ask the core for the next piece of the growing file; on `wait`, sleep about 3 s; on a piece, save the sidecar and publish the text. Pauses (recording continues) while the phone is too hot (`Heat.isTooHot`) or the battery is under 20% and not charging; resumes when that clears. On Stop it finishes the piece in flight (or cancels it if it would delay a cancel), then hands over to the pipeline, which continues from the cursor with `is_final = 1`. Only one model run at a time: the pipeline must not start transcribing while the live loop holds the engine.
5. **Recording screen** shows the live transcript (last few lines, newest at the bottom, scrolls) and one status line: "Writing as you talk", "About N min behind", "Paused while the phone cools down", "Paused: battery low — notes will be written later", or "Notes will be written after you stop" (AI not ready). Keep the existing elements. Stop's label stays "Stop and make notes".
6. **After Stop, keep working in the background.**
   - Keep `beginBackgroundTask` as today.
   - On iOS 26+, also submit a `BGContinuedProcessingTaskRequest` when processing starts after Stop (identifier `com.summaverick.everyday.notes.<noteID>`, registered as the wildcard `com.summaverick.everyday.notes.*` in `BGTaskSchedulerPermittedIdentifiers` and with `BGTaskScheduler.shared.register` at launch). Title "Writing your notes", subtitle with the stage. Report progress (transcribed seconds / duration, then outputs). Do not request `.gpu`.
   - In the background, model runs use the CPU. If a GPU run fails because the app went to the background while a continued-processing task is held, retry that stage on the CPU once. Without that task, keep today's behavior (interrupted, resumes when the app is active).
   - On expiration, cancel the run and mark the note interrupted (it resumes from the sidecar). Call `setTaskCompleted(success:)` exactly once.
   - Resume of interrupted and waiting notes may also run in the background while a continued-processing task is held; otherwise only while active, as today.
7. **Strings:** every new string in `Localizable.xcstrings` with English and Telugu, keeping the file's format (`json.dumps(d, ensure_ascii=False, indent=2)+"\n"`, no key sorting). `ios/tools/check_strings.py` must print STRINGS OK.
8. **Out of scope:** speaker labels, streaming partial words inside a piece, notes written during recording, cloud anything.

## Verify
- `bash core/tests/run.sh` passes, with new tests for the growing-WAV reader and the piece planner (wait/end/cut/final cases, a header with data size 0).
- `core/tests/smoke_gemma.sh` gains a mode that runs `sve_transcribe_next_piece` in a loop over a WAV on the Mac (CPU) and prints each piece; include its output.
- Pure Swift logic you add (live pause rules for heat/battery, backend choice, "minutes behind") lives in small functions checked with a standalone `swiftc` script; include its output.
- iOS build with signing for device `00008140-001668892E22801C`: no new warnings.
- List the device checks for the owner: live text appears while recording with the screen on; lock the phone for 5 minutes mid-recording and the text has advanced when unlocked; Stop after 10 minutes and notes are ready within a few minutes; lock the phone right after Stop and the lock screen shows progress and notes finish; force-quit during processing and it resumes from where it was, not from zero; low battery and heat pauses.
