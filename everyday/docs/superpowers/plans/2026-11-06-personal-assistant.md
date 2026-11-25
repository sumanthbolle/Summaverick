# Summaverick Personal Assistant Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Turn the Summaverick iPhone app into an on-device personal assistant over the person's recordings: three tabs (Home, Deep dive, Ask Summaverick), suggestions that wait for Yes, a learning profile/memory used as context on every Gemma run, and code-enforced medicine safety.

**Architecture:** All data and AI logic go into the portable C++ core (`core/`) behind the C API in `core/include/sve_core.h`, using JSON strings in and out (like the existing note functions). Pure logic (store, medicine check, extraction parsing, context assembly) lives in files that do not include llama.cpp, so it is unit-tested on the Mac with plain clang. Swift only renders, schedules notifications and calls `CoreEngine`.

**Tech Stack:** C++17, SQLite (FTS5), llama.cpp + mtmd (Gemma 4 E4B), nlohmann/json 3.11.3 (vendored single header), SwiftUI (iOS 17), UserNotifications, String Catalog localisation (en, te).

**Spec:** `docs/superpowers/specs/2026-11-06-personal-assistant-design.md` (read it with this plan; section numbers below refer to it).

## Global Constraints

- Nothing leaves the phone; the only network use is the existing model download (spec §3, §4).
- No reminder, task, profile change, vocabulary candidate or remembered fact is saved without the person tapping Yes; the person's own edits apply directly (spec §7.9 "What needs a Yes").
- Never invent a dose, time or instruction; never give medical advice (spec §4, §7.8, §7.8a).
- Gemma context is 8,192 tokens (`ModelSpec.contextTokens`); the always-included memory block targets ≤ 500 tokens, budgeted as ≤ 2,000 UTF-8 bytes.
- Not-found answer text, exactly: English `I didn't hear that in your recordings.` Telugu `మీ రికార్డింగ్‌లలో ఇది నేను వినలేదు.`
- Notes language (default `en`) and profile preferred language are separate settings (spec §7.9 "Languages").
- One profile per phone (spec §7.9 "Whose facts").
- iOS 17+, existing signing: personal team `3JVSNB8RYM`, only the Increased Memory Limit entitlement.
- Qwen models keep working for transcript → notes; assistant features require a model with `hearsAudio == true` and otherwise show "Needs Gemma 4".
- All new on-screen text is in `Localizable.xcstrings` with English and Telugu values.
- Pure core files (`sve_store.*`, `sve_medcheck.*`, `sve_extract.*`, `sve_context.*`, `sve_learn.*`) must not include llama or mtmd headers.

## Review Focus

1. **Transcript in Telugu script, medicine said in English or vice versa** (e.g. quote has `పారాసిటమాల్`, Gemma writes `Paracetamol`): the suggestion must be flagged "Please check" on the name field, not silently accepted and not dropped. Test in Task 3 `name_in_other_script_is_check`.
2. **Gemma returns malformed or fenced JSON** (```json … ```, trailing prose, truncated output): parsing must strip fences, take the first balanced object, and on failure produce zero suggestions plus a logged error, never a crash or half-saved state. Tests in Task 4 `parses_fenced_json`, `truncated_json_yields_empty`.
3. **Note deleted while a reminder from it is active**: the reminder stays, `from_deleted = 1`, notification still fires, tapping it shows "from a deleted recording" instead of crashing on a missing note. Test in Task 2 `delete_note_keeps_confirmed_reminder`; UI check in Task 12.
4. **Audio expired by retention but transcript exists**: source links open the transcript at that piece. Covered in Task 12 manual check; store side in Task 2 `pieces_survive_audio_clear`.
5. **Memory larger than the budget** (hundreds of facts, long vocabulary): the always-included block is cut to ≤ 2,000 bytes on a UTF-8 boundary, most recent first, and relevant items still appear. Test in Task 5 `always_block_respects_budget_utf8`.

---

## File Structure

Core (`core/`):
- `src/third_party/nlohmann/json.hpp` — vendored JSON (create).
- `src/sve_store.h/.cpp` — extended schema and CRUD for profile, medicines, pieces, suggestions, reminders, tasks, topics, memory, decisions, insights (modify).
- `src/sve_medcheck.h/.cpp` — pure medicine-field check against a quote (create).
- `src/sve_extract.h/.cpp` — extraction prompt builder + tolerant JSON parser (create).
- `src/sve_context.h/.cpp` — memory/context assembler (create).
- `src/sve_learn.h/.cpp` — decision logging → preference suggestions (create).
- `src/sve_llm.h` + `src/sve_llm.cpp` — the `Llm` struct moved out of `sve_core.cpp` (create).
- `src/sve_internal.h` — `struct sve_ctx` definition shared by core .cpp files (create).
- `src/sve_assistant.cpp` — C API glue for assistant features that call the LLM (create).
- `src/sve_core.cpp` — keep notes/LLM/audio API; use the new headers (modify).
- `include/sve_core.h` — new C API (modify).
- `tests/` — `check.h`, `test_*.cpp`, `run.sh`, `smoke_gemma.sh`, `fixtures/doctor_visit.txt` (create).

iOS (`ios/SummaverickEveryday/`):
- `Engine/AssistantModels.swift` — Codable types for the new JSON (create).
- `Engine/CoreEngine+Assistant.swift` — `CoreEngine` methods for the new C API (create).
- `Engine/ReminderScheduler.swift` — UNUserNotificationCenter scheduling + tap routing (create).
- `Engine/ProcessingPipeline.swift`, `Engine/SpeechTranscriber.swift`, `App.swift` — pieces, extract stage, notes language (modify).
- `Views/RootView.swift` — tabs + centre Record button (create).
- `Views/Today/TodayView.swift`, `Views/Today/SuggestionCard.swift` (create).
- `Views/DeepDive/DeepDiveView.swift`, `Views/DeepDive/TopicDetailView.swift` (create); `Views/HomeView.swift` becomes the Timeline list (modify).
- `Views/Ask/AskView.swift` (create).
- `Views/Profile/ProfileSetupView.swift`, `Views/Profile/ProfileView.swift`, `Views/Profile/LearnedView.swift` (create).
- `Views/NoteDetailView.swift` — `startAt` seek, transcript-at-piece fallback (modify).
- `Localizable.xcstrings` (create).
- `ios/make_xcodeproj.py` — keep team ID, add `.xcstrings` resources, pick up new core files (modify).

---

### Task 0: Repo, project generator, test harness

**Files:**
- Modify: `ios/make_xcodeproj.py` (`DEVELOPMENT_TEAM` line ~185; resources phase ~86-140)
- Create: `core/tests/check.h`, `core/tests/test_smoke.cpp`, `core/tests/run.sh`, `core/src/third_party/nlohmann/json.hpp`

**Interfaces:**
- Produces: `core/tests/run.sh` builds every `core/tests/test_*.cpp` with `core/src/{sve_store,sve_medcheck,sve_extract,sve_context,sve_learn}.cpp` (those that exist) and `-lsqlite3`, runs it, exits non-zero on any failure. `check.h` provides `TEST(name)` registration and `CHECK(cond)`, `CHECK_EQ(a,b)` macros printing file:line on failure.

- [ ] **Step 1:** `cd /Users/sumanth.bolle/Downloads/summaverick-everyday && git init && git add -A && git commit -m "chore: baseline before assistant work"` (add `.gitignore` with `.DS_Store`, `build/`, `*.xcuserstate`, `xcuserdata/`, `DerivedData/` first).
- [ ] **Step 2:** Download nlohmann/json v3.11.3 single header from `https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp` into `core/src/third_party/nlohmann/json.hpp`.
- [ ] **Step 3:** In `make_xcodeproj.py` set `"DEVELOPMENT_TEAM": "3JVSNB8RYM"`, and add every `*.xcstrings` under `SWIFT_DIR` to the resources phase as `lastKnownFileType = text.json.xcstrings`.
- [ ] **Step 4:** Write `test_smoke.cpp` with `TEST(harness_runs) { CHECK_EQ(1 + 1, 2); }`; write `check.h` and `run.sh`.
- [ ] **Step 5:** Run `core/tests/run.sh` → expect `1 passed, 0 failed`.
- [ ] **Step 6:** Run `cd ios && python3 make_xcodeproj.py` then `xcodebuild -project SummaverickEveryday.xcodeproj -scheme SummaverickEveryday -destination 'generic/platform=iOS' -allowProvisioningUpdates build` → `** BUILD SUCCEEDED **`, and `git diff ios/SummaverickEveryday.xcodeproj/project.pbxproj` shows `DEVELOPMENT_TEAM = 3JVSNB8RYM` kept.
- [ ] **Step 7:** Commit `chore: test harness, json header, generator keeps team`.

### Task 1: Split `Llm` and `sve_ctx` out of `sve_core.cpp`

**Files:**
- Create: `core/src/sve_llm.h`, `core/src/sve_llm.cpp`, `core/src/sve_internal.h`
- Modify: `core/src/sve_core.cpp`

**Interfaces:**
- Produces: `namespace sve { struct Llm { /* members as today */ bool load(...); std::string apply_template(const std::string&, const std::string&) const; bool complete(const std::string&, int, std::string&, sve_progress_cb, void*, const char*, int, int); bool generate(int, bool, std::string&, sve_progress_cb, void*, const char*, int, int); int count_tokens(const std::string&) const; ... }; std::string strip_thinking(std::string); }` in `sve_llm.h`. `struct sve_ctx { std::unique_ptr<sve::Store> store; std::unique_ptr<sve::Llm> llm; mtmd_context *mtmd; ... }` in `sve_internal.h`. `void sve_logf(int level, const char *fmt, ...)` declared in `sve_internal.h`.

- [ ] **Step 1:** Move code without behaviour change.
- [ ] **Step 2:** Verify with the Gemma smoke build from this session: `core/tests/smoke_gemma.sh` (create it now: builds `core/tests/smoke_gemma.cpp` against Homebrew `llama`/`mtmd` with an include shim dir where `llama -> /opt/homebrew/include`, runs `sve_load_llm` + `sve_load_audio` + `sve_transcribe_audio` on `$SVE_TEST_WAV`, prints transcript). Env: `SVE_GEMMA`, `SVE_MMPROJ`, `SVE_TEST_WAV`. Expect non-empty Telugu transcript.
- [ ] **Step 3:** iOS build succeeds (command from Task 0 Step 6).
- [ ] **Step 4:** Commit `refactor(core): move Llm and sve_ctx into internal headers`.

### Task 2: Store schema and CRUD for assistant data

**Files:**
- Modify: `core/src/sve_store.h`, `core/src/sve_store.cpp`
- Test: `core/tests/test_store_assist.cpp`

**Interfaces:**
- Produces (in `sve::Store`, all JSON as `nlohmann::json`):
  - Schema additions (all `CREATE TABLE IF NOT EXISTS`, FK on): `transcript_pieces(note_id INTEGER REFERENCES notes(id) ON DELETE CASCADE, idx INTEGER, start_sec INTEGER, text TEXT, PRIMARY KEY(note_id, idx))` + `pieces_fts` (fts5, content table, same tokenizer and triggers as notes); `medicines(id, name, dose, when_text, times TEXT /*JSON ["20:00"]*/, repeat TEXT /*"daily"|"twice_daily"|"weekly"|"once"*/, until_date TEXT NULL /*YYYY-MM-DD*/, created_at)`; `suggestions(id, kind TEXT CHECK(kind IN ('reminder','task','profile','vocab','fact')), status TEXT DEFAULT 'pending', payload TEXT /*JSON*/, quote TEXT, note_id INTEGER NULL REFERENCES notes(id) ON DELETE CASCADE, start_sec INTEGER NULL, medicine_id INTEGER NULL REFERENCES medicines(id) ON DELETE CASCADE, check_status TEXT CHECK(check_status IN ('ok','check')), check_fields TEXT /*JSON array*/, created_at)`; `reminders(id, text, times, repeat, start_date, until_date, note_id INTEGER NULL REFERENCES notes(id) ON DELETE SET NULL, start_sec, medicine_id NULL REFERENCES medicines(id) ON DELETE CASCADE, from_deleted INTEGER DEFAULT 0, active INTEGER DEFAULT 1, created_at)`; `tasks(id, text, who, due_date, done INTEGER DEFAULT 0, note_id NULL ON DELETE SET NULL, start_sec, from_deleted INTEGER DEFAULT 0, created_at)`; `topics(id, kind TEXT CHECK(kind IN ('topic','person')), name, summary TEXT NULL, summary_stale INTEGER DEFAULT 1, UNIQUE(kind, name))`; `note_topics(note_id ON DELETE CASCADE, topic_id ON DELETE CASCADE, PRIMARY KEY(note_id, topic_id))`; `memory(id, kind TEXT CHECK(kind IN ('fact','preference','vocab')), text, note_id NULL REFERENCES notes(id) ON DELETE CASCADE, created_at)` + `memory_fts`; `decisions(id, kind, field, from_value, to_value, created_at)`; `insights(id, text, note_id NULL ON DELETE CASCADE, dismissed INTEGER DEFAULT 0, created_at)`. Triggers on `notes` delete set `from_deleted = 1` on reminders/tasks whose `note_id` was the deleted id (BEFORE DELETE trigger).
  - Profile stored in `settings` key `profile` as JSON `{"name":str,"lang":"te"|"en"|...,"style":str,"routine":{"wake":"HH:MM","breakfast":..,"lunch":..,"dinner":..,"bed":..,"classes":str}}`.
  - Methods: `bool set_pieces(int64_t note_id, const json& pieces /*[{start_sec,text}]*/)`, `json pieces(int64_t note_id)`, `json search_pieces(const std::string& q, int limit)` → `[{note_id,title,start_sec,text}]`; `int64_t add_suggestion(const json&)`, `json suggestions(const std::string& status)`, `bool set_suggestion_status(int64_t id, const std::string& status)`, `json suggestion(int64_t id)`; `int64_t add_reminder(const json&)`, `json reminders(bool active_only)`, `bool set_reminder_active(int64_t, bool)`; `int64_t add_task(const json&)`, `json tasks(bool open_only)`, `bool set_task_done(int64_t, bool)`; `int64_t upsert_topic(const std::string& kind, const std::string& name)`, `bool link_topic(int64_t note_id, int64_t topic_id)`, `json topics(const std::string& kind)` → `[{id,name,note_count,open_tasks,last_title}]`, `json topic_notes(int64_t topic_id)`, `bool set_topic_summary(int64_t, const std::string&)`, `bool rename_topic(int64_t, const std::string&)`, `bool merge_topics(int64_t from, int64_t into)`; `int64_t add_memory(const std::string& kind, const std::string& text, std::optional<int64_t> note_id)`, `json memory(const std::string& kind_or_empty)`, `json search_memory(const std::string& q, int limit)`, `bool update_memory(int64_t, const std::string&)`, `bool delete_memory(int64_t)`; `int64_t add_medicine(const json&)`, `json medicines()`, `bool update_medicine(int64_t, const json&)`, `bool delete_medicine(int64_t)`; `bool log_decision(kind, field, from, to)`, `json decisions(kind, field, int limit)`; `int64_t add_insight(...)`, `json latest_insight()`, `bool dismiss_insight(int64_t)`. Linking a note to a topic sets that topic's `summary_stale = 1`.

- [ ] **Step 1: Write failing tests** in `test_store_assist.cpp` using `Store(":memory:")`:
  - `pieces_roundtrip_and_search`: create note, `set_pieces` with `[{0,"ETF index mutual funds"},{28,"debt funds 5%"}]`; `search_pieces("debt")` returns one row with `start_sec == 28` and the note title.
  - `delete_note_keeps_confirmed_reminder`: reminder with `note_id`, delete note → reminder still listed, `from_deleted == 1`, `note_id` null; pending suggestion for that note is gone; pieces gone; memory with that `note_id` gone.
  - `pieces_survive_audio_clear`: `set_text_column(id,"audio_path",nullptr)` leaves `pieces(id)` size 2.
  - `topic_link_marks_stale_and_counts`: upsert topic "Finance class" twice returns same id; link two notes → `topics("topic")[0].note_count == 2`, `summary_stale == 1` after second link even if summary set in between.
  - `merge_topics_moves_links`.
  - `suggestion_status_transitions`: add pending → `suggestions("pending").size()==1` → set `accepted` → pending empty.
  - `memory_search_finds_fact`: add fact "Dr. Rao: under one teaspoon salt a day" → `search_memory("salt")` size 1.
- [ ] **Step 2:** `core/tests/run.sh` → these tests FAIL (methods missing).
- [ ] **Step 3:** Implement schema and methods.
- [ ] **Step 4:** `core/tests/run.sh` → all PASS.
- [ ] **Step 5:** Commit `feat(core): assistant tables and store API`.

### Task 3: Medicine check (pure)

**Files:**
- Create: `core/src/sve_medcheck.h`, `core/src/sve_medcheck.cpp`
- Test: `core/tests/test_medcheck.cpp`

**Interfaces:**
- Produces: `namespace sve { struct MedFields { std::string name, dose, when_text, frequency, duration; }; struct MedCheck { std::string status; /* "ok" | "check" | "drop" */ std::vector<std::string> fields; /* names of fields not found in quote */ }; MedCheck check_medicine(const MedFields& f, const std::string& quote); std::string normalize_for_match(const std::string&); }`
- Rules: a field passes if empty, or if its normalized value appears in the normalized quote. Normalization: lowercase ASCII, collapse whitespace, map English number words one–twelve and Telugu ఒకటి/ఒక→1, రెండు→2, మూడు→3, నాలుగు→4, ఐదు→5 to digits, map Telugu digits ౦–౯ to 0–9, strip punctuation except `.` inside numbers. For `name`, also pass if every ASCII token of length ≥ 4 appears. `drop` when both `name` and `dose` are non-empty and both fail; otherwise `check` if any field fails; else `ok`.

- [ ] **Step 1: Failing tests:**
  - `exact_match_ok`: quote `"Take Amlodipine 5 mg once a day after dinner"`, fields `{Amlodipine,"5 mg","after dinner","once a day",""}` → `status=="ok"`.
  - `invented_dose_is_check`: same quote, dose `"10 mg"` → `status=="check"`, `fields=={"dose"}`.
  - `number_words_match`: quote `"two tablets twice a day"`, dose `"2 tablets"` → ok.
  - `telugu_number_words_match`: quote `"రెండు మాత్రలు రాత్రి"`, dose `"2"` → dose passes.
  - `name_in_other_script_is_check`: quote `"పారాసిటమాల్ 500 mg"`, name `"Paracetamol"`, dose `"500 mg"` → `status=="check"`, fields `{"name"}`.
  - `nothing_matches_is_drop`: quote `"come back next week"`, name `"Metformin"`, dose `"500 mg"` → `drop`.
- [ ] **Step 2:** run → FAIL. **Step 3:** implement. **Step 4:** run → PASS.
- [ ] **Step 5:** Commit `feat(core): code-enforced medicine check`.

### Task 4: Extraction prompt and tolerant parser (pure)

**Files:**
- Create: `core/src/sve_extract.h`, `core/src/sve_extract.cpp`
- Test: `core/tests/test_extract.cpp`

**Interfaces:**
- Produces: `namespace sve { std::string extraction_prompt(const std::string& context_block, const std::string& transcript_with_markers, const std::string& today_iso); struct Extracted { nlohmann::json reminders, tasks, profile, vocab, facts, topics, people; bool ok; }; Extracted parse_extraction(const std::string& model_output); std::string pieces_with_markers(const nlohmann::json& pieces); }`
- Model output contract (copy into the prompt verbatim as the schema): a single JSON object
  `{"reminders":[{"text":"","when_text":"","times":["HH:MM"],"repeat":"once|daily|twice_daily|weekly","until_date":"YYYY-MM-DD|null","quote":"","start_sec":0,"medicine":{"name":"","dose":"","frequency":"","duration":""}|null}],"tasks":[{"text":"","who":"","due_date":"YYYY-MM-DD|null","quote":"","start_sec":0}],"profile":[{"change":"add_medicine|update_medicine|stop_medicine|routine","detail":"","quote":"","start_sec":0,"medicine":{...}|null}],"vocab":["term"],"facts":[{"text":"","quote":"","start_sec":0}],"topics":["name"],"people":["name"]}`.
  Prompt rules: only statements clearly about the listener ("you", "your") go into `profile`; general medicine talk never does; `quote` must be copied word-for-word from the transcript; `start_sec` is the `[t=SEC]` marker of the piece the quote is in; empty arrays when nothing applies; at most 3 topics and 5 people.
- `pieces_with_markers` renders `[t=28] text` lines.
- Parser: strip ``` fences, take the first balanced `{…}` (respecting strings), parse; missing keys become empty arrays; any parse failure → `ok=false`, all arrays empty.

- [ ] **Step 1: Failing tests:** `parses_plain_json`, `parses_fenced_json` (input wrapped in ```json fences with leading "Here you go:"), `missing_keys_become_empty`, `truncated_json_yields_empty` (`ok==false`), `markers_rendered` (`pieces_with_markers([{0,"a"},{28,"b"}]) == "[t=0] a\n[t=28] b\n"`), `prompt_contains_schema_and_today` (prompt contains `"reminders"` and the given `today_iso`).
- [ ] **Step 2:** FAIL. **Step 3:** implement. **Step 4:** PASS.
- [ ] **Step 5:** Commit `feat(core): extraction prompt and tolerant parser`.

### Task 5: Context assembler (pure)

**Files:**
- Create: `core/src/sve_context.h`, `core/src/sve_context.cpp`
- Test: `core/tests/test_context.cpp`

**Interfaces:**
- Consumes: `Store::setting_get("profile")`, `medicines()`, `memory(kind)`, `search_memory`, `search_pieces` (Task 2).
- Produces: `namespace sve { std::string always_block(Store&, size_t max_bytes = 2000); std::string relevant_block(Store&, const std::string& query, size_t max_bytes); std::string truncate_utf8(const std::string&, size_t max_bytes); }`
- `always_block` layout (English labels, the model handles any language): `About the person:` name, preferred language name, speaking style, routine times; `Medicines:` one line each `name dose — when_text (times) repeat`; `Preferences:` most recent first; `Words to spell this way:` comma-separated vocab. Cut to `max_bytes` on a UTF-8 boundary, dropping oldest preferences/vocab first.
- `relevant_block`: up to 5 memory facts and 6 transcript pieces from FTS search on `query`, each piece labelled `[S<n>] <title> @<mm:ss>:` — the label numbering is returned via `json relevant_sources(Store&, const std::string& query)` (same order) so Ask can map `[S2]` back to `{note_id,start_sec,title}`.

- [ ] **Step 1: Failing tests:** `always_block_includes_profile_and_meds` (contains name, `Amlodipine 5 mg`, `20:00`), `always_block_respects_budget_utf8` (500 Telugu vocab entries → result ≤ 2000 bytes and valid UTF-8, i.e. no byte ≥0x80 left dangling at the end), `relevant_block_labels_sources` (query "salt" with one matching piece at 252 s → contains `[S1]` and `@4:12`, and `relevant_sources` returns `start_sec==252`).
- [ ] **Step 2:** FAIL. **Step 3:** implement. **Step 4:** PASS.
- [ ] **Step 5:** Commit `feat(core): memory context assembler`.

### Task 6: Learning from decisions (pure)

**Files:**
- Create: `core/src/sve_learn.h`, `core/src/sve_learn.cpp`
- Test: `core/tests/test_learn.cpp`

**Interfaces:**
- Consumes: `Store::log_decision`, `decisions`, `add_suggestion`.
- Produces: `namespace sve { void record_edit(Store&, const std::string& kind, const nlohmann::json& before, const nlohmann::json& after); }` — logs one decision per changed field (`text`, `times`, `when_text`, `due_date`, `repeat`); when the same `(kind, field, from_value → to_value)` pair has now been logged twice and no pending/accepted `preference` suggestion with the same text exists, adds a pending `fact`-kind suggestion with payload `{"memory_kind":"preference","text":"When I say \"<from>\" I mean <to>"}`.

- [ ] **Step 1: Failing tests:** `second_identical_edit_suggests_preference` (two edits `times ["20:00"]→["21:00"]` with `when_text "after dinner"` → exactly one pending suggestion whose payload text contains `after dinner` and `21:00`), `different_edits_do_not_suggest`, `no_duplicate_preference_suggestion` (third identical edit adds nothing).
- [ ] **Step 2:** FAIL. **Step 3:** implement. **Step 4:** PASS.
- [ ] **Step 5:** Commit `feat(core): learn preferences from repeated edits`.

### Task 7: Assistant C API (glue with LLM)

**Files:**
- Create: `core/src/sve_assistant.cpp`
- Modify: `core/include/sve_core.h`, `core/src/sve_core.cpp` (transcription returns pieces; `sve_note_delete` unchanged — cascade is in SQL)
- Create: `core/tests/fixtures/doctor_visit.txt` (synthetic Telugu/English doctor visit with `[t=…]` markers: Amlodipine 5 mg after dinner daily; blood test report on Saturday; under one teaspoon salt), extend `core/tests/smoke_gemma.sh`

**Interfaces:**
- Produces (C API, all `char*` JSON freed with `sve_free_string`):
  - `char *sve_transcribe_audio_pieces(sve_ctx*, const char *wav_path, sve_progress_cb, void*)` → `[{"start_sec":0,"text":"…"}]` (piece starts in whole seconds from the existing 28 s cutter); `sve_transcribe_audio` keeps working by joining texts.
  - `int sve_note_set_pieces(sve_ctx*, int64_t note_id, const char *pieces_json)`; `char *sve_note_pieces(sve_ctx*, int64_t note_id)`.
  - `int sve_extract(sve_ctx*, int64_t note_id, sve_progress_cb, void*)`: builds `always_block` + transcript markers, runs Gemma (max 900 new tokens, greedy sampling via `Llm::generate(…, greedy=true, …)` for deterministic output), parses; for each reminder/profile item with `medicine` runs `check_medicine` against `quote` (status `drop` → discard; otherwise store `check_status`/`check_fields`); a quote not found verbatim (after `normalize_for_match`) in the cited piece text → `check` with field `quote`; stores tasks/facts/vocab as pending suggestions; upserts and links topics/people directly. Returns number of suggestions added, negative on error. Logs and returns 0 on `ok==false`.
  - `char *sve_suggestions_json(sve_ctx*, const char *status)`.
  - `int sve_suggestion_accept(sve_ctx*, int64_t id, const char *edited_payload_json /*NULL = as proposed*/)`: marks accepted; calls `record_edit` when edited; creates the reminder/task/medicine/memory row from the final payload; returns the new row id (reminder id for reminders, so Swift can schedule).
  - `int sve_suggestion_reject(sve_ctx*, int64_t id)`.
  - `char *sve_profile_get(sve_ctx*)`, `int sve_profile_set(sve_ctx*, const char *json)`.
  - `char *sve_medicines_json(sve_ctx*)`; `int64_t sve_medicine_save(sve_ctx*, const char *json /*id 0 = new*/)`: saves directly (person's own edit) and adds a pending `reminder` suggestion with `medicine_id`, `check_status "ok"`, quote = the typed text; `int sve_medicine_delete(sve_ctx*, int64_t)` (cascades its reminders).
  - `char *sve_reminders_json(sve_ctx*, int active_only)`, `int sve_reminder_set_active(sve_ctx*, int64_t, int)`; `char *sve_tasks_json(sve_ctx*, int open_only)`, `int sve_task_set_done(sve_ctx*, int64_t, int)`.
  - `char *sve_topics_json(sve_ctx*, const char *kind)`, `char *sve_topic_notes_json(sve_ctx*, int64_t)`, `int sve_topic_rename(sve_ctx*, int64_t, const char*)`, `int sve_topic_merge(sve_ctx*, int64_t from, int64_t into)`, `char *sve_topic_summary(sve_ctx*, int64_t topic_id, const char *lang, sve_progress_cb, void*)` (returns cached summary unless stale; otherwise generates `{"summary":"","key_terms":[{"term":"","meaning":""}],"quiz":[{"q":"","a":""}]}` from the topic's notes' summaries via `reduce_to_fit`, caches the JSON string).
  - `char *sve_memory_json(sve_ctx*, const char *kind)`, `int sve_memory_update(sve_ctx*, int64_t, const char*)`, `int sve_memory_delete(sve_ctx*, int64_t)`, `int64_t sve_vocab_add(sve_ctx*, const char *term)` (person's own fix → direct).
  - `char *sve_assistant_ask(sve_ctx*, const char *question, sve_progress_cb, void*)` → `{"answer":"","sources":[{"note_id":0,"start_sec":0,"title":""}],"not_found":bool,"medicine":bool}`. Medicine route when the question (normalized) contains any stored medicine name or one of `tablet, medicine, dose, pill, మందు, మాత్ర, డోస్`: answer built in code from `medicines()` plus up to 3 `search_pieces` hits for those names, formatted as quoted lines with sources, in the profile language (English/Telugu fixed templates), no free generation. Otherwise: prompt = system rules (spec §7.8) + `always_block` + `relevant_block(question)` + question; instruct to cite `[S<n>]` and to reply exactly `NOT_FOUND` when unsupported; map cited labels via `relevant_sources`; `NOT_FOUND` or zero valid citations → `not_found=true` with the exact not-found text from Global Constraints in the profile language.
  - `char *sve_insight_generate(sve_ctx*, int64_t note_id)`: at most one per calendar day (settings key `insight.last_date`); prompt over the last 10 notes' short summaries + open tasks + `always_block`; stores and returns `{"id":0,"text":""}` or `null`.
  - `char *sve_insight_latest(sve_ctx*)`, `int sve_insight_dismiss(sve_ctx*, int64_t)`.
- Every Gemma call in this file prepends `always_block` to the system prompt, and `sve_generate`/`sve_suggest_title` in `sve_core.cpp` do too (spec §7.3).
- Gemma replies are requested in the profile language for Ask/insights/suggestion `text`; notes keep using the notes language passed by the caller.

- [ ] **Step 1:** Add declarations to `sve_core.h` with one-line doc comments.
- [ ] **Step 2:** Implement in `sve_assistant.cpp`.
- [ ] **Step 3:** Extend `smoke_gemma.sh` with a mode `extract`: creates `:memory:` store, profile `{"name":"Lakshmi","lang":"en","routine":{"dinner":"20:00"}}`, note from `fixtures/doctor_visit.txt` pieces, runs `sve_extract`, prints `sve_suggestions_json("pending")`, then `sve_assistant_ask("What did the doctor say about salt?")` and `("When do I take Amlodipine?")`.
- [ ] **Step 4:** Run `SVE_GEMMA=… SVE_MMPROJ=… core/tests/smoke_gemma.sh extract`. Expected: ≥1 reminder suggestion mentioning Amlodipine with `check_status` `ok` and quote containing `5 mg`; a task for the blood test report; salt answer has `not_found:false` and a source with `start_sec` of the salt piece; Amlodipine answer has `medicine:true` and quotes the fixture line verbatim.
- [ ] **Step 5:** `core/tests/run.sh` still all PASS; iOS build succeeds.
- [ ] **Step 6:** Commit `feat(core): assistant C API (extract, suggestions, ask, topics, insights, profile)`.

### Task 8: Swift engine layer, pipeline, notes language

**Files:**
- Create: `ios/SummaverickEveryday/Engine/AssistantModels.swift`, `ios/SummaverickEveryday/Engine/CoreEngine+Assistant.swift`
- Modify: `Engine/ProcessingPipeline.swift`, `ASR/SpeechTranscriber.swift`, `App.swift`, `Views/SettingsView.swift`

**Interfaces:**
- Consumes: Task 7 C API.
- Produces: Codable structs `Profile`, `Routine`, `Medicine`, `Suggestion { id, kind: SuggestionKind, payload: SuggestionPayload, quote, noteID, startSec, medicineID, checkStatus, checkFields }`, `Reminder`, `TaskItem`, `Topic`, `TopicSummary`, `MemoryItem`, `Insight`, `AskAnswer`, `Source`, `TranscriptPiece`. `CoreEngine` methods mirroring each C function with Swift names: `profile() -> Profile?`, `setProfile(_:)`, `medicines()`, `saveMedicine(_:) -> Int64`, `deleteMedicine(_:)`, `suggestions(status:)`, `accept(_ id: Int64, edited: SuggestionPayload?) -> Int64`, `reject(_:)`, `reminders(activeOnly:)`, `setReminderActive(_:_:)`, `tasks(openOnly:)`, `setTaskDone(_:_:)`, `topics(kind:)`, `topicNotes(_:)`, `renameTopic`, `mergeTopic`, `topicSummary(_:lang:) throws -> TopicSummary`, `memory(kind:)`, `updateMemory`, `deleteMemory`, `addVocab`, `ask(_ question: String) throws -> AskAnswer`, `extract(noteID:) throws -> Int`, `latestInsight()`, `generateInsight(noteID:)`, `dismissInsight`, `setPieces(_:_:)`, `pieces(_:)`, `transcribeAudioPieces(path:onProgress:) throws -> [TranscriptPiece]`. `AppSettings.notesLanguage: String` (UserDefaults key `notesLang`, default `"en"`). `Notification.Name.assistantChanged`.
- Pipeline order after transcription (spec §7.9 "Order and timing"): set pieces → set transcript (joined) → **extract** (only when `ModelManager.shared.selected.hearsAudio`) → title → summary/notes/questions in `notesLanguage` → `generateInsight`. Post `.assistantChanged` after extract and insight. Apple-speech path produces pieces too (`SpeechTranscriber.transcribe` returns `[TranscriptPiece]` with `start_sec = index * 55`).
- Settings: when the selected model hears audio, replace the "Language of recordings" picker with a "Notes language" picker (English, Telugu, Hindi, Tamil, Kannada, Malayalam) bound to `notesLanguage`.

- [ ] **Step 1:** Implement.
- [ ] **Step 2:** `python3 make_xcodeproj.py` + iOS build → BUILD SUCCEEDED.
- [ ] **Step 3:** Commit `feat(ios): assistant engine bindings, extract stage, notes language`.

### Task 9: Reminder scheduling

**Files:**
- Create: `ios/SummaverickEveryday/Engine/ReminderScheduler.swift`
- Modify: `App.swift`

**Interfaces:**
- Consumes: `CoreEngine.reminders(activeOnly: true)`.
- Produces: `@MainActor final class ReminderScheduler: NSObject, UNUserNotificationCenterDelegate { static let shared; func requestAuthorization() async -> Bool; func resync() async; var pendingRoute: ReminderRoute? }` with `enum ReminderRoute { case note(id: Int64, startSec: Int?), profile, deletedRecording(reminderID: Int64) }`. `resync` removes all requests with identifier prefix `sve.rem.` and re-adds one `UNCalendarNotificationTrigger` per reminder time: `repeat == "daily"|"twice_daily"` → repeating daily at each time (until_date enforced on each resync at app launch), `weekly` → weekday of `start_date`, `once` → that date. Identifier `sve.rem.<id>.<HH:MM>`; userInfo `{reminder_id, note_id?, start_sec?, from_deleted}`. Title "Summaverick", body = reminder text. Tap sets `pendingRoute`. iOS limits pending notifications to 64: schedule soonest first and stop at 60.
- `App.swift`: set delegate at launch, `resync()` on launch and on `.assistantChanged`.

- [ ] **Step 1:** Implement. **Step 2:** iOS build succeeds.
- [ ] **Step 3:** Commit `feat(ios): local reminder notifications`.

### Task 10: Root tabs and Home (Today)

**Files:**
- Create: `Views/RootView.swift`, `Views/Today/TodayView.swift`, `Views/Today/SuggestionCard.swift`, `Localizable.xcstrings`
- Modify: `App.swift` (show `RootView` instead of `HomeView`)

**Interfaces:**
- Consumes: Task 8 engine methods, Task 9 `ReminderScheduler.resync`.
- Produces: `RootView` with tabs `.today`, `.deepDive`, `.ask` and a centre red Record button (52 pt circle, raised above the bar, uses existing consent + `RecordView` flow). `@Observable final class Router { var tab; var openNote: (id: Int64, startSec: Int?)?; var askDraft: String? }` injected via environment; `ReminderScheduler.pendingRoute` feeds it.
- TodayView (spec §6.1): greeting `Good morning/afternoon/evening, <name>` + weekday + "N things need you today"; sections Waiting for you (pending suggestions → `SuggestionCard`), Reminders (today + next 3 days), To do (open tasks with checkbox), Insight (dismissable), ask bar (tap → `router.tab = .ask`). Keeps `StatusBanner` and `ModelNudge`. If no profile, show first-launch setup sheet (Task 13).
- SuggestionCard: kind icon, Gemma's `text`, the exact `quote` in a quoted block with "▶ <title> @mm:ss" link when `noteID` exists, resolved times/dates, a yellow "Please check" banner listing `checkFields` when `checkStatus == "check"` (those fields highlighted), buttons **Yes / Edit / No** (min 44 pt). Edit opens a form for `text`, `times`, `repeat`, `until_date`, `due_date`, medicine fields; Save calls `accept(id, edited:)`. After accept of a reminder → `ReminderScheduler.resync()`.
- All strings added to `Localizable.xcstrings` with Telugu values.

- [ ] **Step 1:** Implement. **Step 2:** regenerate project, iOS build succeeds.
- [ ] **Step 3: Manual check on iPhone 16e:** record the doctor-visit fixture read aloud (or play `clip-first-2-minutes`) → a suggestion appears; Edit time → Yes → notification scheduled (Settings → Notifications shows pending via a test reminder 2 minutes ahead).
- [ ] **Step 4:** Commit `feat(ios): tabs, Today screen, suggestion cards`.

### Task 11: Deep dive

**Files:**
- Create: `Views/DeepDive/DeepDiveView.swift`, `Views/DeepDive/TopicDetailView.swift`
- Modify: `Views/HomeView.swift` → rename type to `TimelineView` (file `Views/DeepDive/TimelineView.swift`), remove its own Record button and settings sheet ownership (Root owns them)

**Interfaces:**
- Consumes: `topics(kind:)`, `topicNotes`, `topicSummary`, `renameTopic`, `mergeTopic`, `tasks`.
- Produces: segmented Topics · People · Timeline; topic rows with note count, last title, red badge for open tasks; long-press → Rename / Merge into…. TopicDetailView: summary, key terms (term — meaning), quiz (tap to reveal answer), open tasks, recordings list → `NoteDetailView`. Summary loads with a spinner and is cached by the core.

- [ ] **Step 1:** Implement. **Step 2:** build. **Step 3:** Commit `feat(ios): Deep dive topics, people, timeline`.

### Task 12: Ask Summaverick and source links

**Files:**
- Create: `Views/Ask/AskView.swift`
- Modify: `Views/NoteDetailView.swift`

**Interfaces:**
- Consumes: `CoreEngine.ask`, `transcribeAudioPieces` (voice question), `AudioRecorder`, `Router`.
- Produces: chat list of `(question, AskAnswer)` kept in memory for the session; text field + mic button (hold to record ≤ 30 s, then Gemma transcribes and sends); chips "Quiz me", "What's due?", "Summarise this week" (send fixed questions). Each answer shows source links "▶ <title> @mm:ss" → `router.openNote = (id, startSec)`. Each answer also has "Make reminder" and "Make to-do" buttons that open the SuggestionCard Edit form prefilled from the answer (text, source); the person saving that form is their own edit, so it creates the row directly via new C API `int64_t sve_reminder_create(sve_ctx*, const char *json)` and `int64_t sve_task_create(sve_ctx*, const char *json)`, bound as `CoreEngine.createReminder(_:) -> Int64` and `CoreEngine.createTask(_:) -> Int64`. When the selected model does not hear audio, AskView shows "Needs Gemma 4" with a button to Settings (same on TodayView's assistant sections).
- NoteDetailView: new init parameter `startAt: Int? = nil`; when audio exists, seek `AVAudioPlayer.currentTime = startAt` on first play and auto-scroll the transcript to the matching piece; when audio is gone, scroll and highlight the piece; when the note no longer exists (deleted), show "This came from a deleted recording." Transcript shows pieces with tappable `mm:ss` stamps; tapping a word-fix (long-press → "Fix word") calls `addVocab`.
- Not-found answers render the exact Global Constraints text.

- [ ] **Step 1:** Implement (including `sve_reminder_create` in core with a store test `reminder_create_direct` added to `test_store_assist.cpp`).
- [ ] **Step 2:** `core/tests/run.sh` PASS; iOS build succeeds.
- [ ] **Step 3: Manual check:** ask "What did the doctor say about salt?" → answer with source; tap source → note opens at that time. Ask something never recorded → exact not-found text. Ask "When do I take Amlodipine?" → quoted answer, no extra advice.
- [ ] **Step 4:** Commit `feat(ios): Ask Summaverick with sources and voice questions`.

### Task 13: Profile setup, Profile screen, What Summaverick has learned

**Files:**
- Create: `Views/Profile/ProfileSetupView.swift`, `Views/Profile/ProfileView.swift`, `Views/Profile/LearnedView.swift`
- Modify: `Views/SettingsView.swift` (rows "Profile", "What Summaverick has learned"); `Views/SettingsView.swift` "Delete all" also clears profile, medicines, memory, reminders, tasks, topics, insights (add C API `int sve_assistant_wipe(sve_ctx*)` with store test `wipe_clears_everything`) and calls `ReminderScheduler.resync()`

**Interfaces:**
- Consumes: `profile`, `setProfile`, `medicines`, `saveMedicine`, `deleteMedicine`, `memory`, `updateMemory`, `deleteMemory`.
- Produces: setup sheet with 4 skippable steps (name → preferred language → routine times with sensible defaults wake 06:30, breakfast 08:00, lunch 13:00, dinner 20:00, bed 22:00 → medicines list add/edit: name, dose, when (free text, e.g. "after dinner"), times (prefilled from routine keywords breakfast/lunch/dinner/morning/night/ఉదయం/మధ్యాహ్నం/రాత్రి), repeat, until date). Saving a medicine creates a reminder suggestion shown on Today (spec §7.8b). ProfileView edits the same fields. LearnedView lists vocab, preferences, facts with edit/delete, grouped, with a one-line explanation at top. Also request notification permission at the end of setup.

- [ ] **Step 1:** Implement (with the wipe test first: FAIL → implement → PASS).
- [ ] **Step 2:** build. **Step 3: Manual check:** fresh install → setup appears; add "Amlodipine 5 mg after dinner" → Today shows a reminder suggestion at 20:00; Yes → notification pending; LearnedView shows a vocab fix made in Task 12.
- [ ] **Step 4:** Commit `feat(ios): profile setup, profile, learned memory`.

### Task 14: End-to-end verification and polish

**Files:** any touched above; `README.md` (short "Assistant" section)

- [ ] **Step 1:** `core/tests/run.sh` → all PASS; `core/tests/smoke_gemma.sh extract` → expectations from Task 7 Step 4.
- [ ] **Step 2:** iOS build with signing succeeds; run on iPhone 16e.
- [ ] **Step 3: Device script:** (a) record ~2 min Telugu lecture → notes in English with Telugu terms, topic "Finance"/similar created, one insight; (b) record the doctor fixture → reminder + task suggestions with quotes; Edit time twice across two runs → preference suggestion appears; (c) delete the doctor note → confirmed reminder remains, tapping its notification shows "from a deleted recording"; (d) switch iPhone language to Telugu → new screens show Telugu labels; (e) switch model to Qwen → Today/Ask show "Needs Gemma 4", notes still generate.
- [ ] **Step 4:** Move every remaining hard-coded English string in existing screens (Settings, NoteDetailView, RecordView, StatusBanner/pipeline stage labels, alerts) into `Localizable.xcstrings` with Telugu values (spec §7.9 "Languages"); rebuild; spot-check with the iPhone in Telugu.
- [ ] **Step 5:** Update `README.md` with how to run `core/tests/run.sh` and `smoke_gemma.sh`.
- [ ] **Step 6:** Commit `chore: assistant end-to-end verification`.
