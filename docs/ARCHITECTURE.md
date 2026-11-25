# Summaverick Everyday: architecture

Written 2026-11-12 from the code and the specs in `docs/superpowers/`; updated the same day after the Whisper build (commits `569508a`..`d115f17`). Section 9 is the history, day by day, and section 10 lists what is still open. Where a spec and the code disagree, this file follows the code. The plan checkboxes in `docs/superpowers/plans/` are all unticked, so they say nothing about status. Use the table in section 8.

## 1. What the app is, and its hard rules

An iPhone app that records a lecture, meeting or doctor's visit (Telugu, English or a mix) and writes a short summary, detailed notes and revision questions. It also keeps a "second brain": things said, people, promises, reminders. Everything runs on the phone. No account, no server.

Hard rules. Each one is enforced in code, not just policy.

| Rule | Where it holds |
|---|---|
| On-device only. The one network use is the model download. | `ios/SummaverickEveryday/Engine/ModelManager.swift` (background URLSession) |
| No AI wording in the UI. A person advising, not a machine. | `docs/superpowers/2026-11-09-voice-guide.md`; strings in `Localizable.xcstrings`; `ios/tools/check_strings.py` checks every string has a Telugu value |
| Medicine and health answers are built in code. The model may not write them. | `core/src/sve_medcheck.cpp`, `sve_health.cpp`, `sve_chat.cpp` (medicine sentences are held back from model text and quoted from the recording) |
| Nothing is saved or rings until the person taps Yes. | `suggestions` table, `sve_suggestion_accept/reject`, `Views/Today/SuggestionCard.swift` |
| One model in memory at a time. | `CoreEngine.swift`: loading Whisper frees Gemma and loading Gemma frees Whisper (`ModelSlot`); `ensureGemmaLoaded()` brings Gemma back for any caller. Whisper (~1.1 GB) and Gemma (~4.9 GB) do not fit together on 8 GB. The embedder is the one planned CPU-only exception (section 5). |
| No model on the Mac for testing. | Core tests use only `clang++` and SQLite, and the model-free API check. Model runs happen on the phone. |
| Plain voice for medicine, privacy, consent, "Delete everything". | Voice guide, "Where the voice stays plain" |

Other privacy rules: audio is deleted a retention window (7 days default) after its usable transcript was saved (`notes.transcribed_at`; a recording still waiting to be transcribed is never removed by age), notes stay until the person deletes them, the `Summaverick` and `Chats` folders (notes database, recordings, transcript checkpoints, chats, activity log) and the model files are excluded from iCloud and computer backups (`PrivateStorage` in `Security/AppLock.swift`, at every launch), "Delete everything" succeeds only when no note and no file in the recordings folder is left, and the app is behind Face ID / passcode (`Security/AppLock.swift`).

Checking agent: a note's outputs are owed a check from the moment they are stored (`ProcessingPipeline.checksDue`, kept across launches). A hot phone, a stop, a failure or a closed app leaves the check due; the resume loop runs it later on the stored outputs. The note shows "Notes available · checks pending" until there is a verdict ("Checked against the recording", or "Finished · N items need review"). A check that fails 3 times is given up and the note claims no verdict. The verifier (`core/src/sve_verify.cpp`) checks every number first, also in a one-word answer, and flags a decision whose closest transcript clause has the opposite polarity ("did not approve" against "approved") unless the judge confirms it.

## 2. Layers

```
+--------------------------------------------------------------------------+
| SwiftUI views     ios/SummaverickEveryday/Views/                         |
|  Today  Record  Notes (NoteDetail, Chapters, FinishNow)  Ask  DeepDive   |
|  Map (knowledge graph)  Memory  Profile  Setup  Settings                 |
+--------------------------------------------------------------------------+
| Swift engine      ios/SummaverickEveryday/Engine/, Audio/                |
|  RecordingController / AudioRecorder   LiveTranscriber + TranscriptRun   |
|  ProcessingPipeline   TranscriptSidecar   SectionWriter                  |
|  ModelManager (download)   CoreEngine (+Assistant, +Memory)              |
|  HeatGovernor (heat ladder)   ContinuedWork   OvernightWork              |
|  NotesNotifier   ReminderScheduler   ChatStore   LiveActivityLogic       |
|  ModelLogic.swift: pure decision logic, checked with swiftc scripts      |
+--------------------------------------------------------------------------+
| Widget extension  ios/SummaverickWidgets/ (Live Activity, Record control)|
|  shares ios/Shared/ (RecordingActivity, intents, Theme)                  |
+--------------------------------------------------------------------------+
| C bridge          ios/SummaverickEveryday/Bridging.h -> core/include/    |
|                   sve_core.h  (the whole contract, ~517 lines, sve_*)    |
+--------------------------------------------------------------------------+
| C++ core          core/src/  (compiled straight into the app target)     |
|  sve_core / sve_assistant (API)   sve_store (SQLite + FTS5)              |
|  sve_llm, sve_audio (Gemma + mmproj)   sve_sections, sve_assemble        |
|  sve_extract, sve_suggest, sve_medcheck, sve_health, sve_learn           |
|  sve_memory, sve_consolidate, sve_brain, sve_brainmap                    |
|  sve_chat, sve_textmatch, sve_embed, sve_context, sve_budget, sve_threads|
|  sve_whisper_text (pause cuts, groups, repeat cleanup, language choice)  |
|  whisper.cpp (vendored, MIT, -O3 always) built against llama's ggml      |
+--------------------------------------------------------------------------+
| llama.cpp / ggml  ios/Frameworks/llama.xcframework (Metal, prebuilt)     |
| mtmd (audio projector)   SQLite (system lib)                             |
+--------------------------------------------------------------------------+
```

Key points:

- `core/` is portable. The same sources are meant to move to Android and the hardware device. On iOS they are compiled into the app target by `ios/make_xcodeproj.py` (the `CORE_SRC` path). `core/CMakeLists.txt` is the Linux/device build and the `sve` CLI; it is not used by Xcode.
- The Swift engine owns time, heat, iOS lifecycle and notifications. The core owns text, prompts, SQL, parsing and checks. Decision logic that does not need UIKit lives in `ModelLogic.swift` so it can be tested without a phone.
- `CoreEngine` is an actor that holds two `sve_ctx` handles on the same database file: one for model calls, one for store-only calls ("CoreStore") so a read never waits behind a minutes-long model call. SQLite runs in WAL mode with a 5 s busy timeout (see `sve_core.h` header comment).
- Rules of the C API: strings are UTF-8, every returned `char*` is freed with `sve_free_string`, functions return 0 or negative on failure, one `sve_ctx` is used by one thread at a time.
- `ProcessingPipeline.swift` is the largest file (about 2,900 lines) and `ModelLogic.swift` the second (about 2,200). Most behaviour questions end up in one of them.

## 3. Recording to notes

1. **Record.** `Audio/RecordingController.swift` and `AudioRecorder.swift` write a 16 kHz mono WAV in the app sandbox. The audio session keeps the app alive with the phone locked. A Live Activity (`SummaverickWidgets/RecordingLiveActivity.swift`) and a Record control start and stop it. `Views/Components/RecordingPulseView.swift` and `ScreenAwake.swift` handle the screen. A note row is created with `sve_note_create`.
2. **Stop.** Recording ends. The recording is compressed later (`AudioCompressor.swift`). Transcription continues from the saved cursor.
3. **Sidecar.** Progress sits next to the audio in `<audio>.live.json` (`Engine/TranscriptSidecar.swift`): `cursor_sample`, finished `pieces` (`start_sec`, `text`, and for Whisper's unsure groups `pending` with `start_sample`/`end_sample`), finished `sections`, and `parts`. It is written atomically after every piece, section or part. Any interruption (crash, kill, heat pause) resumes from the cursor. The sidecar is deleted with the audio after the transcript is saved to the note.
4. **Transcript pieces.** Two engines, decided in `ProcessingPipeline.transcribe` by `WhisperFlow.next(...)`:
   - **Whisper first** (when the model file is present and verified). `TranscriptRun.runWhisper` calls `sve_whisper_next_piece` group by group (up to 28 s, cut at pauses). Each group is either Whisper text, or marked `needs_gemma` and saved as a *pending* piece in the sidecar. Details in section 6.
   - **Gemma fills the rest.** Whisper is freed, Gemma loads, and `TranscriptRun.fillPending` sends each pending piece's exact samples to `sve_transcribe_span`, with the previous piece's text as context.
   - **Gemma only** (no Whisper file, or the app in the background): `TranscriptRun` cuts ~28 s pieces and sends each to Gemma 4 E4B through its audio projector (`sve_load_audio`, `sve_set_audio_threads`). Gemma listens to the audio itself, so Telugu, English and mixed speech come out as text. In the background a Whisper run hands off cleanly to this path from its cursor.
   - Transcribing while still recording is off by default (commit `83a7603`); every piece is written after Stop unless live transcription is turned on in settings (`LiveTranscribe` in `ModelLogic.swift`). Live transcription always uses Gemma.
5. **Sections.** The pieces are grouped into sections (`core/src/sve_sections.cpp`, `sve_section_summarize`). Each section gets a title, a topic, points with `start_sec`, terms and hints (`SectionSummary` in the sidecar file). Very long recordings become many sections.
6. **Outputs.** `sve_assemble` and `sve_section_output_put` write the notes (kind 1) and revision questions (kind 2) part by part, then the short summary, in small saved steps (`step_attempts` records tries). `Views/ChaptersView.swift` and `NoteDetailView.swift` show them. A part that already exists for the same language and piece range is not redone.
7. **Extraction.** `sve_extract` reads the transcript for reminders, tasks, medicines and facts. They become `suggestions`, shown as cards on Today. `sve_medcheck` compares medicines with the doctor's exact words and the person's profile. It runs after the title, summary, notes and questions are stored, never before them (`ExtractAfterNotes` in `ModelLogic.swift`; 2026-11-14, after note 41's extract ran 47 minutes before its summary and found nothing). A stopped or cut-off extract goes on the find-reminders queue and runs again by itself; sections-after (notes first), memories and insight follow it.
8. **Memories.** After the notes, one section at a time, `sve_memories_from_section` writes memories. They never come before the notes, never hold them up and never fail them. Older notes get theirs when the app is idle in front, or overnight.
9. **Embeddings.** Core code exists to embed pieces and memories (`sve_embed_pending`). The iOS app does not load an embedder yet (section 5).
10. **Notifications.** `NotesNotifier.swift` posts local notifications only while the app is not in front. One identifier per note (`notes.<id>`), at most one per stage per run. A multi-part run says "1 of 3 ready", "2 of 3 ready", "3 of 3 ready" in the subtitle (what was said, summary, notes). Paused and failed get their own text. Tapping opens the note via `ReminderScheduler.pendingRoute`. Wording follows the voice guide.

### Background, locked and hot behaviour

Measured on the owner's iPhone (memory note "iPhone background inference"): CPU runs at about 1.1x real time with the phone locked. GPU (Metal) work is refused by iOS in the background.

- **Backend choice.** In front, the notes steps use the GPU. In the background they use the CPU (`ModelBackend` in `CoreEngine.swift`). A run in progress moves to the GPU at its next step when the app comes to the front. Pieces use the GPU only at full heat.
- **Threads.** `HeatThreads` / `ThreadChoice` pick CPU threads from the heat level, whether the app is recording or in the background. The core applies them before each CPU run (`sve_set_threads`, `sve_threads.h`).
- **Heat ladder.** `HeatGovernor.swift` and `HeatLadder` in `ModelLogic.swift` use four levels: `full`, `eased`, `minimal`, `paused`. Inputs are thermal state, battery and Low Power Mode. A level drops back by itself after about a minute of calm. At `paused`, the note keeps what is done and resumes later (stage `.paused`). At serious heat the pipeline rests between pieces (stage `.cooling`). The Record screen shows a tip when the phone is warm.
- **Locked phone.** `ContinuedWork.swift` uses a continued-processing task on iOS 26+ (`BGContinuedProcessingTask`) so work goes on after the person leaves.
- **Overnight.** If cut off with the app not in front, the note goes on the overnight list. `OvernightWork.swift` (`BGTaskScheduler`) finishes it, on battery too, and `NotesNotifier` says when the notes are ready. Opening the app resumes at once. A long recording can be chosen for overnight while charging.
- **Before the model exists.** Recordings made before the download finishes wait and are processed one at a time once it is ready.
- **"Write my notes" / Finish now.** `Views/FinishNowView.swift` shows the whole job, one measure and an estimate that counts the transcript too. The person can also tap Write my notes on a note.

## 4. Data on disk

SQLite file in Application Support, schema in `core/src/sve_store.cpp`. Full-text search uses FTS5.

| Table | Purpose |
|---|---|
| `notes` (+ `notes_fts`) | One row per recording: title, lang, duration, audio path, transcript, short summary, detailed notes, revision questions |
| `settings` | Key/value settings kept by the core |
| `transcript_pieces` (+ `pieces_fts`) | The ~28 s pieces with start time, for search and "play from here" |
| `sections` | Per-note section ranges and each section's summary JSON |
| `section_outputs` | One section's notes (kind 1) or questions (kind 2), written part by part |
| `step_attempts` | How many times each notes step was tried, to avoid loops |
| `medicines` | The person's profile medicines (name, dose, timing) |
| `suggestions` | Reminder, task, medicine and fact cards waiting for Yes / Edit / No |
| `reminders` | Accepted reminders; `ReminderScheduler` turns them into local notifications |
| `tasks` | Accepted to-dos, with done flag |
| `topics` | Topics and people (one table; "entities") |
| `note_topics` | Which topics a note covers |
| `memory` (+ `memory_fts`) | Older learning memory: words and preferences the app learned from corrections |
| `memories` (+ `memories_fts`) | The second brain. Types: fact, decision, promise, question, person, idea. Has who, to_whom, due date, confidence, review state (new/kept/corrected/wrong), health flag, done, importance, origin, superseded_by, conflict_with |
| `memory_sources` | Which note, piece and time each memory came from |
| `memory_entities` | Links a memory to topics/people with a role (who, to_whom, mentioned, topic) |
| `memory_progress` | Which sections have had memories written, so work resumes |
| `person_questions` | "Is this the same person?" questions about two similar names |
| `decisions` | Log of what the person changed (kind, field, from, to), feeding learning |
| `insights` | One insight a day for Today, with dismiss flag |
| `embeddings` | Meaning-search vectors (256 floats) for a piece or a memory, with the model that made them |

Files on disk:

| What | Where |
|---|---|
| Notes database | Application Support, app sandbox |
| Recordings (WAV, later compressed) | App sandbox, path in `notes.audio_path` |
| Sidecar `<audio>.live.json` | Next to each recording, removed with it |
| Models and `.resume` files | `ModelManager.modelsDirectory`; `gemma-4-E4B-it-Q4_K_M.gguf`, `mmproj-gemma-4-E4B-it-Q8_0.gguf`; excluded from iCloud backup |
| Chat history | `Engine/ChatStore.swift` |

## 5. Second brain, knowledge graph and Ask

**Memories** (`sve_memory.cpp`, `sve_consolidate.cpp`). Each section yields typed memories with a quote and source. Consolidation merges duplicates, marks superseded or conflicting ones, and scores importance. Low-confidence or health-related memories wait for the person: Keep, Corrected or Wrong (`MemoryReviewCard.swift`). Waiting points show on the map with a "?".

**People and promises.** People are `topics` rows linked through `memory_entities`. A promise is a memory with `who`, `to_whom` and a due date. "Same person?" prompts come from `person_questions`. Today shows reminders, open to-dos, promises and one insight (`Views/Today/TodayView.swift`).

**Knowledge graph.** `sve_brain_map_json` (`sve_brainmap.cpp`) builds `{center, list, branches, links}` for one note or all recordings. `Views/Map/BrainMapView.swift` and `BrainMapLayout.swift` draw it. The owner wants 3D animation and rich styling here (memory note "Mind map design"); treat the current view as the first version. The UI calls it Knowledge graph.

**Retrieval for Ask** (top priority per the owner).
- Keyword: FTS5 over notes, pieces and memories, plus `sve_textmatch.cpp` (Telugu and English, mixed and romanised spellings).
- Meaning: `sve_embed.cpp` holds the vector maths, rank fusion and format. EmbeddingGemma, a second CPU-only model, cut to 256 dimensions. With no embedder loaded, search is keyword order only.
- `sve_search_json` returns fused results. `sve_budget.h` and `sve_context.cpp` fit the chosen passages into the model's context.
- **Gap:** the C API has `sve_load_embedder`, but nothing in `ios/` calls it and `ModelCatalog` lists only Gemma. So on the phone today, Ask is keyword-only.

**Chat.** `sve_assistant_chat` (`sve_assistant.cpp`) with `sve_chat.cpp` handles the question's language, earlier turns for follow-ups, and the visible answer (thinking and citation labels hidden). Answers come only from the person's recordings, each links to its source, and if the recordings do not contain it the app says so. Voice questions use the same path. `Views/Ask/AskView.swift`.

**Health and medicine.** `sve_health.cpp` flags health questions (English, Telugu, mixed, romanised) and errs on the side of flagging. A flagged question is never answered from general knowledge. Medicine sentences are built from stored data and quoted from the recording, not written by the model. `sve_medcheck.cpp` shows the yellow "Please check" banner and asks "Save anyway?" when name, dose or time does not match.

**Reminders.** Accepted reminders are scheduled as local notifications by `ReminderScheduler.swift`. Medicine reminders keep the plain tone: title "Summaverick", body the exact words.

**Learning.** `sve_learn.cpp` and the `memory` / `decisions` tables. When the person makes the same correction more than once (for example moving a reminder time), it offers a preference. Review or delete them in Profile, "What Summaverick has learned" (`Views/Profile/LearnedView.swift`).

## 6. Models

| Model | Role | Files | Status |
|---|---|---|---|
| Gemma 4 E4B, Q4_K_M | Listens to audio, writes transcript, sections, notes, memories, answers | `gemma-4-E4B-it-Q4_K_M.gguf` (unsloth), about 5 GB with the projector | In use |
| Audio projector (mmproj) | Lets Gemma hear the audio | `mmproj-gemma-4-E4B-it-Q8_0.gguf` (ggml-org, about 560 MB; a slimmer file is planned, see `ios/tools/model/`) | In use |
| EmbeddingGemma (256 dims) | Meaning search | not downloaded by the app yet | Core ready, not wired on iOS |
| Whisper te-medium (vasista22, Apache-2.0), ggml q8_0 | Telugu transcript pieces | `ggml-te-medium-q8_0.bin`, 823,369,779 bytes | Built in; inactive until the file is on the phone |
| Qwen3 0.6B / 1.7B | Old summariser | retired (`ModelCatalog.retiredFilePrefixes` deletes old files) | Gone from the app. Still referenced in the root README and `core/README.md` (stale) |

- **Download.** `ModelManager` downloads by itself after install, in a background URLSession (`com.summaverick.everyday.model`), resumes after failure, checks size and free space, and posts `.aiReady`. The person never picks a model. `DeviceGate` refuses phones without enough memory.
- **Memory.** The app needs the Increased Memory Limit entitlement (`SummaverickEveryday.entitlements`) so the 5 GB model fits. It is the only extra entitlement, because personal-team signing allows nothing else.
- **Contexts.** `ModelSpec.contextTokens` is 8192 for Gemma. `CoreEngine.loadModel` defaults to 4096 when not told. Long transcripts are cut by section and by part so no single call needs more than the context. `sve_transcript_fits` tells whether a text fits.
- **One model at a time.** Gemma is unloaded and reloaded when the backend changes (CPU to GPU) via `lastLoad` in `CoreEngine`.

### Whisper Telugu transcripts (built 2026-11-12, not yet tested on the phone)

Why: Gemma transcription took about as long as the audio or longer on the iPhone 16e and sometimes looped. Spec `docs/superpowers/specs/2026-11-12-whisper-telugu-transcripts-design.md`, plan `docs/superpowers/plans/2026-11-12-whisper-telugu-transcripts.md`.

**Engine.** whisper.cpp (MIT) vendored into `core/src` (`whisper.cpp`, `whisper-arch.h`, `whisper-logging.h`, `core/include/whisper.h`), compiled into the app against the ggml inside `llama.xcframework`, so there is one ggml. `ios/make_xcodeproj.py` defines `SVE_WITH_WHISPER=1`, finds `ggml.h` under `$(BUILT_PRODUCTS_DIR)/llama.framework/Headers`, and compiles `whisper.cpp` at -O3 even in Debug (Debug -O0 was about 10x slower). The Neural Engine path is stubbed (`sve_whisper_stubs.cpp`). Two small SVE patches, to re-apply when whisper.cpp is updated: `whisper_set_audio_ctx_with_state`, so the language check only encodes the real length of a piece; and encoder reuse (`whisper_set_encode_reuse_with_state`), so a piece's decode reuses its language check's encoder run instead of encoding the same audio twice. Reuse is taken only when the mel (fingerprinted), offset and encoder size match; `SVE_WHISPER_REENCODE=1` turns it off to compare text both ways. Each piece logs `whisper piece N s: lang X ms, decode Y ms, encoder reused|run again`.

**Pure helpers** (`core/src/sve_whisper_text.h/.cpp`, tested in `core/tests/test_whisper_text.cpp`):
- `plan_group`: 20 ms loudness frames, smoothed over +/-7 frames; spans of 4-10 s cut at the quietest frame; spans joined into groups of at most 28 s. It waits when the recording is still growing and less than 31 s is available, and ends when final with under 0.5 s left.
- `collapse_repeats`: a word or short phrase repeated more than 3 times in a row is cut to one copy (the ticking-noise loops); text that is only such a run becomes empty.
- `lang_choice`: Telugu if p_te >= 0.6 and p_te - p_en >= 0.3, otherwise Gemma.

**C API** (`sve_core.h`):
- `sve_load_asr(ctx, path, use_gpu, n_threads)`, `sve_unload_asr`, `sve_asr_loaded`.
- `sve_whisper_next_piece(ctx, wav, start_sample, is_final, cb, user)` returns JSON: `{"status":"piece","start_sec","end_sample","text","conf","needs_gemma"}`, or `wait` / `end`. Each span is language-checked; if any span in a group is unsure the whole group is `needs_gemma`. `conf` is the mean token probability over kept spans (null when none).
- `sve_transcribe_span(ctx, wav, start, end, prev_text, cb, user)` is Gemma on an exact sample range, returning `{"text","conf"}`.
- Decoding: greedy, `no_timestamps=true` (timestamp mode broke the first letter of every piece), temperature fallback off, `no_context=true`, `audio_ctx = min(1500, samples*50/16000 + 64)`, language `te`.

**Swift side:**
- `ModelLogic.swift`: `WhisperChoice` (GPU whenever the app is in front and heat is not `paused`, re-decided per group; CPU speed in the background is not measured, so `cpuFasterThanAudio=false` sends background work to Gemma), `WhisperFlow.next` (`.whisper(backend)`, `.gemmaRemainder`, `.fillPending`, `.done`), `ModelSlot`, `WhisperFile` (size + SHA-256 verdict: ok / missing / bad), and progress that leaves pending audio out until Gemma fills it.
- `CoreEngine.swift`: `loadWhisper`, `unloadWhisper`, `whisperNextPiece`, `transcribeSpan`, `ensureGemmaLoaded()`; logs `[sve] model swap: whisper->gemma N s`.
- `TranscriptSidecar.swift`: `appendPending`, `pendingIndices`, `fill` (empty text removes the piece); `previousText` skips pending pieces.
- `ModelManager.swift`: a second `ModelPart` "whisper", downloaded after Gemma in the same background session; verified once by size and SHA-256 `0f71156c...98c3ad` (cached as `whisper.verified` = size:mtime), excluded from backup. A bad file is deleted and fetched again. `WhisperHost.url` holds an `OWNER_REPO` placeholder, so nothing downloads until the owner's Hugging Face repo is set. Model card for the upload: `tools/model/whisper-te-README.md`.
- DEBUG hook: launch with `SVE_WHISPER_RETRANSCRIBE=<note id>` (optional `SVE_WHISPER_BACKEND=gpu|cpu`) to re-run one note through Whisper on a temporary copy of its WAV; the note is not changed.

**Spike measurements** (branch `spike/whisper-telugu`, note 25 = 10 min Telugu, note 22 = 10 min English, hot phone; "clean chars" leaves out loop lines; Gemma wrote 3729):

| Setting | Clean chars | Time per audio second |
|---|---|---|
| 10 s pieces, full encoder | 4080 | 0.62 |
| 10 s pieces, encoder sized to piece | 3341 | 0.41 |
| 16 s pieces, sized | 3034 | 0.35 |
| 16 s pieces, full | 2917 | 0.49 |

Rejected along the way: stock whisper large-v3-turbo (garbage on Telugu), 15 s fixed chunks with timestamps (broken first letters, ~60% of Gemma's text). Found: English comes out in Telugu letters with `lang=te`, hence the language check and Gemma fallback.

## 7. Build and test

**Generate the Xcode project.** `cd ios && python3 make_xcodeproj.py`. It writes `SummaverickEveryday.xcodeproj` with no XcodeGen. Object IDs come from file paths, so output is stable. It builds two targets: the app and `SummaverickWidgets` (bundle id `com.summaverick.everyday.widgets`, embedded). `Shared/*.swift` compiles into both. `core/src` is added to the app target. Re-run it when Swift files are added or renamed. `project.yml` is an unused XcodeGen alternative.

**llama.** `cd ios && ./get-llama-xcframework.sh` downloads the prebuilt `llama.xcframework`; the version is pinned in `ios/Frameworks/llama.version`.

**Device build.** Open the project in Xcode 16+, set the Team under Signing & Capabilities, pick a connected iPhone (iOS 17+), Run. Do not add capabilities; a personal team will fail signing. Background continued processing needs iOS 26.

**Core tests (no phone, no model).**
- `bash core/tests/run.sh` compiles each `core/tests/test_*.cpp` with the pure core sources using `clang++ -std=c++17 -DSVE_TEST_MAIN` and `-lsqlite3`, runs them and prints a TOTAL line. Covers store, extraction, medicine checks, learning, sections, memory, brain map, consolidation, chat, assemble, embed, budget, threads, Whisper text helpers. Last run 2026-11-12: 395 passed, 0 failed.
- `bash core/tests/smoke_gemma.sh api` checks the C API with no model.
- `smoke_gemma.sh extract` and `transcribe` need Gemma and run on a machine with llama.cpp. These contradict the "no model on the Mac" rule for day-to-day work; use them only if the owner agrees.

**Swift checks.** Pure Swift files (`TranscriptSidecar.swift`, `ModelLogic.swift`, `LiveActivityLogic.swift`) have no UIKit and are checked with standalone `swiftc` scripts. `python3 ios/tools/check_strings.py <derived-data-dir>` confirms every string is in the catalog with a Telugu value.

**Linux / device core.** `cmake -S core -B core/build` with sibling checkouts `deps/llama.cpp` and `deps/whisper.cpp` builds `libsvecore` and the `sve` CLI.

## 8. Status

Built means present in code. "Verified on phone" is not claimed here; I did not run anything.

| Feature | Status | Evidence |
|---|---|---|
| Record 16 kHz WAV, background recording, Live Activity, Record control | Built | `Audio/`, `SummaverickWidgets/` |
| Transcript pieces via Gemma 4 E4B audio, sidecar cursor, resume | Built | `LiveTranscriber.swift`, `TranscriptSidecar.swift` |
| Live transcription while recording | Built, off by default | commit `83a7603` |
| Sections, notes, questions, summary in small saved steps (engine v2) | Built | `sve_sections`, `sve_assemble`, `SectionWriter.swift` |
| Heat ladder, CPU fallback, cooling and pause | Built | `HeatGovernor.swift`, `ModelLogic.swift` |
| Notes finish by themselves: continued task, overnight, notifications 1/2/3 | Built | `ContinuedWork`, `OvernightWork`, `NotesNotifier` |
| Finish now screen and estimate | Built | `FinishNowView.swift` |
| Model auto-download, device gate | Built | `ModelManager.swift` |
| Suggestions with Yes / Edit / No, reminders, tasks | Built | `sve_suggest`, `ReminderScheduler`, `Today/` |
| Medicine checks, health gate | Built | `sve_medcheck`, `sve_health` |
| Learning from corrections | Built | `sve_learn`, `LearnedView` |
| Memories, people, promises, review cards | Built | `sve_memory`, `sve_consolidate`, `Views/Memory/` |
| Deep dive by topic and person, timeline | Built | `Views/DeepDive/` |
| Ask as chat, text and voice, with sources | Built | `sve_chat`, `AskView`, `ChatStore` |
| Knowledge graph (map) with waiting points | Built, first version | `sve_brainmap`, `Views/Map/` |
| Look, guided setup, voice wording | Built | `Views/Setup/`, voice guide, `Brand.swift` |
| Meaning search (embeddings) in Ask | Core built, iOS not wired | no `sve_load_embedder` call in `ios/`, no embedder in `ModelCatalog` |
| 3D animated knowledge graph | Planned | owner request, second-brain Task 17 |
| Whisper Telugu transcripts | Built (Tasks 1-5), inactive until the model file is on the phone; device checks (Task 6) pending | commits `569508a`..`d115f17`; section 6 |
| Piece speed work | Notes only | `docs/superpowers/2026-11-09-piece-speed.md` |
| Import audio files and any-mic live recording | Planned (spec approved 2026-11-10) | `specs/2026-11-10-import-and-any-mic-design.md`; no importer in `ios/` |
| Opt-in cloud connectors, richer onboarding | Planned | owner memory only; on-device stays default |
| Android app, hardware device (RK3576) | Planned | `core/README.md`; same C API, whisper/IndicConformer backend |
| Slim mmproj hosted by owner | Planned | `ModelHost` comment in `ModelManager.swift` |

Known stale docs: the root `README.md` and `core/README.md` still describe Apple Speech and Qwen3 as the iPhone path. The code uses Gemma 4 E4B for audio and text. Fix those before sharing the repo.

## 9. How we got here (history, 258 commits, 2026-11-06 to 2026-11-12)

Each feature went the same way: design in `docs/superpowers/specs/`, owner approval, plan in `docs/superpowers/plans/`, build task by task with a review after each task and a whole-branch review at the end, then checks on the owner's iPhone 16e. All work is on `feature/personal-assistant`, not pushed.

**2026-11-06, personal assistant (61 commits).** Spec `2026-11-06-personal-assistant-design.md`. Core: assistant tables, extraction prompt with a tolerant JSON parser, the code-enforced medicine check (fractions, ranges, windowed name match, ambiguous doses; five review rounds), learning from repeated edits, the memory context assembler, the assistant C API. iOS: Today screen and suggestion cards with Yes / Edit / No, local reminders (once, daily, weekly), Deep dive (topics, people, timeline), Ask with sources and voice questions, profile and learned memory, Delete everything, the English + Telugu string catalog.

**2026-11-07, model download and live transcription (14).** The app downloads Gemma by itself; Qwen and Apple Speech removed (Apple has no on-device Telugu). Pure rules for download, memory and heat. Live transcription while recording (`sve` incremental pieces from a WAV still being written), resume after locking, the Stop hand-over.

**2026-11-08, long recordings and the second brain (34).** Recordings planned into ~5 minute sections with section summaries; notes, titles and chapters built from them. Overnight finishing while charging, recording compression, a slimmer projector tool. Regenerate / Undo / Try again. Second-brain design: Telugu-aware text matching, memory tables, grounding check (names must be said, real quotes), health flag and importance, per-piece transcript confidence.

**2026-11-09, finishing without the app open (63).** Memories written after the notes, in the background and overnight; memory review cards; the memory accuracy test set. Recording Live Activity; start/stop from Shortcuts, Siri, the Action button and Control Center; auto-stop after quiet. Separate store connection so screens never wait behind the model. Background tasks with progress every 10 s; notes say per stage when they are ready. The heat ladder (ease off with rests and fewer threads instead of stop-start). Faster pieces (prefill and audio encoder on up to 8 threads). Notes accuracy fixes from the owner's audit of note 19 (vocabulary as spelling only, `[unclear]` pieces). Ask becomes a streaming chat that remembers the conversation, answers in the language asked, routes by kind of question, and never sends a health question to the model. The voice guide: no AI wording.

**2026-11-10, look, setup, engine v2 (33).** New look (light grey and black, the owner's logo, Appearance setting) and guided first-run setup. "1 of 3 ready" notices. Background CPU thread caps; asking for background time at Record. Pure map builder; same-person questions and merges; memory consolidation across recordings. Notes and questions written one part at a time. Meaning-search vectors fused with keyword search, EmbeddingGemma support in the core. Engine v2 design and plan; import/any-mic design (draft).

**2026-11-11, engine v2 and the map (39).** Notes written in small saved steps so a cut-off costs one step; failures counted only when real. Silent notes notices at night. The 3D knowledge-graph map with actions on every memory, waiting points marked "?", health memories and conflicts kept off it. A part's notes and questions written together. Finish now (dim screen, minutes, an estimate that counts the transcript). Notes written while recording. Live transcription paced by heat, then turned off by default (`83a7603`) because it heated the phone to thermal state 2 in about 10 minutes.

**2026-11-11/12, Whisper spike.** The owner reported transcription was still too slow. Stock large-v3-turbo failed on Telugu. The Telugu fine-tune worked; fixes found by testing on the phone: -O3 for whisper.cpp, pauses instead of fixed cuts, `no_timestamps` for the broken first letters, `audio_ctx` for speed, a repeat filter for ticking loops, and a language check because English came out in Telugu letters.

**2026-11-12, Whisper build (14).** Design approved with the owner's answers (host the model on the owner's Hugging Face with credit to vasista22; unsure pieces go to Gemma, no new control). Plan of 6 tasks; Tasks 1-5 built and reviewed: pure helpers (`569508a`, `f5c239c`), whisper.cpp and the C calls (`2bed677`), pending sidecar pieces (`b167f64`, `67b3ecb`), the pipeline (`fa9efb9`, `b847eee`), the download (`9a1bdc8`), final-review fixes (`377ae0e`, `d115f17`). This architecture file (`87c8105`).

## 10. Open items

- **Whisper device checks (Task 6):** copy the model to the phone, run notes 22 and 25 through the DEBUG hook (language detection 95%+ right), a 30-minute class within 14 min hot / 12 min warm, mixed speech, ticking noise, model missing, memory peak, CPU speed while locked; then tune `lang_choice` and the `[unclear]` threshold.
- **Owner:** upload `ggml-te-medium-q8_0.bin` with `tools/model/whisper-te-README.md` to Hugging Face and give the repo so `WhisperHost.url` can be set.
- **Known minor Whisper gaps** (accepted for now): a quick background blip reloads Whisper; one Gemma GPU piece may run after a long rest; overlapping verifies can hash twice; a bad hosted file is retried 3 times; the Linux CMake build with vendored whisper is untested.
- **Ask is keyword-only on the phone:** `sve_load_embedder` is never called from iOS. The owner rates retrieval for Ask as top priority.
- **Stale docs:** root `README.md` and `core/README.md` still describe Apple Speech and Qwen3.
- **Planned:** import audio files and any-mic recording; richer 3D map styling; opt-in cloud connectors; Android and the pocket device on the same C API.

