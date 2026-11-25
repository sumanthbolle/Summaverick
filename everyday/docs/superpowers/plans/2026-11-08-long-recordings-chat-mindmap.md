# Long Recordings, Slimmer Model, Mini ChatGPT and Mind Map — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Long recordings get notes, chapters and a mind map with little work after Stop; the model download shrinks; the Ask tab becomes a streaming multi-turn chat; all on the phone with Gemma 4 E4B.

**Architecture:** Pure logic (section planning, chapter merging, JSON validation, chat prompt trimming, health guard, storage) lives in new `core/src` files covered by `core/tests/run.sh`. Model-calling glue lives in `sve_core.cpp` / `sve_assistant.cpp` behind new C API functions, exercised by `core/tests/smoke_gemma.sh` modes. Swift orchestrates (pipeline, live transcriber, background tasks, UI). Section summaries are written while transcribing and kept in the transcript sidecar until the note exists, then in the database.

**Tech Stack:** C++17, llama.cpp b12119 + mtmd, SQLite FTS5, nlohmann/json; Swift/SwiftUI (iOS 17 target), AVFoundation, BackgroundTasks; Python 3 + `gguf` package for model tools.

**Spec:** `docs/superpowers/specs/2026-11-08-long-recordings-chat-mindmap-design.md`

## Global Constraints

- On-device only. No network use except the model download. No other models.
- Deployment target iOS 17; iOS 26+ APIs behind `#available`. Device: iPhone 16e `00008140-001668892E22801C`.
- New data and AI logic in `core/` behind `core/include/sve_core.h`; Swift only orchestrates and draws.
- Context 8,192 tokens (`ModelSpec.contextTokens`). Model work in the background runs on the CPU (4 threads); GPU only when the app is active and not recording. Pick the backend right before each model run.
- Heat rule for all long work: `Heat.beforeRun` — `.serious` rests 30 s then runs, `.critical` pauses; only `.critical` cancels mid-run.
- Medicine rules unchanged: medicine answers built from stored facts (`medicine_route`), never generated; quote checks stay.
- Every new user-visible string in `ios/SummaverickEveryday/Localizable.xcstrings` with English and Telugu, file format `json.dumps(d, ensure_ascii=False, indent=2)+"\n"`, no key sorting; `python3 ios/tools/check_strings.py <derivedData>` prints `STRINGS OK`.
- Verify per task: `bash core/tests/run.sh` all pass; device build `cd ios && xcodebuild -project SummaverickEveryday.xcodeproj -scheme SummaverickEveryday -destination 'id=00008140-001668892E22801C' -derivedDataPath <scratch>/dd-dev -allowProvisioningUpdates build` succeeds with no new warnings. New Swift files need `cd ios && python3 make_xcodeproj.py`.
- Commits end with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`. Branch `feature/personal-assistant`.
- Section length: target 300 s of audio, between 240 and 360 s, cut only at piece boundaries.

## Review Focus

1. Gemma returns broken or truncated JSON for a section summary or mind map — retry once with greedy decoding, then fall back (section: plain-text summary from the first sentences; map: list view). Tests in Tasks 2 and 12.
2. A Telugu or mixed-language medicine question in chat ("ఈ tablet ఎన్ని సార్లు వేసుకోవాలి", "paracetamol తో ibuprofen తీసుకోవచ్చా") — never a general-knowledge answer. Tests in Task 9.
3. A note deleted, or regenerated, while its sections are being written or the overnight task holds it — no orphan sections, no crash, no resurrected note. Tests in Tasks 1 and 5.
4. A chat longer than the context window — oldest turns collapse to a one-line summary; system prompt, profile and the newest turn always fit. Tests in Task 9.
5. The app killed in the middle of AAC conversion — the WAV is kept, the half-written `.m4a` is removed at next launch, conversion retried. Tests in Task 6.

---

## Phase 1 — Long recordings and a slimmer model

### Task 1: Section planning and storage (core, pure)

**Files:**
- Create: `core/src/sve_sections.h`, `core/src/sve_sections.cpp`
- Modify: `core/src/sve_store.cpp` (schema + accessors), `core/src/sve_store.h`, `core/tests/run.sh` (add `sve_sections` to `SRCS`), `core/include/sve_core.h`, `core/src/sve_core.cpp`
- Test: `core/tests/test_sections.cpp`

**Interfaces:**
- Produces:
  - `struct sve::PieceRef { int64_t start_sec; int64_t end_sec; };` (`end_sec` = next piece's start, or note duration for the last)
  - `std::vector<std::pair<size_t,size_t>> sve::plan_sections(const std::vector<PieceRef>&, bool is_final, int target=300, int min_len=240, int max_len=360)` — half-open piece index ranges; when `!is_final`, a trailing range shorter than `min_len` is not returned.
  - Table `sections(note_id INTEGER, idx INTEGER, first_piece INTEGER, last_piece INTEGER, start_sec INTEGER, end_sec INTEGER, summary_json TEXT, PRIMARY KEY(note_id, idx))`, deleted with its note (same place `transcript_pieces` rows are deleted), also cleared by `sve_note_clear_transcript`.
  - Section summary JSON shape (used by every later task): `{"title":"…","topic":"…","points":[{"text":"…","start_sec":N}],"terms":["…"],"hints":["…"]}`
  - C API: `int sve_note_set_sections(sve_ctx*, int64_t note_id, const char *sections_json)` (array of `{"idx","first_piece","last_piece","start_sec","end_sec","summary":{…}}`, replaces all), `char *sve_note_sections(sve_ctx*, int64_t note_id)` (same array, `[]` when none).

- [ ] **Step 1: Write failing tests** in `core/tests/test_sections.cpp`: `plan_sections_cuts_near_300s` (30 pieces of 28 s, final → ranges whose durations are all within 240–360 except possibly the last, covering every piece exactly once); `plan_sections_holds_short_tail_when_not_final` (10 pieces of 28 s, not final → 0 ranges; final → 1 range of all 10); `plan_sections_single_long_piece` (one piece of 400 s → one range); `sections_roundtrip_and_cascade` (set 2 sections on a note, read back equal; delete note → `sve_note_sections` returns `[]`; clear transcript → `[]`).
- [ ] **Step 2: Run** `bash core/tests/run.sh` — expect BUILD FAILED or failures for `test_sections`.
- [ ] **Step 3: Implement** planner (greedy: extend a range while adding the next piece keeps it ≤ `max_len`; close it at the first boundary ≥ `target`, or ≥ `min_len` if the next piece would pass `max_len`), schema, store accessors and the two C functions.
- [ ] **Step 4: Run** `bash core/tests/run.sh` — expect `TOTAL: … 0 failed`.
- [ ] **Step 5: Commit** `feat(core): plan recordings into ~5 minute sections and store them`.

### Task 2: Section summaries from Gemma (core)

**Files:**
- Modify: `core/src/sve_sections.cpp/.h` (prompt builder, JSON parse/repair), `core/src/sve_core.cpp` (LLM call), `core/include/sve_core.h`, `core/src/sve_prompts.h`
- Test: `core/tests/test_sections.cpp`, `core/tests/smoke_gemma.sh` + `smoke_gemma.cpp` (new mode `sections`)

**Interfaces:**
- Consumes: Task 1 summary JSON shape.
- Produces:
  - `std::string sve::section_prompt(const std::vector<std::pair<int64_t,std::string>> &pieces, const std::string &lang, const std::string &vocab_line)` — instruction to write the shape above in `lang` (`"en"`/`"te"`), names and technical terms kept in the script spoken, each point's `start_sec` copied from the piece it came from.
  - `bool sve::parse_section_summary(const std::string &model_out, const std::vector<int64_t> &piece_starts, json &out)` — strips code fences/thinking, parses the first JSON object, clamps each `start_sec` to the nearest piece start, caps points at 6, false when no title or no points.
  - `json sve::fallback_section_summary(const std::vector<std::pair<int64_t,std::string>> &pieces)` — title = first 8 words, points = first sentence of up to 3 pieces with their starts, `"fallback":true`.
  - C API: `char *sve_section_summarize(sve_ctx*, const char *pieces_json, const char *lang, sve_progress_cb, void*)` — `pieces_json` = `[{"start_sec","text"}]`; returns the summary JSON (never NULL on a model that ran: falls back after one greedy retry); NULL on cancel/no model. Output tokens capped at 512.

- [ ] **Step 1: Write failing tests**: `parse_section_summary_accepts_fenced_json`, `parse_section_summary_clamps_start_secs` (start 95 with pieces [0,28,56,84,112] → 84), `parse_section_summary_rejects_truncated` (cut mid-string → false), `parse_section_summary_caps_points_at_6`, `fallback_summary_shape`, `section_prompt_mentions_language_and_starts` (contains `"te"` instruction text and each start second).
- [ ] **Step 2: Run** `bash core/tests/run.sh` — new tests fail.
- [ ] **Step 3: Implement** the three pure functions and `sve_section_summarize` (sampled first try, greedy retry, then fallback; reuse `transcription_vocab_line`).
- [ ] **Step 4: Run** `bash core/tests/run.sh` — pass. Add smoke mode `sections <wav>`: transcribe with `sve_transcribe_next_piece`, plan with `plan_sections(final)`, summarise each, print JSON and seconds per section. Run it on the 2-minute Telugu fixture the smoke script already uses; paste output in the report.
- [ ] **Step 5: Commit** `feat(core): section summaries from Gemma with JSON repair and fallback`.

### Task 3: Notes, title and chapters from sections (core)

**Files:**
- Modify: `core/src/sve_sections.cpp/.h`, `core/src/sve_core.cpp`, `core/include/sve_core.h`
- Test: `core/tests/test_sections.cpp`, smoke mode `longnotes`

**Interfaces:**
- Consumes: Task 1 storage, Task 2 shape.
- Produces:
  - `json sve::merge_chapters(const json &sections)` — consecutive sections whose normalised `topic` (lower-case, trimmed, Unicode-aware) is equal merge; output `[{"title","start_sec"}]`, title from the first section of the run.
  - `std::vector<std::string> sve::section_groups(const json &sections, int budget_tokens, const std::function<int(const std::string&)> &count_tokens)` — renders section summaries as text blocks and groups consecutive blocks to fit `budget_tokens`.
  - C API: `char *sve_generate_from_sections(sve_ctx*, int64_t note_id, sve_output_kind kind, const char *lang, sve_progress_cb, void*)` — one group: one call; several: summarise each group, then the combined group summaries (hierarchical). `char *sve_title_from_sections(sve_ctx*, int64_t note_id, const char *lang)`. `char *sve_chapters_json(sve_ctx*, int64_t note_id)` (pure, no model).
  - Behaviour: when a note has no sections, `sve_generate_from_sections` returns NULL and callers use the existing `sve_generate` (old notes until backfilled).

- [ ] **Step 1: Write failing tests**: `merge_chapters_joins_same_topic` (topics A, a , B, B, A → 3 chapters at the right starts), `merge_chapters_telugu_topics` (Telugu topic equal after trim → merged), `section_groups_respects_budget` (fake counter = bytes/4; 36 sections; every group ≤ budget; order kept; all sections present).
- [ ] **Step 2: Run** — fail.
- [ ] **Step 3: Implement** functions and C API (budget = `n_ctx − max_out − prompt overhead`, as `reduce_to_fit` computes).
- [ ] **Step 4: Run** tests — pass. Smoke mode `longnotes <wav>`: builds a ~2-hour input by repeating the fixture's pieces with shifted starts (reuse Task 2 summaries per repeat to keep it fast; label output "synthetic"), times `sve_generate_from_sections` for all three kinds + title; paste timings.
- [ ] **Step 5: Commit** `feat(core): notes, titles and chapters built from section summaries`.

### Task 4: Sections in the app (pipeline, live, chapters UI)

**Files:**
- Create: `ios/SummaverickEveryday/Engine/SectionWriter.swift`, `ios/SummaverickEveryday/Views/ChaptersView.swift`
- Modify: `Engine/TranscriptSidecar.swift` (add `sections`), `Engine/LiveTranscriber.swift`, `Engine/ProcessingPipeline.swift`, `Engine/CoreEngine+Assistant.swift` (wrappers), `Views/NoteDetailView.swift`, `Localizable.xcstrings`

**Interfaces:**
- Consumes: Tasks 1–3 C API.
- Produces:
  - `struct SectionSummary: Codable` mirroring the JSON shape; `struct NoteSection: Codable { idx, firstPiece, lastPiece, startSec, endSec, summary }`.
  - Sidecar gains `sections: [NoteSection]` (decodes old sidecars without it).
  - `SectionWriter.writeDue(pieces:[TranscriptPiece], existing:[NoteSection], isFinal:Bool, lang:String) async throws -> [NoteSection]` — plans with the core planner (exposed as `sve_plan_sections(const char *pieces_json, int is_final)` returning `[[first,last)]` JSON; add it in this task), summarises only new ranges, one at a time, applying the heat rule before each.
  - CoreEngine: `sectionSummarize(pieces:lang:shouldStop:)`, `setSections(_:_:)`, `sections(_:)`, `generateFromSections(noteID:kind:lang:shouldStop:)`, `titleFromSections(noteID:lang:)`, `chapters(_:) -> [Chapter]` with `struct Chapter: Decodable { title: String; start_sec: Int }`.
- Rules:
  - Live: after each piece, if fewer than 10 pieces (≈1 section) are waiting to be transcribed, write due sections into the sidecar; otherwise skip until caught up.
  - Pipeline: after transcription completes, write remaining sections (`isFinal = true`), store them (`setSections`), then outputs via `generateFromSections` (fallback `sve_generate`), title via `titleFromSections`. Extraction unchanged.
  - Notes without sections (made before this change): sections are written the first time the note is opened or regenerated (backfill), using stored pieces.
  - Note screen: a chapters strip at the top ("0:00 Introduction") — tap plays from `start_sec` with the existing player. Hidden when fewer than 2 chapters.
  - New strings: "Chapters" (en/te).

- [ ] **Step 1:** Add the planner C function + a core test `plan_sections_json_api` (pieces JSON in, ranges JSON out), run `bash core/tests/run.sh` — pass.
- [ ] **Step 2:** Implement Swift parts; add a standalone `swiftc` check (`<scratch>/sections_check.swift` compiling `TranscriptSidecar.swift` + a test main) for: old sidecar without `sections` decodes; sections survive a save/load round trip; "behind by ≥10 pieces → skip" rule. Run it; paste output.
- [ ] **Step 3:** Device build + `STRINGS OK`.
- [ ] **Step 4: Commit** `feat(ios): write section summaries while transcribing; chapters on the note`.

### Task 5: Overnight mode

**Files:**
- Create: `ios/SummaverickEveryday/Engine/OvernightWork.swift`
- Modify: `Info.plist` (`UIBackgroundModes` add `processing`; `BGTaskSchedulerPermittedIdentifiers` add `com.summaverick.everyday.overnight`), `App.swift` (register at launch), `Engine/ProcessingPipeline.swift`, `Engine/ModelLogic.swift`, `Views/NoteDetailView.swift`, `Views/DeepDive/TimelineView.swift` (`StatusBanner`), `Localizable.xcstrings`

**Interfaces:**
- Produces:
  - `enum WorkEstimate { static func minutesLeft(audioSecondsLeft: Int, sectionsLeft: Int, outputsLeft: Int) -> Int }` — 1.2 s of work per audio second left + 60 s per section + 90 s per output (from device measurements; constants in one place).
  - `OvernightWork.shared.schedule()` submits `BGProcessingTaskRequest(identifier: "com.summaverick.everyday.overnight")` with `requiresExternalPower = true`, `requiresNetworkConnectivity = false`; the handler resumes notes marked overnight one at a time on the CPU, sets an expiration handler that aborts and keeps progress, calls `setTaskCompleted` exactly once, and reschedules if work remains.
  - Pipeline: notes marked overnight are stored in `UserDefaults` key `pipeline.overnight` (`[Int64]`); `processWaitingNotes` and resume skip them while the app is not plugged in, unless "Do it now".
- Behaviour:
  - After Stop, when `WorkEstimate.minutesLeft > 20`, the note offers "This will take a while. Finish overnight while charging?" (Yes / Keep going). Yes: stop after the current piece, mark overnight, schedule.
  - Waiting note shows "Waiting for the charger — finishes overnight" and a "Do it now" button (clears the mark, resumes now).
  - When the app comes to the front while plugged in, overnight notes may continue in front as normal work.
  - Deleting a note removes it from the overnight list; the handler skips ids that no longer exist.
  - When the app goes to the background with any note still having work left (overnight-marked or not), `schedule()` is called so unfinished notes can also finish while charging.
  - Strings (en/te): the three lines above, "Do it now", "Keep going".

- [ ] **Step 1:** Add `WorkEstimate` to `ModelLogic.swift` and a `swiftc` check: 60 min audio left + 12 sections + 3 outputs → 1440+720+270 s = 41 min → offer; 10 min left → no offer; overnight list ignores deleted ids. Run; paste output.
- [ ] **Step 2:** Implement `OvernightWork`, pipeline marks, UI.
- [ ] **Step 3:** Device build, `STRINGS OK`, Info.plist keys present in the built app (`plutil -p`).
- [ ] **Step 4: Commit** `feat(ios): finish long recordings overnight while charging`.

### Task 6: Compress audio after notes

**Files:**
- Create: `ios/SummaverickEveryday/Audio/AudioCompressor.swift`
- Modify: `Engine/ProcessingPipeline.swift` (after outputs are done), `App.swift` (launch sweep), `Audio/AudioRecorder.swift` (`existingAudioPath` must also resolve `.m4a`), `core` only if `audio_path` update needs an API (`sve_note_set_audio_path(sve_ctx*, int64_t, const char*)`, add it with a core test if missing)

**Interfaces:**
- Produces: `AudioCompressor.compress(wav: URL) async throws -> URL` — writes `<name>.m4a.part` with `AVAudioFile` reading the WAV and `AVAudioFile` writing AAC (`kAudioFormatMPEG4AAC`, mono, 16 kHz, 32 kbit/s), renames to `<name>.m4a` when done; `AudioCompressor.verify(m4a: URL, expectedSeconds: Double) -> Bool` (opens; |duration − expected| ≤ 1.0 s).
- Order: compress → verify → update note `audio_path` → delete WAV and any sidecar. Any failure: delete `.part`/`.m4a`, keep WAV, retry on the next launch sweep.
- Only when the note has a transcript and all three outputs. Never during a recording or while the note is being processed.
- Launch sweep: delete stray `*.m4a.part`; queue notes that are done but still WAV.

- [ ] **Step 1:** `swiftc` check with a generated 10 s sine WAV: compress, verify true, size < 1/5 of the WAV; a `.part` left behind is removed by the sweep function; verify false when expected seconds is off by 5. Paste output.
- [ ] **Step 2:** Implement and wire.
- [ ] **Step 3:** Device build. Playback of `.m4a` in `NoteDetailView` (AVAudioPlayer) checked in the device checklist.
- [ ] **Step 4: Commit** `feat(ios): compress recordings once their notes are done`.

### Task 7: Projector without the image encoder

**Files:**
- Create: `tools/model/strip_vision.py`, `tools/model/README.md` (upload steps for the owner's Hugging Face repo)
- Modify: `ios/SummaverickEveryday/Engine/ModelManager.swift` (projector URL and size move into a named constant `ModelHost.projectorURL` / `ModelHost.projectorBytes`, defaulting to the current file; the README says how to switch them after the owner's upload)

**Interfaces:**
- Produces: `python3 -I tools/model/strip_vision.py <in.gguf> <out.gguf>` — copies all metadata and every tensor whose name does not start with `v.`; keeps `mm.` tensors only if the audio path uses them (check names; document the rule). Prints sizes before/after.
- Gate: smoke `transcribe` on the fixture with the original and the stripped projector gives byte-identical text (greedy). Also the `live` mode.

- [ ] **Step 1:** Write the script (use the `gguf` package, `GGUFReader` + `GGUFWriter`); run on `~/Library/Caches/llama.cpp/mmproj-gemma-4-E4B-it-Q8_0.gguf`; expected out ≈ 0.33 GB.
- [ ] **Step 2:** Run both smoke comparisons; save diffs; paste "identical" lines.
- [ ] **Step 3:** README: create repo, upload with `huggingface-cli upload`, then change `ModelHost.projectorURL` and size. App keeps the current URL until then.
- [ ] **Step 4: Commit** `feat(tools): projector without the image encoder, with an identical-output check`.

### Task 8: Language-trimmed vocabulary (experiment, gated)

**Files:**
- Create: `tools/model/trim_vocab.py`, `tools/model/compare_outputs.sh`, `tools/model/testset/README.md`
- No app change in this task.

**Interfaces:**
- Produces: `trim_vocab.py <in.gguf> <out.gguf> --keep-scripts latin,telugu,devanagari --keep-observed <tokens.txt>` — keeps special/template tokens, byte-fallback tokens, tokens fully in kept scripts plus digits/punctuation/whitespace, and every token id listed in `tokens.txt` (ids the full model emitted on the test set); rewrites tokenizer metadata and slices `token_embd` and `per_layer_token_embd` consistently; prints kept count and sizes.
- `compare_outputs.sh` runs the full and trimmed weights over the test set (greedy) — transcriptions of ≥20 clips (owner's recordings copied from the phone with `devicectl … copy from` into `tools/model/testset/` — git-ignored, plus the Telugu reference clip), notes, extraction, a chat prompt, a long-notes prompt — and prints `IDENTICAL` or the first difference per item, then `GATE: PASS|FAIL`.
- On FAIL: write the result in the report and stop; do not iterate on the model. On PASS: README gains the upload steps; app change waits for the owner's upload (same as Task 7).

- [ ] **Step 1:** Build the test set and collect emitted token ids with the full model.
- [ ] **Step 2:** Write and run `trim_vocab.py`.
- [ ] **Step 3:** Run `compare_outputs.sh`; paste the summary.
- [ ] **Step 4: Commit** `feat(tools): language-trimmed vocabulary experiment and its gate` (scripts and README only; no model files, no recordings in git).

## Phase 2 — Mini ChatGPT and mind map

### Task 9: Conversations, prompt trimming and health guard (core, pure)

**Files:**
- Create: `core/src/sve_chat.h`, `core/src/sve_chat.cpp`, `core/tests/test_chat.cpp`
- Modify: `core/src/sve_store.cpp/.h` (tables), `core/tests/run.sh` (`sve_chat` in `SRCS`), `core/include/sve_core.h`, `core/src/sve_core.cpp` / `sve_assistant.cpp` (C wrappers), `sve_assistant_wipe` (also clears chats)

**Interfaces:**
- Produces:
  - Tables `conversations(id INTEGER PRIMARY KEY, title TEXT, created_at INTEGER, updated_at INTEGER)`, `messages(id INTEGER PRIMARY KEY, conversation_id INTEGER, role TEXT CHECK(role IN ('user','assistant')), text TEXT, kind TEXT, sources_json TEXT, created_at INTEGER)`; messages deleted with their conversation.
  - C API: `int64_t sve_chat_create(sve_ctx*, const char *title)`, `char *sve_chats_json(sve_ctx*)` (newest first: id, title, updated_at), `char *sve_chat_messages(sve_ctx*, int64_t id)`, `int sve_chat_delete(sve_ctx*, int64_t id)`.
  - `enum class sve::HealthFlag { none, health }`; `HealthFlag sve::health_keywords(const std::string &q, const std::vector<std::string> &stored_medicine_names)` — flags stored medicine names, common medicine names list, dose/timing/side-effect/symptom words in English and Telugu (including mixed spellings such as "tablet", "మాత్ర", "డోస్", "ఎన్ని సార్లు", "side effect", "నొప్పి", "జ్వరం").
  - `std::string sve::chat_prompt(const std::string &system, const std::vector<Turn> &turns, const std::string &material, const std::string &question, int budget_tokens, const std::function<int(const std::string&)> &count_tokens)` with `struct Turn { std::string role, text; }` — keeps system, material and question; adds turns newest first while they fit; when any are dropped, adds one line `Earlier in this chat: <first 15 words of each dropped user turn, joined by "; ">` trimmed to fit.

- [ ] **Step 1: Write failing tests**: `chat_store_roundtrip_and_cascade`; `wipe_clears_chats`; `health_keywords_english` ("can I take paracetamol with ibuprofen?" → health; "what is inflation?" → none); `health_keywords_telugu` ("ఈ tablet ఎన్ని సార్లు వేసుకోవాలి" → health; "జ్వరం వస్తే ఏం చేయాలి" → health; "ఈ lecture లో ముఖ్యమైన పాయింట్లు ఏమిటి" → none); `health_keywords_stored_name` (stored "Metformin", question "metformin timing?" → health); `chat_prompt_keeps_newest_turns_within_budget` (40 turns, small budget → newest kept, the "Earlier in this chat:" line present, total ≤ budget, question last).
- [ ] **Step 2: Run** — fail.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** — pass.
- [ ] **Step 5: Commit** `feat(core): chat history, prompt trimming and a health guard`.

### Task 10: Chat answers with streaming (core)

**Files:**
- Modify: `core/src/sve_llm.h/.cpp` (optional text callback in `generate`/`complete`), `core/src/sve_assistant.cpp`, `core/include/sve_core.h`, `core/src/sve_prompts.h`
- Test: `core/tests/test_chat.cpp` (routing with a fake), smoke mode `chat`

**Interfaces:**
- Consumes: Task 9; existing `medicine_route`, retrieval used by `sve_assistant_ask`, `not_found_text`.
- Produces:
  - `typedef int (*sve_text_cb)(const char *new_text, void *user);` — called with each new decoded fragment; non-zero cancels.
  - `char *sve_chat_send(sve_ctx*, int64_t conversation_id, const char *message, sve_text_cb on_text, sve_progress_cb cb, void *user)` — stores the user turn, answers, stores the assistant turn, returns `{"message_id":N,"text":"…","kind":"recordings|general|medicine|not_found","sources":[…]}`.
  - Routing (pure, testable): `std::string sve::chat_route(bool is_medicine_question, HealthFlag kw, bool model_says_health, bool has_relevant_material)` →
    - `"medicine"` if `is_medicine_question` (existing `medicine_route`, not streamed);
    - `"recordings"` if material found (streamed; sources attached; health questions allowed only here);
    - `"not_found"` if `kw == health || model_says_health` and no material (text: not-found line + " Please ask your doctor." in the profile language);
    - `"general"` otherwise (streamed; text prefixed with the label "From Gemma's general knowledge, not your recordings:" in the profile language).
  - `model_says_health`: one short greedy classification call (answer `yes`/`no`, max 3 tokens) run only when keywords did not already flag and no material was found.
  - Recordings answers use pieces and section summaries via FTS (sections indexed in a new `sections_fts`).
  - Prompt rule for recordings answers with a general addition: the recordings part comes first with its sources; a general addition sits under its own label; when general knowledge differs from the recording, the answer says so ("Your recording says X; generally Y"), never silently replacing the recording.

- [ ] **Step 1: Write failing tests** for `chat_route` (all four outcomes; health + material → recordings; health + no material → not_found; neither → general) and `sections_fts` search returning a section by a term in its points.
- [ ] **Step 2: Run** — fail. **Step 3: Implement.** **Step 4: Run** — pass.
- [ ] **Step 5:** Smoke mode `chat`: a 3-turn conversation (a recordings question about the fixture, a follow-up "and what else?", a general question "what is GDP?", a medicine question "how many times should I take dolo?"); print kinds, first 200 chars, time to first text and total time. Paste output.
- [ ] **Step 6: Commit** `feat(core): multi-turn chat with streaming, sources and health routing`.

### Task 11: Chat screen (iOS)

**Files:**
- Create: `ios/SummaverickEveryday/Views/Ask/ChatListView.swift`, `ios/SummaverickEveryday/Views/Ask/ChatView.swift`, `ios/SummaverickEveryday/Engine/ChatModels.swift`
- Modify: `Views/Ask/AskView.swift` (becomes the entry: list + new chat; keep voice input, starter chips and the Yes / Edit / No card actions), `Engine/CoreEngine+Assistant.swift` (`chatSend(conversationID:message:onText:shouldStop:) async throws -> ChatReply`), `Views/SettingsView.swift` (Delete everything already calls wipe — check chats go), `Localizable.xcstrings`

**Interfaces:**
- Consumes: Tasks 9–10 C API.
- Produces: `struct ChatReply: Decodable { message_id: Int64; text: String; kind: String; sources: [AskSource] }` (reuse the existing source type from Ask).
- Behaviour: answers stream into the bubble, UI updated at most every 100 ms; Stop button cancels (partial answer kept and stored as is, marked "(stopped)"); medicine replies appear whole; general replies show their label in a smaller line; sources as tappable chips that open the note at the time; the model runs on the GPU when in front and not recording, CPU otherwise; list of chats with titles from the first question (first 6 words), swipe to delete.
- Strings (en/te): "New chat", "Chats", "Stop", "(stopped)", "From Gemma's general knowledge, not your recordings", "Please ask your doctor."

- [ ] **Step 1:** `swiftc` check for the throttled text accumulator (fragments arriving every 5 ms → ≤ 10 UI updates per second; final text equals the concatenation). Paste output.
- [ ] **Step 2:** Implement views and wiring.
- [ ] **Step 3:** Device build, `STRINGS OK`.
- [ ] **Step 4: Commit** `feat(ios): Ask becomes a chat with history, streaming and sources`.

### Task 12: Mind map data (core)

**Files:**
- Create: `core/src/sve_mindmap.h`, `core/src/sve_mindmap.cpp`, `core/tests/test_mindmap.cpp`
- Modify: `core/src/sve_store.cpp/.h` (`note_maps(note_id INTEGER PRIMARY KEY, map_json TEXT)`, cascade with the note, cleared with transcript), `core/tests/run.sh`, `core/include/sve_core.h`, `core/src/sve_core.cpp`

**Interfaces:**
- Consumes: Task 1 sections.
- Produces:
  - Map JSON: `{"center":"…","topics":[{"title":"…","points":[{"text":"…","start_sec":N}]}]}`, 3–7 topics, ≤6 points each.
  - `bool sve::parse_mindmap(const std::string &model_out, const std::vector<int64_t> &valid_starts, json &out)` — repairs fences, drops empty topics, caps counts, clamps `start_sec` to the nearest valid section-point start; false if fewer than 2 topics.
  - `json sve::mindmap_fallback(const json &sections)` — `{"list":true,"center":<title of first section>,"topics":[one per chapter with its points]}`.
  - C API: `char *sve_mindmap_build(sve_ctx*, int64_t note_id, const char *lang, sve_progress_cb, void*)` (prompt from section summaries, grouped as in Task 3 when too long; one greedy retry; fallback), stores and returns it; `char *sve_mindmap_get(sve_ctx*, int64_t note_id)` (NULL when none).
  - A note with one section and fewer than 4 points total gets the fallback list directly (no model call).

- [ ] **Step 1: Write failing tests**: `parse_mindmap_valid`, `parse_mindmap_caps_and_clamps`, `parse_mindmap_rejects_single_topic`, `mindmap_fallback_from_sections`, `note_maps_cascade_and_clear`.
- [ ] **Step 2: Run** — fail. **Step 3: Implement.** **Step 4: Run** — pass. Smoke mode `mindmap <wav>`: print the map for the fixture.
- [ ] **Step 5: Commit** `feat(core): mind map built from section summaries`.

### Task 13: Mind map screen (iOS)

**Precondition:** the controller shows the owner a mockup of the mind map screen and records the approved layout in the brief for this task before dispatch.

**Files:**
- Create: `ios/SummaverickEveryday/Views/MindMapView.swift`, `ios/SummaverickEveryday/Views/MindMapLayout.swift`
- Modify: `Views/NoteDetailView.swift` (a "Mind map" choice next to Summary / Notes / Questions), `Engine/ProcessingPipeline.swift` (build the map after the outputs; heat rule), `Engine/CoreEngine+Assistant.swift` (`mindMap(_:)`, `buildMindMap(noteID:lang:shouldStop:)`), `Localizable.xcstrings`

**Interfaces:**
- Consumes: Task 12.
- Produces: `MindMapLayout.layout(map: MindMap, size: CGSize, expanded: Set<Int>) -> [PlacedNode]` (pure; centre in the middle, topics evenly around it, points fanned outward from their topic; no two node frames overlap for up to 7 topics × 6 points).
- Behaviour: pinch to zoom (0.5×–3×), drag to pan, tap a topic to fold/unfold (all folded at first if more than 4 topics), tap a point to play from `start_sec`; minimum touch target 44 pt; `"list": true` maps render as a plain outline list. Strings (en/te): "Mind map".

- [ ] **Step 1:** `swiftc` check for `MindMapLayout`: 7 topics × 6 points, all expanded, on 390×700 → no overlapping frames; folded topics place no points. Paste output.
- [ ] **Step 2:** Implement view and wiring (regenerate rebuilds the map).
- [ ] **Step 3:** Device build, `STRINGS OK`.
- [ ] **Step 4: Commit** `feat(ios): mind map for each recording`.

### Task 14: Device checklist and measurements

**Files:**
- Create: `docs/superpowers/2026-11-08-long-recordings-device-checks.md`

- [ ] **Step 1:** Write the owner's checklist from spec §6: 1-hour recording → notes within ~10 min of Stop on a cool phone (record time); chapters jump correctly; overnight offer appears for a long backlog, "Do it now" works, the task finishes while charging; audio plays after compression and the file is ~10× smaller; chat follow-ups keep context, sources open the right moment, Stop works, history persists and deletes, medicine and health questions never get a general answer; mind map readable in Telugu and English, fold/unfold, tap plays.
- [ ] **Step 2:** Include the smoke timings from Tasks 2, 3, 10 and the gate results from Tasks 7–8.
- [ ] **Step 3: Commit** `docs: device checks for long recordings, chat and mind map`.
