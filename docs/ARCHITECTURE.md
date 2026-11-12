# Summaverick Everyday: architecture

Written 2026-11-12 from the code and the specs in `docs/superpowers/`. Where a spec and the code disagree, this file follows the code. The plan checkboxes in `docs/superpowers/plans/` are all unticked, so they say nothing about status. Use the table in section 8.

## 1. What the app is, and its hard rules

An iPhone app that records a lecture, meeting or doctor's visit (Telugu, English or a mix) and writes a short summary, detailed notes and revision questions. It also keeps a "second brain": things said, people, promises, reminders. Everything runs on the phone. No account, no server.

Hard rules. Each one is enforced in code, not just policy.

| Rule | Where it holds |
|---|---|
| On-device only. The one network use is the model download. | `ios/SummaverickEveryday/Engine/ModelManager.swift` (background URLSession) |
| No AI wording in the UI. A person advising, not a machine. | `docs/superpowers/2026-11-09-voice-guide.md`; strings in `Localizable.xcstrings`; `ios/tools/check_strings.py` checks every string has a Telugu value |
| Medicine and health answers are built in code. The model may not write them. | `core/src/sve_medcheck.cpp`, `sve_health.cpp`, `sve_chat.cpp` (medicine sentences are held back from model text and quoted from the recording) |
| Nothing is saved or rings until the person taps Yes. | `suggestions` table, `sve_suggestion_accept/reject`, `Views/Today/SuggestionCard.swift` |
| One model in memory at a time. | `CoreEngine.swift` loads/unloads Gemma (`sve_load_llm`, `sve_unload_llm`). The embedder is the one planned CPU-only exception (section 5). |
| No model on the Mac for testing. | Core tests use only `clang++` and SQLite, and the model-free API check. Model runs happen on the phone. |
| Plain voice for medicine, privacy, consent, "Delete everything". | Voice guide, "Where the voice stays plain" |

Other privacy rules: audio is deleted after a retention window (7 days default), notes stay until the person deletes them, model files are excluded from iCloud backup, and the app is behind Face ID / passcode (`Security/AppLock.swift`).

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
3. **Sidecar.** Progress sits next to the audio in `<audio>.live.json` (`Engine/TranscriptSidecar.swift`): `cursor_sample`, finished `pieces` (`start_sec`, `text`), finished `sections`, and `parts`. It is written atomically after every piece, section or part. Any interruption (crash, kill, heat pause) resumes from the cursor. The sidecar is deleted with the audio after the transcript is saved to the note.
4. **Transcript pieces.** `TranscriptRun` (in `LiveTranscriber.swift`) cuts the audio into pieces of about 28 s and sends each one to Gemma 4 E4B through its audio projector (`sve_load_audio`, `sve_set_audio_threads`). Gemma listens to the audio itself, so Telugu, English and mixed speech come out as text. Transcribing while still recording is off by default (commit `83a7603`); every piece is written after Stop unless live transcription is turned on in settings (`LiveTranscribe` in `ModelLogic.swift`).
5. **Sections.** The pieces are grouped into sections (`core/src/sve_sections.cpp`, `sve_section_summarize`). Each section gets a title, a topic, points with `start_sec`, terms and hints (`SectionSummary` in the sidecar file). Very long recordings become many sections.
6. **Outputs.** `sve_assemble` and `sve_section_output_put` write the notes (kind 1) and revision questions (kind 2) part by part, then the short summary, in small saved steps (`step_attempts` records tries). `Views/ChaptersView.swift` and `NoteDetailView.swift` show them. A part that already exists for the same language and piece range is not redone.
7. **Extraction.** `sve_extract` reads the transcript for reminders, tasks, medicines and facts. They become `suggestions`, shown as cards on Today. `sve_medcheck` compares medicines with the doctor's exact words and the person's profile.
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
| Whisper te-medium | Telugu transcript pieces | none merged | Design and plan only, see below |
| Qwen3 0.6B / 1.7B | Old summariser | retired (`ModelCatalog.retiredFilePrefixes` deletes old files) | Gone from the app. Still referenced in the root README and `core/README.md` (stale) |

- **Download.** `ModelManager` downloads by itself after install, in a background URLSession (`com.summaverick.everyday.model`), resumes after failure, checks size and free space, and posts `.aiReady`. The person never picks a model. `DeviceGate` refuses phones without enough memory.
- **Memory.** The app needs the Increased Memory Limit entitlement (`SummaverickEveryday.entitlements`) so the 5 GB model fits. It is the only extra entitlement, because personal-team signing allows nothing else.
- **Contexts.** `ModelSpec.contextTokens` is 8192 for Gemma. `CoreEngine.loadModel` defaults to 4096 when not told. Long transcripts are cut by section and by part so no single call needs more than the context. `sve_transcript_fits` tells whether a text fits.
- **One model at a time.** Gemma is unloaded and reloaded when the backend changes (CPU to GPU) via `lastLoad` in `CoreEngine`.

### Whisper Telugu plan (in progress, nothing merged)

`docs/superpowers/specs/2026-11-12-whisper-telugu-transcripts-design.md` and `docs/superpowers/plans/2026-11-12-whisper-telugu-transcripts.md`. Reason: Gemma transcription takes about as long as the audio or longer on the iPhone 16e and sometimes loops. A spike (branch `spike/whisper-telugu`, not merged) showed Whisper te-medium doing 10 minutes of audio in 3.5 to 6.2 minutes with cleaner Telugu. Problems found: English comes out in Telugu letters, and Whisper loops on ticking noise. The design therefore uses Whisper for Telugu pieces, and Gemma when the piece is English or Whisper is unsure. The spec status is "owner's questions answered; awaiting approval". The plan has 5 tasks, starting with pure helpers in the core. Today `SVE_WITH_WHISPER` is only used in the CMake / device build, not in the iOS target.

## 7. Build and test

**Generate the Xcode project.** `cd ios && python3 make_xcodeproj.py`. It writes `SummaverickEveryday.xcodeproj` with no XcodeGen. Object IDs come from file paths, so output is stable. It builds two targets: the app and `SummaverickWidgets` (bundle id `com.summaverick.everyday.widgets`, embedded). `Shared/*.swift` compiles into both. `core/src` is added to the app target. Re-run it when Swift files are added or renamed. `project.yml` is an unused XcodeGen alternative.

**llama.** `cd ios && ./get-llama-xcframework.sh` downloads the prebuilt `llama.xcframework`; the version is pinned in `ios/Frameworks/llama.version`.

**Device build.** Open the project in Xcode 16+, set the Team under Signing & Capabilities, pick a connected iPhone (iOS 17+), Run. Do not add capabilities; a personal team will fail signing. Background continued processing needs iOS 26.

**Core tests (no phone, no model).**
- `bash core/tests/run.sh` compiles each `core/tests/test_*.cpp` with the pure core sources using `clang++ -std=c++17 -DSVE_TEST_MAIN` and `-lsqlite3`, runs them and prints a TOTAL line. Covers store, extraction, medicine checks, learning, sections, memory, brain map, consolidation, chat, assemble, embed, budget, threads.
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
| Whisper Telugu transcripts | In progress: design awaiting approval, plan written, nothing merged | spec and plan dated 2026-11-12; spike branch unmerged |
| Piece speed work | Notes only | `docs/superpowers/2026-11-09-piece-speed.md` |
| Import audio files and any-mic live recording | Planned (spec approved 2026-11-10) | `specs/2026-11-10-import-and-any-mic-design.md`; no importer in `ios/` |
| Opt-in cloud connectors, richer onboarding | Planned | owner memory only; on-device stays default |
| Android app, hardware device (RK3576) | Planned | `core/README.md`; same C API, whisper/IndicConformer backend |
| Slim mmproj hosted by owner | Planned | `ModelHost` comment in `ModelManager.swift` |

Known stale docs: the root `README.md` and `core/README.md` still describe Apple Speech and Qwen3 as the iPhone path. The code uses Gemma 4 E4B for audio and text. Fix those before sharing the repo.
