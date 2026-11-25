# Second Brain (memory layer, chat, living map, adaptive recall) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** After every recording Summaverick keeps a short list of checked memories (facts, decisions, promises, open questions, people, ideas), each tied to its exact words and time. On top of that list it builds a memories-first chat, one living map across all recordings, and an adaptive recall engine that works inside moments the person already uses. Everything runs on the phone.

**Architecture:** The pure logic lives in new `core/src` files, all covered by `core/tests/run.sh`:
- text matching
- the grounding check
- consolidation
- the scheduler and cue choice
- the moment policy
- grading in code
- map data
- chat trimming

Model-calling glue and the C API for memories, recall and the map go in a new `core/src/sve_brain.cpp`; chat goes in `sve_assistant.cpp`. Both are exercised by `core/tests/smoke_gemma.sh` modes. Memory candidates come from the existing section-summary call, which gets extra JSON fields. They are stored inside the section's summary JSON (sidecar first, then the database). After the notes are written, they are grounded, double-checked and consolidated, one section at a time, resumably. Swift orchestrates (pipeline stage, background and overnight) and draws (review queue, memory card, chat, map, recall card, "Your memory").

**Tech Stack:** C++17, llama.cpp b12119 + mtmd, SQLite FTS5, nlohmann/json. Swift/SwiftUI (iOS 17 target), BackgroundTasks, UserNotifications. Python 3 (stdlib `sqlite3`, `json`, `html`) for the accuracy and meaning-gate tools. EmbeddingGemma 300M GGUF (gated).

**Spec:** `docs/superpowers/specs/2026-11-08-second-brain-design.md`. It extends `docs/superpowers/specs/2026-11-08-long-recordings-chat-mindmap-design.md`, whose §5.5 chat stays and whose §5.6 is replaced. This plan carries over Tasks 9–11 and 14 of `docs/superpowers/plans/2026-11-08-long-recordings-chat-mindmap.md`; its Tasks 12–13 are dropped.

## Global Constraints

- On-device only. The only network use is the model download (Gemma, projector and, if its gate passes, EmbeddingGemma), with the same Wi-Fi and storage rules. No other models.
- Deployment target iOS 17; iOS 26+ APIs behind `#available`. Device: iPhone 16e `00008140-001668892E22801C`.
- New data and AI logic go in `core/`, behind `core/include/sve_core.h`. Swift only orchestrates and draws.
- Context is 8,192 tokens (`ModelSpec.contextTokens`). Model work in the background runs on the CPU (4 threads). The GPU is used only when the app is active and not recording. Pick the backend right before each model run.
- Heat rule for all long work: `Heat.beforeRun`. `.serious` rests 30 s, then runs. `.critical` pauses. Only `.critical` cancels a run in the middle.
- Medicine rules unchanged:
  - Medicine answers are built from stored facts (`medicine_route`), never generated, and are not streamed.
  - Quote checks stay.
  - `check_medicine` behaviour is unchanged: `core/tests/test_medcheck.cpp` passes unmodified.
- **Notes parity:** notes, title and questions for a transcript that fits one prompt (`sve_transcript_fits` path, e00d269) stay byte-identical under greedy decoding. The summary part of the section prompt keeps its wording byte for byte. `always_block` output is unchanged. Section key-point `type` and `memories` never reach the notes renderers (`section_groups`, `merge_chapters`).
- No memory exists without a quote found in its recording. Every name, number, date and time a memory relies on is stored as the exact words of the quote (checked fields), never translated. A wrong memory is worse than a missing one: when in doubt, drop it or send it to the review queue.
- Health-related memories (spec §5.9 definition) always wait in the review queue. Candidates that mention a medicine are not made into memories. Medicine memories come only from saved medicines (Task 4).
- Copy: in-app text says "helps you remember". Never "boosts memory power", never medical claims. Recall feedback never scolds. There are no streaks.
- Every new user-visible string goes in `ios/SummaverickEveryday/Localizable.xcstrings` with English and Telugu. File format is `json.dumps(d, ensure_ascii=False, indent=2)+"\n"`, with no key sorting. `python3 ios/tools/check_strings.py <derivedData>` prints `STRINGS OK`.
- Verify per task:
  - `bash core/tests/run.sh` all pass. New pure core files are added to `SRCS` in `core/tests/run.sh`.
  - Device build succeeds with no new warnings: `cd ios && xcodebuild -project SummaverickEveryday.xcodeproj -scheme SummaverickEveryday -destination 'id=00008140-001668892E22801C' -derivedDataPath <scratch>/dd-dev -allowProvisioningUpdates build`.
  - New Swift or core files need `cd ios && python3 make_xcodeproj.py`.
  - Pure Swift logic is checked with a standalone script: `xcrun swiftc -o <scratch>/<name>/<name> <scratch>/<name>/main.swift <the Foundation-only files>`. Paste its output.
- Scratch dir: `/private/tmp/claude-501/-Users-sumanth-bolle-Downloads-summaverick-everyday-ios/0a2e198b-c172-4f76-84bc-6ed75626fdb9/scratchpad` (derived data: `…/scratchpad/dd-dev`). Do not install on or launch the phone; the controller does device testing. Do not touch `docs/superpowers/2026-11-06-personal-assistant-handover.md`.
- Commits end with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`. Branch `feature/personal-assistant`.
- `PARITY_BASE` = the commit that adds this plan. Parity checks compare it with HEAD.

## Review Focus

1. **A name with a Telugu case ending.** The quote says "రవికి చెప్పాను" and the checked name is "రవి": the memory passes. A checked name the model transliterated ("Ravi" against a Telugu-only quote) is dropped, never shown. Tests in Tasks 1 and 3.
2. **A relative date in an old recording.** "next Friday" in a recording processed three weeks later (old-recording backfill, or overnight) resolves against the recording's date, not today. A recording made just before midnight uses its own local date. Tests in Tasks 1 and 3.
3. **Truncated memory JSON.** A long Telugu section's combined output is cut off inside `"memories"`. The section summary is still kept (salvaged), and the memories are redone with the memory-only prompt. Notes never lose a section because of memories. Tests in Task 4.
4. **A recording deleted mid-way.** The recording is deleted while its memories are being written, or while one of its memories is on the map or due for practice:
   - memories that came only from it disappear, with their recall state;
   - memories with another source keep it;
   - the next `sve_memories_from_section` for it returns -1;
   - nothing crashes or comes back.

   Tests in Tasks 2, 6 and 19.
5. **Ten days away.** The person comes back after ten days away with 40 memories due:
   - never more prompts than the daily cap;
   - promises and medicines due soonest come first;
   - the rest wait;
   - no "you missed" text;
   - no skipped memory comes back within 12 h.

   Tests in Task 19.

---

## Phase A — Memory layer, grounding and accuracy

### Task 1: Telugu and English text matching (core, pure)

**Files:**
- Create: `core/src/sve_textmatch.h`, `core/src/sve_textmatch.cpp`, `core/tests/test_textmatch.cpp`
- Modify: `core/src/sve_medcheck.h/.cpp` (expose today's tokenizer as `std::vector<std::string> med_tokens(const std::string&, bool split = true)` and `bool contains_run(...)`; no behaviour change), `core/tests/run.sh`

**Interfaces:**
- Produces (namespace `sve`):
  - `std::string nfc_clean(const std::string &s)`. Telugu canonical composition (U+0C46 U+0C56 → U+0C48) and canonical reordering of Telugu combining marks (nukta U+0C3C ccc 7, virama U+0C4D ccc 9, U+0C55 ccc 84, U+0C56 ccc 91). It also removes U+200C and U+200D. Other scripts pass unchanged; the portable core has no ICU.
  - `std::vector<std::string> match_tokens(const std::string &s)`. Runs `nfc_clean`, then `med_tokens`, then maps tokens to canonical forms:
    - numbers become digits: English and Telugu words for 0–100, hundreds (వంద/నూరు/వందలు/వందల), thousands (వెయ్యి/వేలు/వేల), lakh (లక్ష/లక్షలు). Compounds add up: "రెండు వందల యాభై" → `250`, "two thousand five hundred" → `2500`;
    - weekdays become `wd1`…`wd7` (Monday = 1; English full and 3-letter forms; సోమవారం…ఆదివారం);
    - months become `mo1`…`mo12` (English; జనవరి…డిసెంబర్);
    - relative words: ఈరోజు/ఈ రోజు/నేడు/today → `rd0`, రేపు/tomorrow → `rd1`, ఎల్లుండి/"day after tomorrow" → `rd2`, నిన్న/yesterday → `rd-1`, మొన్న → `rd-2`, వచ్చే వారం/"next week" → `rw1`, ఈ వారం/"this week" → `rw0`, వచ్చే నెల/"next month" → `rm1`.
  - `std::vector<std::string> canonical_facts(const std::string &text)`. The `match_tokens` that are digits or `wd`/`mo`/`rd`/`rw`/`rm` tokens, in order.
  - `extern const std::vector<std::string> kTeluguCaseEndings`: కి, కు, ని, ను, తో, గారు, గారికి, గారితో, గారిని, గారి, లో, ది, దే, కే, నుంచి, తోటి.
  - `bool name_in(const std::string &name, const std::string &text)`. True when today's `name_ok` matches. Also true when a word of `nfc_clean(text)` starts with the name's last word and adds only an ending from `kTeluguCaseEndings` (the name's other words must match in order before it).
  - `std::string resolve_spoken_date(const std::string &words, const std::string &said_ymd)` returns `YYYY-MM-DD` or `""`. It handles:
    - `rd*`, counted from `said_ymd`;
    - a weekday: the nearest upcoming one after `said_ymd`; with "next"/"వచ్చే", the one in the following week (same rule as `build_suggestions`);
    - "in/<n> days/weeks" and "<n> రోజుల్లో/వారాల్లో";
    - day + month, rolling to next year if already past.

    Anything else returns `""`.

- [ ] **Step 1: Write failing tests** in `test_textmatch.cpp`:
  - `nfc_clean_composes_telugu_ai`: `nfc_clean("ై") == "ై"`, and ZWJ/ZWNJ are removed (`nfc_clean("ర‍వి") == "రవి"`).
  - `match_tokens_numbers`: `canonical_facts("పది రూపాయలు") == {"10"}`, `canonical_facts("రెండు వందల యాభై") == {"250"}`, `canonical_facts("ఒక లక్ష") == {"100000"}`, `canonical_facts("twenty five") == {"25"}`.
  - `match_tokens_days`: `canonical_facts("శుక్రవారం") == canonical_facts("Friday") == {"wd5"}`, `canonical_facts("రేపు") == {"rd1"}`, `canonical_facts("వచ్చే వారం") == {"rw1"}`.
  - `name_in_case_endings`: `name_in("రవి","రవికి చెప్పాను")`, `name_in("రావు","రావు గారికి")`, `name_in("Dr. Rao","Dr. Rao said")`; `!name_in("రవి","రవీంద్ర వచ్చాడు")`; `!name_in("Ravi","రవికి చెప్పాను")`.
  - `resolve_spoken_date_uses_said_date`: `resolve_spoken_date("next Friday","2026-11-06") == "2026-11-13"` (said on a Friday); `resolve_spoken_date("శుక్రవారం","2026-11-04") == "2026-11-06"`; `resolve_spoken_date("రేపు","2026-11-30") == "2026-12-01"`; `resolve_spoken_date("15 January","2026-11-08") == "2027-01-15"`; `resolve_spoken_date("soon","2026-11-08") == ""`.
  - `medcheck_tokens_unchanged`: `normalize_for_match("Dolo 650, పది")` gives the same string as before this task (pin today's output literally: `"dolo 650 పది"`, because medcheck does not map పది).
- [ ] **Step 2: Run** `bash core/tests/run.sh`. Expected: `test_textmatch` fails to build (no header).
- [ ] **Step 3: Implement** the functions above in `sve_textmatch.cpp`. The number parser runs over the token list: a unit word (hundred/thousand/lakh) multiplies the running group, and a larger unit closes it.
- [ ] **Step 4: Run** `bash core/tests/run.sh`. Expected: all pass, `test_medcheck` unchanged.
- [ ] **Step 5: Commit** `feat(core): Telugu-aware text matching for memories`.

### Task 2: Memory store, migration and deletion rules (core, SQLite)

**Files:**
- Modify: `core/src/sve_store.h/.cpp`, `core/tests/test_store_assist.cpp`
- Create: `core/tests/test_memory_store.cpp`

**Interfaces:**
- Produces:
  - Schema migration via `PRAGMA user_version` in `Store::Store` after `kSchema`, as `bool Store::migrate()`. Version 0 → 1:
    - `ALTER TABLE transcript_pieces ADD COLUMN conf REAL`, only if the column is missing;
    - copies every `memory` row with `kind='fact'` into `memories` (type `fact`, `origin 'migrated'`, `review 'kept'`, `confidence 'high'`, `said_date` from `created_at`), then deletes those rows from `memory`;
    - `preference` and `vocab` rows stay in `memory` (so `always_block` and the transcription vocabulary line are unchanged);
    - sets `user_version=1`.
  - Tables:
    ```
    memories(id INTEGER PRIMARY KEY AUTOINCREMENT,
      type TEXT NOT NULL CHECK(type IN ('fact','decision','promise','question','person','idea')),
      text TEXT NOT NULL, who TEXT, to_whom TEXT, due_date TEXT, due_text TEXT, said_date TEXT,
      checked TEXT NOT NULL DEFAULT '[]',            -- [{"kind":"name|number|date|time","text":"<quote words>"}]
      confidence TEXT NOT NULL DEFAULT 'high' CHECK(confidence IN ('high','low')),
      review TEXT NOT NULL DEFAULT 'new' CHECK(review IN ('new','kept','corrected','wrong')),
      queue INTEGER NOT NULL DEFAULT 0, health INTEGER NOT NULL DEFAULT 0, done INTEGER NOT NULL DEFAULT 0,
      importance REAL NOT NULL DEFAULT 0,
      origin TEXT NOT NULL DEFAULT 'recording' CHECK(origin IN ('recording','chat','medicine','migrated')),
      medicine_id INTEGER REFERENCES medicines(id) ON DELETE CASCADE,
      suggestion_id INTEGER REFERENCES suggestions(id) ON DELETE SET NULL,
      superseded_by INTEGER REFERENCES memories(id) ON DELETE SET NULL,
      conflict_with INTEGER REFERENCES memories(id) ON DELETE SET NULL,
      first_seen INTEGER NOT NULL, last_seen INTEGER NOT NULL, last_used INTEGER, created_at INTEGER NOT NULL)
    memory_sources(memory_id INTEGER NOT NULL REFERENCES memories(id) ON DELETE CASCADE,
      note_id INTEGER NOT NULL REFERENCES notes(id) ON DELETE CASCADE, start_sec INTEGER NOT NULL,
      quote TEXT NOT NULL, conf REAL, PRIMARY KEY(memory_id, note_id, start_sec))
    memory_entities(memory_id INTEGER NOT NULL REFERENCES memories(id) ON DELETE CASCADE,
      topic_id INTEGER NOT NULL REFERENCES topics(id) ON DELETE CASCADE,
      role TEXT NOT NULL CHECK(role IN ('who','to_whom','mentioned','topic')), PRIMARY KEY(memory_id, topic_id, role))
    memory_progress(note_id INTEGER NOT NULL REFERENCES notes(id) ON DELETE CASCADE, section_idx INTEGER NOT NULL,
      PRIMARY KEY(note_id, section_idx))
    memories_fts USING fts5(text, content='memories', content_rowid='id', tokenize='unicode61 remove_diacritics 0') + ai/ad/au triggers
    ```
  - Trigger `memory_sources_ad`: after a source is deleted, delete the memory when it has `origin IN ('recording','chat')` and no sources left. This covers note delete through the cascade.
  - `clear_transcript(note_id)` also deletes that note's `memory_sources` and `memory_progress`.
  - `wipe_assistant` also clears `memory_entities`, `memory_sources`, `memories`, `memory_progress`, plus every table added by later tasks (each later task adds its tables to `kTables`).
  - `set_pieces` stores an optional `conf` per piece; `pieces()` returns `"conf"` (null when unknown).
  - Store methods (JSON rows in the shape below):
    - `int64_t add_memory_item(const json &row)`;
    - `bool add_memory_source(int64_t memory_id, int64_t note_id, int64_t start_sec, const std::string &quote, std::optional<double> conf)`;
    - `bool link_memory_entity(int64_t memory_id, int64_t topic_id, const std::string &role)`;
    - `json memory_items(const json &filter)`, where the filter keys are `note_id`, `types` [], `reviews` [], `queue` 0/1, `include_wrong` (default false), `include_superseded` (default false), `limit`;
    - `json memory_item(int64_t id)` (adds `"history"`: the rows whose `superseded_by` chain leads to it, newest first);
    - `bool update_memory_item(int64_t id, const json &fields)` (whitelisted columns only);
    - `bool delete_memory_item(int64_t id)`;
    - `json search_memory_items(const std::string &q, int limit)` (excludes `wrong` and superseded rows);
    - `bool mark_memory_section(int64_t note_id, int idx)`;
    - `json memory_sections_done(int64_t note_id)`;
    - `json notes_needing_memories(int limit)`: notes with sections, at least one section not in `memory_progress`, newest first.
  - Memory row JSON: `{"id","type","text","who","to_whom","due_date","due_text","said_date","checked":[…],"confidence","review","queue","health","done","importance","origin","medicine_id","suggestion_id","superseded_by","conflict_with","first_seen","last_seen","sources":[{"note_id","start_sec","quote","title"}],"people":[{"id","name","role"}]}`.
  - `search_memory` (old table) keeps working for preference/vocab callers. `sve::relevant` switches its "Remembered facts" to `search_memory_items` (current, not wrong).

- [ ] **Step 1: Write failing tests** in `test_memory_store.cpp`:
  - `migrate_old_db_adds_conf_and_moves_facts`: build a v0 file DB with the old `transcript_pieces`/`memory` DDL and rows (one fact, one preference, one vocab), then open it with `Store`. Expect:
    - `PRAGMA user_version` = 1;
    - `pieces()` rows have a `"conf"` key;
    - `memory_items({})` holds the fact with `origin == "migrated"`;
    - `memory("preference")` and `memory("vocab")` are unchanged;
    - a second open does not migrate again (still 1 memory item).
  - `always_block_unchanged_after_migration`: `always_block` on the v0 DB before and after migration returns the same string.
  - `note_delete_removes_only_single_source_memories`: memory A has a source in note 1 only, B in notes 1 and 2. Delete note 1: A is gone, B keeps 1 source.
  - `clear_transcript_drops_memory_sources_and_progress`.
  - `memory_filter_excludes_wrong_and_superseded`.
  - `wipe_clears_memory_tables`.
  - `memories_fts_finds_telugu_text`.
- [ ] **Step 2: Run.** Expected: FAIL.
- [ ] **Step 3: Implement** the schema, `migrate()` and the methods.
- [ ] **Step 4: Run.** Expected: all pass, including the old `test_store_assist` cases.
- [ ] **Step 5: Commit** `feat(core): memory layer tables, migration and deletion rules`.

### Task 3: Grounding check, health flag, review rule and importance (core, pure)

**Files:**
- Create: `core/src/sve_health.h/.cpp`, `core/src/sve_memory.h/.cpp`, `core/tests/test_memory.cpp`
- Modify: `core/tests/run.sh`

**Interfaces:**
- Consumes: Task 1 (`match_tokens`, `canonical_facts`, `name_in`, `resolve_spoken_date`), and `mentions_medicine` from `sve_suggest.h`.
- Produces:
  - `enum class HealthFlag { none, health }; HealthFlag health_keywords(const std::string &q, const std::vector<std::string> &stored_medicine_names)`. Flags:
    - stored medicine names;
    - a common medicine names list;
    - dose, timing, side-effect and symptom words in English and Telugu, including mixed spellings: "tablet", "మాత్ర", "డోస్", "ఎన్ని సార్లు", "side effect", "నొప్పి", "జ్వరం".

    This is old Task 9's guard, moved here; chat uses it in Task 13.
  - `bool is_health_memory(const std::string &text, const std::vector<std::string> &med_names)`. True when `health_keywords` flags, or the text has a word for doctor, hospital, symptom, test result or dose (en/te: doctor/డాక్టర్/వైద్యుడు, hospital/ఆసుపత్రి/హాస్పిటల్, symptom/లక్షణం, report/test result/రిపోర్ట్/పరీక్ష, BP, sugar/షుగర్, dose/డోస్, scan/స్కాన్).
  - `json parse_memory_candidates(const json &arr)`. Keeps up to 8 objects that have:
    - a valid `type` from the six types (also accepts "open question" → `question`);
    - non-empty `text` of at most 300 bytes;
    - a string `quote`.

    Missing arrays become `[]`, missing strings become `""`, and `start_sec` defaults to 0.
  - `struct Grounded { bool ok = false; std::string reason; json row; bool needs_double_check = false; };`
  - `Grounded ground_memory(const json &cand, const SectionPieces &pieces, const std::vector<std::optional<double>> &piece_conf, const std::string &said_ymd, const std::vector<std::string> &med_names, double low_conf_threshold)`. Rules, in order:
    1. `mentions_medicine(text + " " + quote, med_names)` → drop `"medicine"` (medicines have their own path).
    2. The quote's `match_tokens` must be a contiguous run in the joined `match_tokens` of the section's pieces (a run may cross one piece boundary). Else drop `"quote_not_found"`. `row.sources[0].start_sec` is the piece where the run starts. `quote` is stored as the model gave it.
    3. Each entry of `names` must satisfy `name_in(name, quote)`, and each entry of `numbers`/`dates`/`times` must have its `match_tokens` as a run in the quote's tokens. Else drop `"field_not_in_quote:<kind>"`. The passing entries become `checked`.
    4. `canonical_facts(text)` must be a subset of `canonical_facts(quote)` (multiset). Else drop `"text_not_in_quote"`.
    5. `who` and `to_whom` are kept only when `name_in` matches them in the quote, or in the 80 bytes of section text before it, or when they are a pronoun in the quote (I/we/you/he/she/they, నేను/మేము/మనం/నువ్వు/మీరు/అతను/ఆమె/ఆయన/వాళ్ళు). Otherwise they become `""`. This never drops the memory.
    6. `due_text` is kept only if it is a run in the quote. `due_date = resolve_spoken_date(due_text, said_ymd)`.
    7. `health = is_health_memory(text + " " + quote, med_names)`.
    8. `confidence = "low"` when any piece the quote covers has `conf < low_conf_threshold`; unknown conf counts as high.
    9. `needs_double_check` when `checked` is non-empty or `type == "promise"`.
  - `std::string double_check_prompt(const std::string &memory_text, const std::string &quote)`. Asks "Does the quote say this? Answer yes or no." with both texts.
  - `int parse_yes_no(const std::string &model_out)`: 1 when it starts with yes/అవును, 0 for no/కాదు/లేదు, -1 otherwise (after `strip_thinking`, trimmed, lower-cased).
  - `bool goes_to_queue(const json &row, int double_check, bool direct_allowed)`. True when health, confidence low, `double_check == -1`, `conflict_with` is set, or `!direct_allowed`.
  - `constexpr bool kDirectToMapDefault = false;` in `sve_memory.h`. Task 8 sets it to `true` only on GATE: PASS.
  - `constexpr double kLowConfidence = 0.0;` in `sve_memory.h` (never flags). Task 8 sets the measured threshold, or leaves 0.0 if the confidence gate fails.
  - `double memory_importance(const json &row, int people_met_max, int practice_successes, int64_t now)`:
    ```
    base = {promise:3, decision:3, question:2, person:2, fact:1, idea:1}[type]
         + (checked has a number or date ? 1 : 0)
         + min(2.0, 0.5 * max(0, people_met_max - 1))      // people_met_max: most notes any linked person appears in
         + (review in {kept, corrected} ? 2 : 0)
         + 0.5 * min(4, practice_successes)
    importance = base * pow(0.5, days_since(max(last_seen, last_used)) / 60.0)
    ```
    Importance orders the map, recaps and practice; it never hides or deletes a memory.

- [ ] **Step 1: Write failing tests** in `test_memory.cpp`:
  - `health_keywords_english` ("can I take paracetamol with ibuprofen?" → health; "what is inflation?" → none).
  - `health_keywords_telugu` ("ఈ tablet ఎన్ని సార్లు వేసుకోవాలి" → health; "జ్వరం వస్తే ఏం చేయాలి" → health; "ఈ lecture లో ముఖ్యమైన పాయింట్లు ఏమిటి" → none).
  - `health_keywords_stored_name` (stored "Metformin", "metformin timing?" → health).
  - `ground_quote_not_found_drops`: the quote is not in the pieces, so `!ok` and `reason == "quote_not_found"`.
  - `ground_quote_across_two_pieces`: the quote spans the end of piece 0 and the start of piece 1, so `ok`, with start_sec = piece 0's.
  - `ground_number_must_be_in_quote`: text "Pay 500 by Friday", quote "ఐదు వందలు శుక్రవారం లోపు". OK. The same text with 600 is dropped with `text_not_in_quote`.
  - `ground_name_with_case_ending`: names ["రవి"], quote "రవికి రిపోర్ట్ పంపుతాను", so ok. Names ["Ravi"] with the same quote is dropped with `field_not_in_quote:name`.
  - `ground_who_never_guessed`: who "Suresh" and the quote has no Suresh, so ok and `row.who == ""`.
  - `ground_due_date_from_said_date`: due_text "వచ్చే శుక్రవారం", said "2026-11-06", so `due_date == "2026-11-13"`.
  - `ground_medicine_candidate_dropped` ("take Dolo 650 twice", reason `medicine`).
  - `ground_low_confidence_marks_low` (threshold 0.5, piece conf 0.3, so `confidence == "low"`; conf null, so "high").
  - `queue_rule`: health → queue; low → queue; double-check -1 → queue; clean and `direct_allowed` → not queued; clean and `!direct_allowed` → queued.
  - `importance_orders_promise_over_idea_and_decays`: a promise outranks an idea of the same age; the same promise 120 days unused has a quarter of the importance.
  - `parse_yes_no_variants` ("Yes." → 1, "అవును" → 1, "No" → 0, "maybe" → -1).
- [ ] **Step 2: Run.** Expected: FAIL. **Step 3: Implement.** **Step 4: Run.** Expected: PASS.
- [ ] **Step 5: Commit** `feat(core): grounding check, health flag and importance for memories`.

### Task 4: Memories from the section call, memory C API (core + model)

**Files:**
- Modify:
  - `core/src/sve_sections.h/.cpp`: `section_prompt` gets `bool with_memories`, plus memory parsing and summary salvage.
  - `core/src/sve_core.cpp`: `sve_section_summarize` uses the combined prompt, `g_sve_section_max_out` becomes 1024.
  - `core/src/sve_assistant.cpp`: `sve_extract` stops adding `fact` suggestions; accepting an old pending `fact` creates a memory item; saving/accepting a medicine creates its medicine memory.
  - `core/src/sve_internal.h`: move `dup`, `dump`, `guard`, `parse` from `sve_assistant.cpp` here as inline, with no behaviour change.
  - `core/include/sve_core.h`, `core/tests/test_sections.cpp`, `core/tests/smoke_gemma.cpp/.sh` (mode `memories`).
- Create: `core/src/sve_brain.cpp` (memory C API), `tools/model/compare_commits.sh`, `tools/model/compare_lib.sh`. The run/mask/check functions move out of `compare_outputs.sh`, whose behaviour is unchanged.

**Interfaces:**
- Consumes: Tasks 2–3.
- Produces:
  - `extern const char *kMemoryShape;` is the fragment inserted before the closing `}` of the JSON shape line: `,"memories":[{"type":"fact|decision|promise|question|person|idea","text":"…","who":"","to_whom":"","due_text":"","quote":"…","start_sec":0,"names":[],"numbers":[],"dates":[],"times":[]}]`.
  - `std::string memory_rules(const std::string &lang)` holds the extra rule lines, appended after `- Use only what was said. Use an empty array when nothing applies.\n`. They say:
    - at most 8 memories, only things worth remembering later;
    - `text` is one sentence in Telugu/English per `lang`;
    - copy `quote` word for word from one or two neighbouring lines, in the script spoken;
    - `names`/`numbers`/`dates`/`times` hold the exact quote words the memory relies on;
    - `who`/`to_whom` only when said, else "";
    - `due_text` holds the exact deadline words, else "";
    - no memories about medicines or doses;
    - each point may carry `"type"` from the six types.
  - `std::string section_prompt(const SectionPieces&, const std::string &lang, const std::string &vocab_line, bool with_memories)`. With `false` it returns exactly today's prompt.
  - `std::string memory_prompt(const SectionPieces&, const std::string &lang, const std::string &vocab_line)` is the memory-only prompt (same memory shape and rules, reply `{"memories":[…]}`). It is used for older sections and when the combined call's memories are missing.
  - `parse_section_summary` (unchanged keys) also keeps a valid point `"type"`, and adds `"memories": parse_memory_candidates(...)` when present and valid, else `"memories_missing": true`.
  - `bool salvage_summary(const std::string &model_out, const std::vector<int64_t> &starts, json &out)`: when the whole object does not parse, cut at the top-level `,"memories"` key, close with `}`, parse the summary only, and set `memories_missing`.
  - `sve_section_summarize` keeps its signature and order: sampled combined, then greedy combined, then fallback. Before each retry it tries `salvage_summary`. Its fallback sets `memories_missing`.
  - C API (`sve_core.h`, implemented in `sve_brain.cpp`):
    - `int sve_memories_from_section(sve_ctx*, int64_t note_id, int section_idx, sve_progress_cb, void*)`. Returns the memories stored (new plus merged); -1 for no note, no section or bad input; -2 for cancel. It:
      - takes the candidates from the section summary, or runs a greedy `memory_prompt` call (900 tokens);
      - runs `ground_memory` with said date = the note's local `created_at` date and threshold `kLowConfidence`;
      - double-checks (greedy, 3 tokens) the candidates that `needs_double_check`, dropping on 0;
      - stores the row and its source; links people and topics through `topics`, using today's person/topic upsert rules: who and to_whom get roles `who`/`to_whom`; other names get `mentioned`; the note's topic whose name `name_in` the text or quote gets `topic`, otherwise the note's first topic;
      - links a pending reminder or task suggestion of the same note to the memory as `suggestion_id` (same `start_sec` and the shorter quote's tokens a run in the longer's);
      - computes `importance` and marks the section done.

      Consolidation is added in Task 9.
    - `char *sve_memory_sections_due(sve_ctx*, int64_t note_id)` returns `"[0,2]"`.
    - `char *sve_notes_needing_memories(sve_ctx*, int limit)` returns `[id,…]`.
    - `char *sve_memories_json(sve_ctx*, const char *filter_json)`; `char *sve_memory_get(sve_ctx*, int64_t id)`.
    - `int sve_memory_review(sve_ctx*, int64_t id, const char *state)` takes `"kept"` or `"wrong"`. Wrong also sets `queue=0`; kept sets `queue=0`.
    - `int sve_memory_edit(sve_ctx*, int64_t id, const char *text)`: the person's edit is applied directly and sets `review='corrected'`, `queue=0`.
    - `int sve_memory_remove(sve_ctx*, int64_t id)`; `int sve_memory_set_done(sve_ctx*, int64_t id, int done)`.
    - `char *sve_review_queue_json(sve_ctx*)`: rows with `queue=1` and `review='new'`, newest note first, each with `"suggestion"` (the linked pending suggestion object or null).
  - Medicine memories: when a medicine is saved (`sve_medicine_save`) or a profile add/update is accepted, upsert one memory with `origin 'medicine'`, `type 'fact'`, `medicine_id`, `health 1` and `review 'kept'`. Its text is built in code from the saved row, like `reminder_text`, as "<name> <dose>, <when_text>". The medicine delete cascades.
  - Smoke mode `memories`:
    - Env: `SVE_DB` (a copy of a phone `notes.sqlite`, never the original), `SVE_NOTES` (`all` or ids).
    - Writes missing sections (combined call), then `sve_memories_from_section` for every due section.
    - Prints per note `memories: <shown> shown, <queued> queued, <dropped> dropped (<reason>:<n>…)` and `--- <n.n> s` per section.
    - Drop reasons are logged to a table `memory_drops(note_id, section_idx, reason, cand_json)`, created only by the smoke mode, for the report.

- [ ] **Step 1: Write failing tests** in `test_sections.cpp`:
  - `section_prompt_summary_part_unchanged`: for fixed pieces and lang te/en, `section_prompt(p, l, v, false)` equals a golden literal copied from today's `section_prompt` output (paste it in the test). For the combined prompt, erasing `kMemoryShape` and `memory_rules(l)` from `section_prompt(p, l, v, true)` gives exactly `section_prompt(p, l, v, false)`.
  - `parse_section_summary_ignores_memories`: the same output with and without a `"memories"` array gives identical title/topic/points(text, start_sec)/terms/hints.
  - `section_groups_ignore_types_and_memories`: `section_groups` and `merge_chapters` output is identical for sections with and without point `type` and `memories`.
  - `salvage_summary_from_truncated_memories`: a valid summary followed by `,"memories":[{"type":"fact","text":"cut` gives `true`, the summary's title and points, and `memories_missing == true`.
  - `parse_memories_bad_array_marks_missing`: `"memories":"none"` gives `memories_missing == true`.
- [ ] **Step 2: Run.** Expected: FAIL. **Step 3: Implement.** **Step 4: Run.** Expected: PASS.
- [ ] **Step 5: Notes parity.** Run `tools/model/compare_commits.sh $PARITY_BASE notes-te notes-en fits`. It builds the smoke binary at the base in a `git worktree` under the scratch dir and at HEAD, runs each item greedy (`SVE_GREEDY=1`) with the masking of `compare_outputs.sh`, and prints `<item>: IDENTICAL` or the first difference, then `PARITY: PASS|FAIL`. Items are `compare_outputs.sh` item names plus `fits` and `transcribe-all` (every WAV in `TESTSET`). Expected: `PARITY: PASS`. Also run `longnotes` and paste its diff: section summaries may differ there (the prompt is longer), so it is reported, not gated.
- [ ] **Step 6: Smoke** `SVE_DB=<copy of fixture-built db> smoke_gemma.sh memories` on the doctor-visit fixture and the Telugu reference clip. Paste the counts, drop reasons and per-section seconds; Task 6 uses the seconds.
- [ ] **Step 7: Commit** `feat(core): memories from section summaries, checked and stored`.

### Task 5: Transcript confidence (core + Swift data)

**Files:**
- Modify:
  - `core/src/sve_llm.h/.cpp`: `generate` gets an optional `double *mean_prob`.
  - `core/src/sve_core.cpp`: `transcribe_piece`, `transcribe_pieces`, `sve_transcribe_audio_pieces` and `sve_transcribe_next_piece` add `"conf"`.
  - `core/src/sve_assistant.cpp`: `sve_note_set_pieces` keeps `conf`.
  - `ios/SummaverickEveryday/Engine/AssistantModels.swift` (`TranscriptPiece.conf: Double?`, encoded when non-nil), `Engine/TranscriptSidecar.swift` (`Piece.conf: Double?`, `append(startSec:endSample:text:conf:)`), `Engine/LiveTranscriber.swift` (incl. `TranscriptRun.pieces`), `Engine/CoreEngine+Assistant.swift`.

**Interfaces:**
- Produces: `bool Llm::generate(int max_new_tokens, bool greedy, std::string &out, sve_progress_cb cb, void *user, const char *phase, int done, int total, double *mean_prob = nullptr)`. When `mean_prob` is set, it is the mean over generated non-EOG tokens of the chosen token's softmax probability under the raw logits (`llama_get_logits_ith(ctx, -1)`, before penalties). It is computed after sampling and never changes the choice. With 0 tokens it is NaN; JSON writes null. Piece JSON adds `"conf": 0.0–1.0 | null`. Recordings from before have null.

- [ ] **Step 1:** swiftc check `conf_check/main.swift` with `TranscriptSidecar.swift` + `AssistantModels.swift` covers:
  - an old sidecar JSON without `conf` decodes;
  - `append` with conf 0.42 round-trips through encode/decode;
  - `TranscriptPiece` without conf encodes no `conf` key.

  Paste the output.
- [ ] **Step 2: Implement.**
- [ ] **Step 3: Parity:** `tools/model/compare_commits.sh $PARITY_BASE transcribe-all notes-te notes-en`. Expected: `PARITY: PASS`. Conf values go to stderr in smoke `transcribe`, so stdout stays comparable. Paste the conf range seen on the test set.
- [ ] **Step 4:** `bash core/tests/run.sh` passes (`set_pieces`/`pieces` with conf round-trip test added to `test_memory_store.cpp`: `pieces_conf_roundtrip`). Device build passes.
- [ ] **Step 5: Commit** `feat: per-piece transcript confidence`.

### Task 6: Memories in the pipeline, background and overnight (iOS engine)

**Files:**
- Create: `ios/SummaverickEveryday/Engine/MemoryModels.swift` (`BrainMemory`, `MemorySource`, `ReviewItem`; Foundation only), `ios/SummaverickEveryday/Engine/CoreEngine+Memory.swift`
- Modify:
  - `Engine/TranscriptSidecar.swift`: `SectionSummary.memories: [JSONValue]?`, `memoriesMissing: Bool?` (`memories_missing`), `Point.type: String?`, all round-tripped. `JSONValue` moves from `AssistantModels.swift` to `TranscriptSidecar.swift` so the file stays Foundation-only and self-contained.
  - `Engine/ProcessingPipeline.swift`: stage `.memories(Int, Int)`, `writeMemories(noteID:quiet:) async -> Bool?` after the three outputs and before `generateInsight`, plus `backfillMemories()`.
  - `Engine/OvernightWork.swift`: after marked notes, notes needing memories, newest first.
  - `Engine/ModelLogic.swift`: `WorkEstimate.secondsPerSection` set to Task 4's measured combined value, new `secondsPerMemorySection`, `minutesLeft(audioSecondsLeft:sectionsLeft:outputsLeft:memorySectionsLeft:)`.
  - `Localizable.xcstrings`.

**Interfaces:**
- Consumes: Task 4 C API.
- Produces:
  - `struct BrainMemory: Identifiable, Decodable, Equatable` mirrors the Task 2 row: `type`, `text`, `who`, `toWhom`, `dueDate`, `dueText`, `saidDate`, `checked: [CheckedField]`, `lowConfidence: Bool`, `review`, `queued`, `health`, `done`, `importance`, `origin`, `suggestionID`, `sources: [MemorySource]`, `people`, `history: [BrainMemory]`, with lenient decoding as in `AssistantModels`.
  - `CoreEngine` methods:
    - `memoriesFromSection(noteID:idx:shouldStop:) throws -> Int`;
    - `memorySectionsDue(_:) -> [Int]`;
    - `notesNeedingMemories(limit:) -> [Int64]`;
    - `memories(_ filter: MemoryFilter) -> [BrainMemory]`;
    - `memory(_:) -> BrainMemory?`;
    - `reviewMemory(_:_:)`, `editMemory(_:_:)`, `removeMemory(_:)`, `setMemoryDone(_:_:)`;
    - `reviewQueue() -> [ReviewItem]`.
  - Pipeline rule: memories run after the notes. They are never before them and never block them. They run one section at a time, each after the heat gate, with the same backend rule. A `-1` result (note gone) ends the run quietly. Cancel keeps finished sections. `backfillMemories` runs only when not busy and the AI is ready: from `processWaitingNotes` when idle in front (quiet, stops when a recording starts), and from the overnight task. It takes `notesNeedingMemories(limit: 20)` newest first.
  - `.memories` label: "Finding things to remember… part %d of %d". Posts `.assistantChanged` after each section.

- [ ] **Step 1:** swiftc check `memstage_check/main.swift` covers:
  - a `SectionSummary` with `memories` and point `type` survives decode → encode → decode unchanged;
  - an old summary without them decodes with nil;
  - `WorkEstimate.minutesLeft` with `memorySectionsLeft: 12` adds `12 * secondsPerMemorySection`, rounded up;
  - `offersOvernight` crosses at the same `offerAboveMinutes`.

  Paste the output.
- [ ] **Step 2: Implement.** Review Focus 4: test that deleting the note between two sections ends `writeMemories` with no failure banner. Use a debug-only `ProcessingPipeline.testHook` if needed, or show it with the smoke `memories` mode deleting the note mid-run and expecting `-1` and no crash. Paste the output.
- [ ] **Step 3:** Device build, `STRINGS OK`.
- [ ] **Step 4: Commit** `feat(ios): write memories after the notes, in the background and overnight`.

### Task 7: Review queue and memory card (iOS UI)

**Files:**
- Create: `ios/SummaverickEveryday/Views/Memory/MemoryCardView.swift`, `Views/Memory/MemoryReviewCard.swift`, `Views/Memory/NoteMemoriesSection.swift`
- Modify: `Views/Today/TodayView.swift` (a "To check" section above reminders when the queue is non-empty), `Views/NoteDetailView.swift` ("Remembered" section under the chapters), `Views/Profile/LearnedView.swift` (the Facts group is removed; facts are now memories), `Views/Today/SuggestionCard.swift` (hide suggestions linked to a queued memory), `Localizable.xcstrings`

**Interfaces:**
- Consumes: Task 6.
- Produces: `MemoryCardView(memoryID:onPlay:)`, reused by the map (Task 17) and recall (Task 21). It shows:
  - text, type, quote in quotation marks, recording title and time with ▶, who, said date, due date with the spoken words;
  - "I may have heard this wrong. Listen?" when low confidence;
  - "New" when `review == new`;
  - the history line from `history` ("Was %@, now %@, said on %@", filled in Task 10).

  Actions: Edit (sheet with a text field), Wrong, Delete, Done (promise/question only). `MemoryReviewCard` uses the Yes / Edit / No pattern of `SuggestionCard` as Keep / Edit / Wrong. A linked pending suggestion shows inside the same card as one action row ("Remind me" / "Make a task", with that suggestion's Yes / Edit / No): one card per thing.
- Strings (en/te): "To check", "Keep", "Wrong", "Edit", "Delete memory", "Done", "Remembered", "New", "I may have heard this wrong. Listen?", "Said on %@", "Due %@ (\"%@\")", "Waiting for you to check", "Health-related: please check".

- [ ] **Step 1:** swiftc check `review_check/main.swift` with `MemoryModels.swift` covers:
  - review-queue JSON with and without a linked suggestion decodes;
  - `ReviewItem.showsSuggestionRow` is true only when the suggestion is pending.

  Paste the output.
- [ ] **Step 2: Implement.** Every memory row plays its source (▶ opens the note at `start_sec` via `NoteMoment`).
- [ ] **Step 3:** Device build, `STRINGS OK`.
- [ ] **Step 4: Commit** `feat(ios): memories to check, and a card for every memory`.

### Task 8: Accuracy test set, gate and confidence gate (tools + owner checkpoint)

**Files:**
- Create: `tools/memory/testset.py`, `tools/memory/README.md`, `docs/superpowers/2026-11-08-memory-accuracy-report.md`
- Modify (only after the measurement): `core/src/sve_memory.h` (`kDirectToMapDefault`, `kLowConfidence`)

**Interfaces:**
- Consumes: Task 4 smoke `memories`, Task 5 conf.
- Produces:
  - Getting the data: copy `Library/Application Support/Summaverick/notes.sqlite` (+ `-wal`, `-shm`) and the `recordings/` files from the phone with `xcrun devicectl device copy from …`, as in `tools/model/testset/README.md`. Work on a copy. All private data stays git-ignored under `tools/memory/private/`. If the phone cannot be read, follow Ruling R3 of the earlier plan: use the Mac's WAVs and fixtures, report the smaller set, and the gate still applies.
  - `python3 tools/memory/testset.py export --db <copy> --audio <dir> --out <dir> [--target 100] [--pieces 60]`:
    - picks notes newest first until at least 100 shown memories (map and queue both count as shown) exist, after running `smoke_gemma.sh memories` on them;
    - writes `sheet.html`, one offline page. Each memory shows type, text, who, due date and words, the quote, and an `<audio>` of its recording from `start_sec` (`#t=` fragment). Right / Wrong radios; on Wrong, a reason: "wrong words heard", "wrong meaning", "wrong person", "wrong date or number". Each recording gets a "missed memories" text box;
    - adds a second part with `--pieces` transcript pieces sampled evenly across conf deciles, each with its audio and "heard right / heard wrong";
    - "Save marks" downloads `marks.json` keyed by memory id and piece rowid, plus the db sha256.
  - `python3 tools/memory/testset.py score --db <copy> --marks marks.json` prints:
    - `SHOWN-CORRECT: <right>/<marked> = <p>%  GATE: PASS|FAIL (>= 95%)`;
    - a per-type and per-reason breakdown;
    - `MISSED (reported): <n>`;
    - the confidence gate: AUC of conf for "heard wrong" pieces; the best threshold t (deciles); at t, the share of wrong pieces flagged and of right pieces flagged; `CONF-GATE: PASS|FAIL`.
- **Accuracy gate (spec §5.3, §7.3):**
  - Measured: of the memories shown (passed every check), the share the owner marks right.
  - Pass: ≥ 95% on at least 100 marked memories.
  - If it fails: the controller and implementer study the wrong ones by reason and tighten the rules, for example routing a type or reason to the queue, or requiring the double-check for all. They re-run `export` on the same notes (new memories need new marks: the owner marks only new or changed rows; marks carry over for identical text+quote). Until a run passes, `kDirectToMapDefault` stays `false`, so every memory waits in the review queue, which is still correct and useful. The map (Task 17) then shows only memories the person kept.
- **Confidence gate (spec §5.9):**
  - Measured: whether low per-piece conf picks out pieces the owner marked "heard wrong".
  - Pass: AUC ≥ 0.70, and some threshold t flags ≥ 50% of wrong pieces while flagging ≤ 20% of right pieces, on at least 60 marked pieces with at least 8 marked wrong.
  - If it passes: set `kLowConfidence = t`.
  - If it fails: `kLowConfidence` stays `0.0`. No memory is ever marked "may have heard wrong"; the column stays (cheap) and the report says it was dropped.

- [ ] **Step 1:** Write `testset.py` with `export`/`score`, plus a self-test `python3 tools/memory/testset.py selftest`. The self-test builds a tiny SQLite with 3 memories and 4 pieces, writes fake marks, and expects `SHOWN-CORRECT: 2/3 = 66.7%  GATE: FAIL` and a computed AUC. Paste the output.
- [ ] **Step 2:** Run `export` on the owner's data. Paste the counts. **Owner checkpoint:** the controller gives the owner `sheet.html` (about 30–40 minutes) and waits for `marks.json`.
- [ ] **Step 3:** Run `score`. Write the report (numbers, breakdown, both gate lines, which constants change).
- [ ] **Step 4:** Apply the constants per the rules above. `bash core/tests/run.sh` passes (`queue_rule` still passes with explicit arguments).
- [ ] **Step 5: Commit** `feat(tools): memory accuracy test set; gate results` (no private data).

## Phase B — Consolidation and meaning search

### Task 9: Consolidation (core)

**Files:**
- Create: `core/src/sve_consolidate.h/.cpp`, `core/tests/test_consolidate.cpp`
- Modify: `core/src/sve_brain.cpp` (consolidate before storing), `core/src/sve_store.h/.cpp` (table `person_questions(id INTEGER PRIMARY KEY AUTOINCREMENT, a_topic INTEGER NOT NULL REFERENCES topics(id) ON DELETE CASCADE, b_topic INTEGER NOT NULL REFERENCES topics(id) ON DELETE CASCADE, a_context TEXT, b_context TEXT, status TEXT NOT NULL DEFAULT 'pending' CHECK(status IN ('pending','same','different')), created_at INTEGER NOT NULL)`, wipe), `core/include/sve_core.h`, `core/tests/run.sh`

**Interfaces:**
- Consumes: Tasks 1–4.
- Produces:
  - `std::string transliterate_te(const std::string &s)`. A table of Telugu vowels, consonants and vowel signs to Latin (inherent "a" unless a sign or virama follows; ం → "m"). The output is normalised like the Latin side.
  - `std::string person_key(const std::string &name)`. `nfc_clean`, then strip the Telugu case endings and honorifics (dr, dr., mr, mrs, ms, sir, madam, garu, గారు, అన్న, అక్క, sir), then transliterate. Latin is lower-cased, doubled letters collapsed, aa→a, ee→i, oo→u, w→v.
  - `enum class PersonMatch { same, maybe, different }; PersonMatch match_person(const std::string &a, const std::string &b)`:
    - same: the normalised raw names are equal, or the `person_key`s are equal and neither name had an honorific the other lacks;
    - maybe: one name's words are a subset of the other's after stripping honorifics, or the keys are within edit distance 1 and the shorter is ≥ 4 letters;
    - otherwise different.
  - `std::string same_person_prompt(a_name, a_context, b_name, b_context)`, answered with `parse_yes_no`.
  - `enum class Pair { unrelated, same, update, contradiction }; Pair classify_pair(const json &newer, const json &older, double cosine /* <0 when unknown */)`. The two must have the same type and share an entity (a person link in common, or both have none and share a topic):
    - `sim` = Jaccard of content tokens (`match_tokens` minus `canonical_facts` minus a short en/te stopword list);
    - same: `(sim >= 0.6 || cosine >= 0.9)` and the canonical facts are equal;
    - update/contradiction: `(sim >= 0.4 || cosine >= 0.8)` and the two have a differing canonical fact of a kind both have (number, or date: `wd`/`mo`/`rd`/digits in a date field). It is an update when `newer.said_date > older.said_date`, a contradiction when the dates are equal;
    - otherwise unrelated.
  - `json change_line(const json &older, const json &newer)` returns `{"was","now","said"}` from the first differing checked field of the same kind (quote words as stored).
  - Behaviour in `sve_memories_from_section`. People: each new name is checked against the existing person topics:
    - same: link to the existing topic;
    - maybe, with a model: Gemma same-person check. No: a new topic. Yes or unclear: a new topic plus a `person_questions` row (never merged silently);
    - different: a new topic.

    Memories: against current memories of the same type that share an entity:
    - same: add a source to the older row and update its `last_seen`; no new row;
    - update: insert the newer, then set `older.superseded_by = newer.id`;
    - contradiction: insert the newer with `conflict_with = older.id`, and set `queue = 1` on both.
  - C API: `char *sve_person_questions_json(sve_ctx*)` returns `[{"id","a":{"id","name","context"},"b":{…}}]` (pending only). `int sve_person_question_answer(sve_ctx*, int64_t id, int same)`: same calls `merge_topics(b → a)` and sets status `same`; else status `different`.
  - `sve_memory_get` adds `"change"` (from `change_line` against the newest superseded row) when history exists.

- [ ] **Step 1: Write failing tests**:
  - `match_person_examples`: ("Ravi","రవి") same; ("రవికి","Ravi") same; ("Lakshmi","లక్ష్మి") same; ("Rao","Dr. Rao") maybe; ("Ravi","Raju") different.
  - `classify_same_fact_again` (same text, same "500", different notes → same).
  - `classify_update_newer_date` (Friday on 6 Nov → Monday on 9 Nov, same person → update, `change_line == {"was":"శుక్రవారం","now":"సోమవారం","said":"2026-11-09"}` with Telugu quotes).
  - `classify_contradiction_same_day`.
  - `classify_unrelated_other_person`.
  - Store-level `person_question_answer_merges_topics` (in `test_memory_store.cpp`).
- [ ] **Step 2: Run.** Expected: FAIL. **Step 3: Implement.** **Step 4: Run.** Expected: PASS. Run smoke `memories` again on the Task 8 data; paste merged/updated/conflict counts.
- [ ] **Step 5: Commit** `feat(core): merge memories across recordings; ask before merging people`.

### Task 10: Same-person questions, changes and conflicts in the app (iOS)

**Files:**
- Modify: `Engine/CoreEngine+Memory.swift` (`personQuestions()`, `answerPersonQuestion(_:same:)`), `Engine/MemoryModels.swift` (`PersonQuestion`, `BrainMemory.change`), `Views/Memory/MemoryReviewCard.swift`, `Views/Memory/MemoryCardView.swift`, `Views/Today/TodayView.swift`, `Localizable.xcstrings`

**Interfaces:**
- Consumes: Task 9.
- Behaviour:
  - "To check" also lists person questions: "Is %@ from %@ the same as %@?", with buttons "Same person" and "Different". Each side shows its context line.
  - Conflict pairs show as one card: "These two disagree", with both memories and their ▶, and Keep on either one marks the other Wrong.
  - The memory card shows `change` as "Was %@, now %@, said on %@".
- Strings (en/te): those four lines, plus "Same person", "Different".

- [ ] **Step 1:** swiftc check: `PersonQuestion` and `change` decode, and the change line formats with a Telugu date (`said` formatted by the note-date formatter). Paste the output.
- [ ] **Step 2: Implement.** **Step 3:** Device build, `STRINGS OK`. **Step 4: Commit** `feat(ios): ask before merging people; show what changed`.

### Task 11: Meaning model in the core and its gate (core + tools, gated experiment)

**Files:**
- Create: `core/src/sve_embed.h/.cpp`, `core/tests/test_embed.cpp` (pure parts), `tools/memory/meaning_gate.py`, `tools/memory/meaning/README.md`, `docs/superpowers/2026-11-08-meaning-gate-report.md`
- Modify: `core/src/sve_llm.h/.cpp` (`Llm::load(..., bool embeddings = false)`: `cp.embeddings = true`, pooling from the model, `n_ctx` 2048), `core/src/sve_internal.h` (`std::unique_ptr<sve::Llm> embed;` in `sve_ctx`), `core/src/sve_store.h/.cpp` (table `embeddings(kind TEXT NOT NULL CHECK(kind IN ('piece','memory')), ref INTEGER NOT NULL, vec BLOB NOT NULL, model TEXT NOT NULL, PRIMARY KEY(kind, ref))`; triggers delete rows when a piece (by rowid) or memory is deleted; wipe), `core/include/sve_core.h`, `core/src/sve_brain.cpp`, `core/tests/smoke_gemma.cpp/.sh` (modes `embed`, `search`)

**Interfaces:**
- Produces:
  - `std::vector<float> embed_normalize(std::vector<float> v, size_t dims)`: truncate to `dims` (Matryoshka), then L2-normalise.
  - `float cosine(const float*, const float*, size_t)`.
  - `std::vector<std::pair<int64_t,double>> rrf(const std::vector<int64_t> &keyword_ranked, const std::vector<int64_t> &meaning_ranked, int k = 60)`: score = Σ 1/(k + rank), rank from 1, sorted by score descending, ties by keyword rank.
  - Constants: `kEmbedDims = 256`; prompts `"task: search result | query: "` (query) and `"title: none | text: "` (document).
  - C API:
    - `int sve_load_embedder(sve_ctx*, const char *gguf_path, int n_threads)`: CPU only. It is a second model, loaded alongside Gemma; never unloads Gemma.
    - `void sve_unload_embedder(sve_ctx*)`; `int sve_embedder_loaded(const sve_ctx*)`.
    - `int sve_embed_pending(sve_ctx*, int limit, sve_progress_cb, void*)`: pieces and current memories without a vector, newest notes first; returns the number embedded, or -2 on cancel.
    - `char *sve_search_json(sve_ctx*, const char *query, int limit)`: returns `[{"kind":"piece","note_id","start_sec","score"}|{"kind":"memory","memory_id","score"}]`. It fuses FTS (top 20, using `keyword_query`) with cosine (top 20) by `rrf`. With no embedder it returns the FTS order.
- **Meaning gate (spec §5.5, §7.4):**
  - Test set: `tools/memory/private/questions.jsonl` (git-ignored). At least 45 questions, 15 Telugu, 15 English and 15 mixed, over the owner's recordings from Task 8. Each is `{"q","lang","note_id","gold_start_secs":[…]}` (1–2 gold pieces). The builder writes them from the transcripts as paraphrases. `meaning_gate.py check` rejects any question that copies 3 or more consecutive words of its gold piece. **Owner checkpoint:** the owner reviews the questions. The file's sha256 is written into the report before any measurement, and the file is not edited after.
  - Measured: hit@5, meaning a gold piece is in the top 5 pieces, for keyword only (today's `keyword_query` + `search_pieces`) and for keyword + meaning (`sve_search_json` pieces). Both run on the Task 8 database copy, with the smallest EmbeddingGemma 300M GGUF quant at or under ~200 MB first, then Q8_0 if that one fails. Also measured: CPU embed time per piece (Mac, 4 threads), index size, and resident memory with both models loaded.
  - Pass: hybrid hits ≥ keyword hits + 3 overall, and hybrid is not worse than keyword by more than 1 question in any language group.
  - If it fails: nothing ships. There is no download, and Task 12 is not dispatched. Keyword search stays. Consolidation and the map use `cosine = -1` (no meaning links). The report records the result, and the core code stays dormant (no model, so `sve_search_json` = FTS order).
- Licence: the report notes the Gemma terms for EmbeddingGemma; the owner checks them before release (Task 23).

- [ ] **Step 1: Write failing tests** in `test_embed.cpp`:
  - `embed_normalize_truncates_and_normalises` (768 → 256, norm 1 ± 1e-5).
  - `rrf_fuses_ranks` (keyword [1,2,3] and meaning [3,4,1] → order [1,3,2,4]).
  - `embeddings_deleted_with_memory` (store-level).
- [ ] **Step 2: Run.** Expected: FAIL. **Step 3: Implement.** **Step 4: Run.** Expected: PASS. Smoke `embed` + `search` on the fixture: paste the top 5 for two questions.
- [ ] **Step 5:** `meaning_gate.py run --db <copy> --questions <file> --embedder <gguf>` prints a per-language table and `MEANING-GATE: PASS|FAIL`. Write the report.
- [ ] **Step 6: Commit** `feat(core): optional meaning search with EmbeddingGemma; gate report`.

### Task 12: Meaning search in the app (iOS, only if Task 11 passed)

**Precondition:** `MEANING-GATE: PASS` in the Task 11 report. If it failed, the controller records "skipped: gate failed" in the ledger and moves on.

**Files:**
- Modify: `Engine/ModelManager.swift` (`ModelSpec.embedder: ModelPart?`; `ModelCatalog.gemma` gets the quant that passed, with URL and approxBytes from the report; downloaded after the main parts with the same Wi-Fi/cellular, space and resume rules; `isReady` does not wait for it; `embedderReady: Bool`), `Engine/ModelLogic.swift` (space check includes it), `Engine/CoreEngine+Memory.swift` (`loadEmbedder`, `embedPending(limit:shouldStop:)`), `Engine/ProcessingPipeline.swift` (after memories: `embedPending` in batches of 50, CPU, heat rule, quiet), `Engine/OvernightWork.swift`, `Views/SettingsView.swift` (size line), `Localizable.xcstrings`

**Interfaces:**
- Consumes: Task 11.
- Behaviour: phones that already have Gemma fetch only the embedder, on Wi-Fi. Old notes are indexed in the background or overnight, newest first. Retrieval (Task 13) and map links (Task 16) use it when it is loaded.

- [ ] **Step 1:** swiftc check: `DownloadMath`/`SpaceCheck` with three parts; `isReady` true with the embedder missing. Paste the output.
- [ ] **Step 2: Implement.** **Step 3:** Device build, `STRINGS OK`. **Step 4: Commit** `feat(ios): download and use the meaning model`.

## Phase C — Chat (old Tasks 9–11, memories first)

### Task 13: Conversations, prompt trimming and memories-first material (core, pure)

**Files:**
- Create: `core/src/sve_chat.h`, `core/src/sve_chat.cpp`, `core/tests/test_chat.cpp`
- Modify: `core/src/sve_store.cpp/.h` (tables; `sections_fts`), `core/src/sve_context.h/.cpp` (`chat_relevant`), `core/tests/run.sh`, `core/include/sve_core.h`, `core/src/sve_assistant.cpp` (C wrappers), wipe (also clears chats)

**Interfaces:**
- Consumes: Task 3 `health_keywords`; Task 2 `search_memory_items`; Task 11 `sve_search_json` when loaded.
- Produces:
  - Tables, with messages deleted together with their conversation:
    - `conversations(id INTEGER PRIMARY KEY, title TEXT, created_at INTEGER, updated_at INTEGER)`;
    - `messages(id INTEGER PRIMARY KEY, conversation_id INTEGER REFERENCES conversations(id) ON DELETE CASCADE, role TEXT CHECK(role IN ('user','assistant')), text TEXT, kind TEXT, sources_json TEXT, created_at INTEGER)`;
    - `sections_fts` over the section titles and points, with triggers on `sections`.
  - C API:
    - `int64_t sve_chat_create(sve_ctx*, const char *title)`;
    - `char *sve_chats_json(sve_ctx*)` (newest first: id, title, updated_at);
    - `char *sve_chat_messages(sve_ctx*, int64_t id)`;
    - `int sve_chat_delete(sve_ctx*, int64_t id)`.
  - `struct Turn { std::string role, text; }`. `std::string chat_prompt(const std::string &system, const std::vector<Turn> &turns, const std::string &material, const std::string &question, int budget_tokens, const std::function<int(const std::string&)> &count_tokens)`:
    - keeps the system prompt, the material and the question;
    - adds turns newest first while they fit;
    - when any are dropped, adds one line `Earlier in this chat: <first 15 words of each dropped user turn, joined by "; ">`, trimmed to fit.
  - `Relevant chat_relevant(Store&, const std::string &query, size_t max_bytes, const json &meaning_hits /* sve_search_json result or [] */)`. Material in this order:
    1. memories (current, not wrong, not queued), labelled `[M1]…`, each with its quote and its first source;
    2. pieces `[S1]…`;
    3. section summaries `[C1]…`.

    `sources` maps every label to `{note_id,start_sec,title}`, with `memory_id` for M labels. Meaning hits (when given) are merged into the memory and piece candidates before byte trimming, in RRF order.
  - `apply_citations` accepts `[M<n>]` and `[C<n>]` as well as `[S<n>]`.

- [ ] **Step 1: Write failing tests**:
  - `chat_store_roundtrip_and_cascade`; `wipe_clears_chats`.
  - `chat_prompt_keeps_newest_turns_within_budget` (40 turns, small budget → newest kept, the "Earlier in this chat:" line present, total ≤ budget, question last).
  - `chat_relevant_memories_first` (a memory and a piece both match → `[M1]` appears before `[S1]` in the block; a `wrong` memory is absent).
  - `citations_map_memory_labels`.
  - `sections_fts_finds_point_term`.
- [ ] **Step 2: Run.** Expected: FAIL. **Step 3: Implement.** **Step 4: Run.** Expected: PASS.
- [ ] **Step 5: Commit** `feat(core): chat history, prompt trimming and memories-first material`.

### Task 14: Chat answers: streaming, routing, regenerate, edit, remember (core)

**Files:**
- Modify: `core/src/sve_llm.h/.cpp` (optional text callback in `generate`/`complete`, no output change), `core/src/sve_assistant.cpp`, `core/src/sve_brain.cpp` (`sve_memory_from_chat`), `core/include/sve_core.h`, `core/src/sve_prompts.h`
- Test: `core/tests/test_chat.cpp` (routing), smoke mode `chat`

**Interfaces:**
- Consumes: Task 13; existing `medicine_route`, `is_medicine_question`, `not_found_text`, `mentions_medicine`.
- Produces:
  - `typedef int (*sve_text_cb)(const char *new_text, void *user);`. Called with each new decoded fragment; non-zero cancels.
  - `char *sve_chat_send(sve_ctx*, int64_t conversation_id, const char *message, sve_text_cb on_text, sve_progress_cb cb, void *user)`. Stores the user turn, answers, stores the assistant turn, and returns `{"message_id":N,"text":"…","kind":"recordings|general|medicine|not_found","sources":[…],"stopped":false}`.
  - `char *sve_chat_regenerate(sve_ctx*, int64_t conversation_id, sve_text_cb, sve_progress_cb, void*)`. Deletes the last assistant message and answers the last user message again, routed afresh. Recordings and general answers are sampled (the default chain) so the wording can change. Medicine and not_found answers go through the same code-built, non-streamed path.
  - `char *sve_chat_edit_last(sve_ctx*, int64_t conversation_id, const char *new_message, sve_text_cb, sve_progress_cb, void*)`. Replaces the last user message's text, deletes its answer, and answers.
  - Routing (pure): `std::string chat_route(bool is_medicine_question, HealthFlag kw, bool model_says_health, bool has_relevant_material)`:
    - `"medicine"` if `is_medicine_question` (existing `medicine_route`, not streamed);
    - `"recordings"` if material was found (streamed; sources attached; health questions allowed only here);
    - `"not_found"` if `kw == health || model_says_health` and there is no material (text: the not-found line + " Please ask your doctor." in the profile language);
    - `"general"` otherwise (streamed; text prefixed with "From Gemma's general knowledge, not your recordings:" in the profile language).
  - `model_says_health`: one short greedy classification call (`yes`/`no`, max 3 tokens), run only when the keywords did not flag and no material was found.
  - First answers are greedy, as `sve_assistant_ask` is today. A recordings answer that `mentions_medicine` is replaced by verbatim quotes, exactly as `sve_assistant_ask` does.
  - Prompt rule for recordings answers with a general addition: the recordings part comes first with its sources, and the general addition sits under its own label. When general knowledge differs from the recording, the answer says so ("Your recording says X; generally Y").
  - Stop: a cancelled answer is stored with what was written plus `"stopped":true`. Medicine answers are never partial.
  - `int64_t sve_memory_from_chat(sve_ctx*, int64_t message_id)`. Allowed only for `recordings` messages with sources. The quote is the first cited piece's text, and `canonical_facts(answer)` must be a subset of the union of the cited pieces' facts (else `-3`). It creates a memory with `origin 'chat'`, `queue 1`, `review 'new'` and a source per cited piece. Returns -1 for any other kind.

- [ ] **Step 1: Write failing tests**:
  - `chat_route` for all four outcomes (health + material → recordings; health + no material → not_found; neither → general).
  - `regenerate_medicine_same_text` (store-level with a stub: two medicine answers for the same question are byte-identical).
  - `memory_from_chat_refuses_unbacked_number` (the answer has "600" and the cited piece only "500" → -3).
- [ ] **Step 2: Run.** Expected: FAIL. **Step 3: Implement.** **Step 4: Run.** Expected: PASS.
- [ ] **Step 5: Smoke mode `chat`:** a 4-turn conversation on the fixture: a recordings question, a follow-up "and what else?", "what is GDP?", and "how many times should I take dolo?". Then regenerate the last and edit-and-resend the first. Print kinds, the first 200 chars, time to first text and total time. Also run notes parity again: `compare_commits.sh $PARITY_BASE notes-te notes-en` → `PARITY: PASS`. Paste the outputs.
- [ ] **Step 6: Commit** `feat(core): multi-turn chat with streaming, regenerate, edit and remember`.

### Task 15: Chat screen (iOS)

**Files:**
- Create: `ios/SummaverickEveryday/Views/Ask/ChatListView.swift`, `ios/SummaverickEveryday/Views/Ask/ChatView.swift`, `ios/SummaverickEveryday/Engine/ChatModels.swift` (includes the pure `TextThrottle`)
- Modify:
  - `Views/Ask/AskView.swift`: becomes the entry (list + new chat); keep voice input, starter chips and the Yes / Edit / No card actions.
  - `Engine/CoreEngine+Assistant.swift`: `chatSend(conversationID:message:onText:shouldStop:) async throws -> ChatReply`, `chatRegenerate(conversationID:onText:shouldStop:)`, `chatEditLast(conversationID:message:onText:shouldStop:)`, `memoryFromChat(messageID:) -> Int64`.
  - `Views/SettingsView.swift`: check that "Delete everything" removes chats.
  - `Localizable.xcstrings`.

**Interfaces:**
- Consumes: Tasks 13–14.
- Produces: `struct ChatReply: Decodable { message_id: Int64; text: String; kind: String; sources: [Source]; stopped: Bool }` (the existing `Source` type).
- Behaviour:
  - Answers stream into the bubble; the UI updates at most every 100 ms. Stop cancels: the partial answer is kept and marked "(stopped)".
  - Medicine replies appear whole. General replies show their label in a smaller line. Sources are tappable chips that open the note at that time.
  - The model runs on the GPU when in front and not recording, CPU otherwise.
  - The chat list takes titles from the first question (first 6 words); swipe to delete.
  - Copy on every answer (`UIPasteboard`). Regenerate only on the last answer. Edit on the last question only: it puts the text in the box, and sending calls `chatEditLast`.
  - "Remember this" on recordings answers; -3 shows "I couldn't find all of this in your recordings, so I didn't save it."; success shows "Saved. You can check it in To check."
- Strings (en/te): "New chat", "Chats", "Stop", "(stopped)", "From Gemma's general knowledge, not your recordings", "Please ask your doctor.", "Copy", "Copied", "Regenerate", "Edit", "Remember this", plus the two lines above.

- [ ] **Step 1:** swiftc check for `TextThrottle`: fragments arriving every 5 ms give ≤ 10 UI updates per second, and the final text equals the concatenation. `ChatReply` with `stopped` decodes. Paste the output.
- [ ] **Step 2:** Implement the views and wiring. **Step 3:** Device build, `STRINGS OK`.
- [ ] **Step 4: Commit** `feat(ios): Ask becomes a chat with history, streaming, sources, regenerate and edit`.

## Phase D — The second-brain map

### Task 16: Map data (core)

**Files:**
- Create: `core/src/sve_brainmap.h/.cpp`, `core/tests/test_brainmap.cpp`
- Modify: `core/src/sve_brain.cpp`, `core/include/sve_core.h`, `core/tests/run.sh`

**Interfaces:**
- Consumes: Tasks 2–3, 9; embeddings when present (Task 11).
- Produces:
  - `json build_brain_map(const json &memories, const json &people, const json &topics, const json &meaning_pairs, int64_t note_id /* 0 = all */, int per_branch = 6)`, pure. It returns `{"center":{"kind":"you"|"recording","note_id"},"list":bool,"branches":[{"kind":"people|topics|promises|decisions|questions","items":[{"ref":"m12"|"t3","label","importance","new","low_confidence","done","count"}],"more":N}],"links":[{"a","b","kind":"person|topic|meaning"}]}`. Rules:
    - include only current memories that are not wrong and not queued (`queue=0`);
    - with `kDirectToMapDefault == false`, only `review in (kept, corrected)` count;
    - items are ordered by importance descending; open promises and questions come before done ones;
    - each branch shows `per_branch` items and `more` = the rest;
    - people and topics items carry `count`, their number of memories;
    - facts and ideas sit under their topic;
    - links: memories sharing a person or topic; meaning links with cosine ≥ 0.8, at most 2 per memory;
    - `list = true` when there are fewer than 8 items in total;
    - `note_id > 0` keeps only memories with a source in that note, with `center.kind = "recording"`.

    Labels are the memory or topic text (branch names are localised in Swift).
  - C API:
    - `char *sve_brain_map_json(sve_ctx*, int64_t note_id)`;
    - `char *sve_topic_memories_json(sve_ctx*, int64_t topic_id)`: memories under a person or topic, by importance; this is "Who is this?" for a person;
    - `int sve_memory_touch(sve_ctx*, int64_t id)`: sets `last_used`, for importance.

- [ ] **Step 1: Write failing tests**:
  - `map_orders_by_importance_and_caps` (9 promises → 6 shown, `more == 3`).
  - `map_excludes_wrong_queued_superseded`.
  - `map_list_when_few` (5 items → `list == true`).
  - `map_per_recording_filters_and_centres`.
  - `map_meaning_links_capped` (3 pairs ≥ 0.8 for one memory → 2 links).
  - `map_direct_off_shows_only_kept`.
- [ ] **Step 2: Run.** Expected: FAIL. **Step 3: Implement.** **Step 4: Run.** Expected: PASS.
- [ ] **Step 5: Commit** `feat(core): one living map of memories across recordings`.

### Task 17: Map screen with actions (iOS)

**Precondition:** the controller shows the owner a mockup of the map screen (the living map in Deep dive, the per-recording view in a note, the tap card with its actions, the few-memories list, Telugu labels) and records the approved layout in this task's brief before dispatch. No dispatch without approval.

**Files:**
- Create: `ios/SummaverickEveryday/Views/Map/BrainMapView.swift`, `Views/Map/BrainMapLayout.swift` (pure, Foundation + CoreGraphics), `Engine/MapModels.swift`
- Modify: `Views/DeepDive/DeepDiveView.swift` ("Map" as the first segment), `Views/NoteDetailView.swift` ("Map" choice for this recording), `Views/Memory/MemoryCardView.swift` (map actions), `Engine/CoreEngine+Memory.swift` (`brainMap(noteID:)`, `topicMemories(_:)`, `touchMemory(_:)`), `Views/RootView.swift` (Router: `askDraft` from "Ask about this"), `Localizable.xcstrings`

**Interfaces:**
- Consumes: Task 16; Task 7 card; the existing suggestion editor (`SuggestionCard`'s Edit sheet, as AskView's `AnswerFormRequest` does).
- Produces: `BrainMapLayout.layout(map: BrainMap, size: CGSize, expanded: Set<String>) -> [PlacedNode]`. The centre sits in the middle and the branches go evenly around it. Items fan outward from their branch. No two node frames overlap for 5 branches × 6 items with "more" nodes. Every frame is ≥ 44 × 44 pt.
- Behaviour:
  - Pinch to zoom (0.5×–3×), drag to pan, tap a branch to fold or unfold (all folded at first when more than 4 branches have items). "More" opens a list.
  - Tapping a memory opens `MemoryCardView` with these actions:
    - **Remind me / Make a task**: the existing editor prefilled with text and the due date; a time is never invented; Yes / Edit / No;
    - **Ask about this**: opens chat with the draft "Tell me more about: <text>";
    - **Done** (promises, questions); **Wrong**; **Edit**.

    "Practise this" is added in Task 21.
  - Tapping a person or topic opens its memories list ("Who is this?" for people, with sources).
  - `list: true` renders a plain list. Readable in Telugu and English.
- Strings (en/te): "Map", "You", "People", "Topics", "Promises", "Decisions", "Open questions", "%d more", "Remind me", "Make a task", "Ask about this", "Tell me more about: %@", "Who is this?".

- [ ] **Step 1:** swiftc check for `BrainMapLayout`:
  - 5 branches × 6 items, all expanded, on 390 × 700: no overlapping frames, all ≥ 44 pt;
  - folded branches place no items;
  - the same input gives the same output.

  Paste the output.
- [ ] **Step 2:** Implement the views and wiring. **Step 3:** Device build, `STRINGS OK`.
- [ ] **Step 4: Commit** `feat(ios): the second-brain map with actions on every memory`.

## Phase E — Adaptive recall engine

### Task 18: Scheduler and cue choice (core, pure)

**Files:**
- Create: `core/src/sve_recall.h/.cpp`, `core/tests/test_recall.cpp`
- Modify: `core/tests/run.sh`

**Interfaces:**
- Produces (namespace `sve`):
  ```cpp
  enum class Outcome { correct, partly, wrong, skipped };
  enum class Cue { free_recall = 0, partial = 1, choice = 2, errorless = 3 };   // higher = more help
  struct RecallState { double stability = 0, difficulty = 5; int64_t due_at = 0, last_at = 0;
                       int reps = 0, lapses = 0, skips = 0; Cue last_cue = Cue::errorless; bool in_practice = false; };
  constexpr double kTargetRetention = 0.85;
  double retrievability(const RecallState &s, int64_t now);          // 1.0 when reps == 0
  double interval_days(double stability);                            // days until R falls to kTargetRetention
  RecallState schedule_after(const RecallState &s, Outcome o, Cue c, int64_t now, int64_t deadline /* 0 = none */);
  Cue choose_cue(const RecallState &s, int64_t now, const std::string &mode /* auto|free|cued|errorless */,
                 int cue_bias /* -1..3 */, bool is_medicine, bool has_choices);
  ```
- Algorithm (FSRS-style; designer's call, fixed here):
  ```cpp
  // R(t) = (1 + (19/81) * t / S)^-0.5, t in days since last_at.   interval = S * (81/19) * (r^-2 - 1)  (≈ 1.637 S at r = 0.85)
  static const double kCueWeight[4] = {1.0, 0.6, 0.35, 0.2};       // success at a harder cue stretches more
  static const double kFirstS[4]    = {0.8, 0.65, 0.55, 0.5};      // first interval ≈ 0.8–1.3 days
  RecallState schedule_after(const RecallState &s0, Outcome o, Cue c, int64_t now, int64_t deadline) {
      RecallState s = s0;
      const int ci = (int)c;
      if (o == Outcome::skipped) {                                     // a signal, not a failure
          s.skips += 1;
          s.due_at = now + (int64_t)(12 * 3600) * (1LL << std::min(s.skips - 1, 3));   // 12 h, 24 h, 48 h, 96 h
          if (s.skips >= 3) s.in_practice = false;                     // caller keeps medicines in practice
          return s;
      }
      s.skips = 0;
      const double R = s.reps == 0 ? 1.0 : retrievability(s, now);
      if (o == Outcome::wrong) {
          s.stability = s.reps == 0 ? 0.25 : std::max(0.25, std::min(s.stability * 0.5, 0.6 * std::pow(s.stability, 0.6)));
          s.lapses += 1;
          s.difficulty += 0.8;
      } else {
          const double q = o == Outcome::partly ? 0.5 : 1.0;
          if (s.reps == 0) s.stability = kFirstS[ci] * (o == Outcome::partly ? 0.8 : 1.0);
          else s.stability = std::max(s.stability * 1.05, s.stability * (1 + 12.0 * kCueWeight[ci] * q * ((11 - s.difficulty) / 6.0)
                                                   * std::pow(s.stability, -0.15) * (std::exp(1.2 * (1 - R)) - 1)));
          s.difficulty += o == Outcome::partly ? 0.2 : (c == Cue::free_recall ? -0.4 : -0.15);
      }
      s.difficulty = std::clamp(0.95 * s.difficulty + 0.05 * 5.0, 1.0, 10.0);
      s.reps += 1; s.last_at = now; s.last_cue = c;
      s.due_at = now + std::max<int64_t>(3600, (int64_t)(interval_days(s.stability) * 86400));
      if (deadline > 0 && now < deadline - 3600 && s.due_at > deadline - 86400)   // strong before its date
          s.due_at = std::max(now + 4 * 3600, deadline - 86400);
      return s;
  }
  Cue choose_cue(const RecallState &s, int64_t now, const std::string &mode, int bias, bool is_medicine, bool has_choices) {
      if (is_medicine || mode == "errorless") return Cue::errorless;
      int L;
      if (mode == "free") L = 0;
      else {
          const double R = s.reps == 0 ? 0.5 : retrievability(s, now);
          L = R >= 0.85 ? 0 : R >= 0.65 ? 1 : R >= 0.45 ? 2 : 3;
          if (mode == "cued") L = std::max(L, 1);
          if (mode == "auto") L = std::clamp(L + bias, 0, 3);
      }
      if (s.reps > 0) L = std::max(L, (int)s.last_cue - 1);          // vanishing cues: one step less help at a time
      if (L == 2 && !has_choices) L = 1;
      return (Cue)L;
  }
  ```

- [ ] **Step 1: Write failing tests**:
  - `first_interval_about_a_day`: a correct free first attempt gives a due time between 1.0 and 1.5 days; errorless gives 0.7–0.9 days.
  - `expanding_intervals`: four correct free recalls at their due times give strictly increasing intervals, the 4th > 7 days.
  - `harder_cue_stretches_more`: from the same state, correct with free gives a later due than correct with partial, which is later than errorless.
  - `failure_shortens`: wrong after S = 10 gives S ≤ 5 and a due time < 2 days.
  - `skip_backs_off_and_drops`: skips give dues +12 h, +24 h, +48 h; after 3, `!in_practice`; S and D unchanged.
  - `deadline_pulls_review_before_date`: deadline in 3 days, S = 10 → due ≤ deadline − 1 day.
  - `cue_vanishes_one_step`: last cue errorless, R high → choice (not free).
  - `cue_medicine_always_errorless`; `cue_choice_needs_choices` (→ partial).
  - `cue_bias_moves_toward_help`: auto, R = 0.9, bias +2 → choice.
- [ ] **Step 2: Run.** Expected: FAIL. **Step 3: Implement** as above. **Step 4: Run.** Expected: PASS.
- [ ] **Step 5: Commit** `feat(core): spaced recall scheduler with vanishing cues`.

### Task 19: Moments, adaptation, grading in code, recall store and C API (core)

**Files:**
- Modify: `core/src/sve_recall.h/.cpp`, `core/src/sve_store.h/.cpp`, `core/src/sve_brain.cpp`, `core/include/sve_core.h`, `core/tests/test_recall.cpp`, `core/tests/test_memory_store.cpp`

**Interfaces:**
- Consumes: Task 18; Task 1 matching; Task 3 importance; Task 2 rows.
- Produces:
  - Tables (cascade with memories; in wipe):
    - `recall(memory_id INTEGER PRIMARY KEY REFERENCES memories(id) ON DELETE CASCADE, stability REAL, difficulty REAL, due_at INTEGER, last_at INTEGER, reps INTEGER, lapses INTEGER, skips INTEGER, last_cue INTEGER, in_practice INTEGER, question TEXT, pinned INTEGER NOT NULL DEFAULT 0)`;
    - `recall_log(id INTEGER PRIMARY KEY AUTOINCREMENT, memory_id INTEGER REFERENCES memories(id) ON DELETE CASCADE, at INTEGER NOT NULL, moment TEXT NOT NULL, cue INTEGER, outcome TEXT NOT NULL CHECK(outcome IN ('correct','partly','wrong','skipped','offered')), response_ms INTEGER, due_at INTEGER)`.
  - Settings key `recall`: `{"on":true,"style":"support|improve","mode":"auto|free|cued|errorless","cue_bias":N,"recap":{"on":false,"time":"20:00"},"practice":{"on":false,"time":"09:00","auto_time":true}}`. Defaults:
    - style `support` until the person picks one;
    - `cue_bias` starts at +3 for support and 0 for improve.
  - Pure policy functions:
    - `bool enters_practice(const json &memory_row, bool pinned, int64_t now)`:
      - pinned, or medicine origin, counts;
      - otherwise review `kept`/`corrected`, or type `promise`/`decision` with review `new`, `queue 0`, `health 0` and first seen ≥ 1 day ago;
      - never wrong, superseded or done.
    - `int daily_cap(const json &log_last_14_days, int64_t now)`:
      - 3 with no data;
      - else `clamp(ceil(0.8 * A), 1, 8)`, where A is the average answered prompts per active day;
      - +1 when on ≥ 5 of the last 7 active days answered == offered == cap.
    - `bool moment_allowed(const json &log, const std::string &moment)`: over the last 30 offers in that moment, `rate = (answered + 1) / (offered + 2)`. Allowed when offered < 5, or rate ≥ 0.25, or `offered % 4 == 0` (it keeps learning).
    - `int adapt_bias(int bias, const json &last_10_attempts)`: −1 when ≥ 9 of 10 are correct, +1 when ≤ 5 are, clamped to −1…3; applied after every 10 attempts in auto mode.
    - `std::string practice_hour(const json &log_14_days, const std::string &set_time, bool auto_time)`: with `auto_time` and ≥ 8 answers, the hour with the most answers (earliest on ties), clamped to 07:00–21:00; else `set_time`.
    - `std::vector<int64_t> pick_due(const json &candidates, const std::string &moment, const std::string &style, int remaining_cap, int64_t now)`:
      - only items with due ≤ now and no skip in the last 12 h;
      - order: deadline within 2 days first, then medicines, then importance, then the oldest due;
      - improve style interleaves notes and topics (no two in a row from the same note when another is available); support takes 1 at a time;
      - never more than `remaining_cap`.
  - Grading in code: `json grade_in_code(const json &memory_row, const std::string &answer, Cue cue, const std::string &chosen_id)` returns `{"outcome":"correct|partly|wrong|ask_model","feedback":"right|almost|partly|shown","expected","given"}`:
    - choice: exact id;
    - errorless: the gap's `match_tokens` must equal the answer's;
    - free/partial:
      - every checked field must be present in the answer (`name_in` / canonical run);
      - a checked field of the same kind present with a different value → `wrong` with feedback `almost`, `expected`, `given` ("Almost: it was Monday, not Friday.");
      - all fields present, and content-word overlap ≥ 0.5 → `correct`;
      - all fields present but overlap < 0.5, or no fields → `ask_model`;
      - fields missing, none wrong → `partly`.

    Numbers, names, dates and medicines must match exactly.
  - Cue text built in code:
    - `std::string partial_cue(const json &row, const std::string &lang)`: each checked field becomes its first letter + "…";
    - `std::string gapped_text(const json &row)`: one checked field, or the longest content word, replaced by "____", with `gap_answer`;
    - `json choices_for(const json &row, const json &others)`: 2 distractors of the same field kind, taken from the person's other memories only. Fewer than 2 → `[]`.
  - Question templates: `std::string question_template(const json &row, const std::string &lang)`:
    - promise: "What was promised to %s?" / "%s కి ఏమి మాట ఇచ్చారు?";
    - decision: "What was decided about %s?";
    - question: "What was left open about %s?";
    - person: "Who is %s?";
    - fact/idea: "What do you remember about %s?" (the topic name, else the recording title).

    Telugu versions sit beside each one.
  - C API:
    - `char *sve_recall_next(sve_ctx*, const char *moment, const char *context_json, int64_t now)` returns a prompt or `"null"`. `moment` ∈ `note_open|chat|before_event|recap|practice`; `context_json` is `{"note_id"}`, `{"memory_ids":[…]}`, `{"person_ids":[…]}` or `{"day":"YYYY-MM-DD"}`. It obeys `on`, `moment_allowed`, `daily_cap` and `pick_due`, and logs `offered`. The prompt is `{"memory_id","moment","cue","question","partial","gapped","gap_answer","choices":[{"id","text"}],"answer":{"text","sources"},"heard_on","style"}`.
    - `char *sve_recall_set(sve_ctx*, const char *moment /* practice|recap */, int64_t now)`: up to the remaining cap; for recap, the day's memories as cues (recap ignores due dates; it uses the day's recordings).
    - `char *sve_recall_answer(sve_ctx*, int64_t memory_id, const char *moment, int cue, const char *answer, const char *chosen_id, int64_t response_ms, int64_t now, sve_progress_cb, void*)`: grades (model step in Task 20), schedules and logs. It returns `{"outcome","feedback","expected","given","next_due","elaborate":bool}`.
    - `int sve_recall_skip(sve_ctx*, int64_t memory_id, const char *moment, int64_t now)`.
    - `int sve_recall_practise(sve_ctx*, int64_t memory_id, int on)` (pins).
    - `char *sve_recall_settings(sve_ctx*)`; `int sve_recall_set_settings(sve_ctx*, const char *json)`.
    - `char *sve_recall_progress(sve_ctx*, int64_t now)` returns:
      - `{"weeks":[{"start","recalled_without_help","kept"}]×8`;
      - `"curve":[{"days":"1","rate","n"}]` (buckets 1, 2–3, 4–7, 8–14, 15–30, 30+ days since the last attempt);
      - `"promises":{"on_time","due"}`;
      - `"active_days_28"`;
      - `"moments":[{"moment","offered","answered","rate"}]`;
      - `"due_rate_14d"`: correct + 0.5 × partly over attempts with `at` in [due_at − 12 h, due_at + 48 h], for kept memories (the §5.7 gate metric);
      - `"free_rate_14d"`.
  - The recall state is created when `enters_practice` first becomes true (checked in `sve_recall_next`/`set` and after reviews). Leaving practice keeps the history.

- [ ] **Step 1: Write failing tests**:
  - `enters_practice_rules` (wrong never; health new promise not until kept; medicine yes).
  - `daily_cap_defaults_and_adapts` (no data → 3; answering 2 a day → 2; full answers on 5 of 7 days → cap + 1).
  - `ten_days_away_respects_cap`: 40 due, cap 3 → `pick_due` returns 3: the two promises due tomorrow first, then the medicine.
  - `skipped_not_again_within_12h`.
  - `moment_allowed_explores` (8 offers, 0 answers → allowed only at offered 8; with 12 → only at offered % 4 == 0).
  - `adapt_bias_moves`.
  - `practice_hour_moves_to_answers_and_clamps`.
  - `interleave_improvers` (no two from the same note in a row when another exists).
  - `grade_number_mismatch_is_almost` (expected "సోమవారం", answer "Friday" → wrong / almost, `expected == "సోమవారం"`).
  - `grade_missing_field_is_partly`.
  - `grade_no_fields_asks_model`.
  - `choices_only_from_own_memories` (with 1 other memory → `[]`).
  - Store-level: `recall_deleted_with_note` (Review Focus 4).
  - `progress_due_rate`.
- [ ] **Step 2: Run.** Expected: FAIL. **Step 3: Implement.** **Step 4: Run.** Expected: PASS.
- [ ] **Step 5: Commit** `feat(core): recall moments that adapt to the person; kind grading`.

### Task 20: Model parts of recall: questions, meaning check, elaboration (core + model)

**Files:**
- Modify: `core/src/sve_brain.cpp`, `core/src/sve_recall.h/.cpp` (prompt builders, pure), `core/tests/test_recall.cpp`, `core/tests/smoke_gemma.cpp/.sh` (mode `recall`)

**Interfaces:**
- Consumes: Task 19.
- Produces:
  - `std::string recall_question_prompt(const json &row, const std::string &lang)`. Gemma writes one question (greedy, ≤ 40 tokens) when a memory enters practice, stored in `recall.question`. `bool question_ok(const std::string &q, const json &row)` requires that no checked field's tokens appear in the question and that it ends with "?"; otherwise the code template is used.
  - `std::string same_meaning_prompt(const std::string &memory_text, const std::string &answer)`, answered with `parse_yes_no`. In `sve_recall_answer`, for `ask_model`: yes → correct; no → wrong when content overlap < 0.2, else partly; unclear → partly. Unsure grading counts as partly, never as wrong. No model loaded → partly.
  - `char *sve_recall_elaborate(sve_ctx*, int64_t memory_id)` returns `{"question","links":[memory rows]}` or `"null"`. Only for style improve, after a correct answer, at most 1 in 3 correct answers (`elaborate` flag in the answer result). Gemma writes a why/how question from the memory and up to 2 linked memories (person, topic or meaning links); it must pass `question_ok`. Its answer is not scheduled; it is logged with moment `elaborate`.
  - Medicine memories never go through Gemma: errorless cue, code template, code grading.

- [ ] **Step 1: Write failing tests**: `question_ok_rejects_leaked_answer` ("Did you promise the report by Friday?" for the checked "Friday" → false); `medicine_never_asks_model` (grading a medicine memory with no fields gives `partly`, not `ask_model`).
- [ ] **Step 2: Run.** Expected: FAIL. **Step 3: Implement.** **Step 4: Run.** Expected: PASS.
- [ ] **Step 5: Smoke** mode `recall` on the fixture's memories: three questions, three graded answers (right, almost, unrelated), one elaboration. Print each with its outcome. Paste the output.
- [ ] **Step 6: Commit** `feat(core): recall questions and meaning checks with Gemma`.

### Task 21: Recall in the app's moments, recap and practice (iOS)

**Precondition:** the controller shows the owner a mockup of the recall card at each moment and has the layout approved before dispatch (one session for Tasks 21 and 22). The moments are:
- note open, above the summary;
- chat: "want to try first?" for improvers, a cue for support;
- Today "Before you see %@";
- the recap screen;
- the practice screen;
- the first "How should Summaverick help you remember?" card;
- the four cue levels;
- the feedback lines.

**Files:**
- Create: `ios/SummaverickEveryday/Views/Recall/RecallCardView.swift`, `Views/Recall/RecallSessionView.swift` (practice and recap), `Engine/RecallModels.swift`, `Engine/RecallLogic.swift` (pure: notification plan, home ordering), `Engine/CoreEngine+Recall.swift`
- Modify:
  - `Views/NoteDetailView.swift` (note_open card);
  - `Views/Ask/ChatView.swift`: before sending, `sve_recall_next("chat", memory_ids from the answer's M sources of the last reply or a pre-search)`; improvers get "You heard this on %@. Want to try first?" [Try first] [Just answer]; support gets the cue line, then the answer;
  - `Views/Today/TodayView.swift`: "Before you see %@" for people linked to reminders or tasks due today; the style-choice card; home ordering;
  - `Views/Memory/MemoryCardView.swift` ("Practise this");
  - `Engine/ReminderScheduler.swift`: recall notifications use their own prefix `sve.recall.`, and resync never removes them;
  - `App.swift` (notification route to the sessions), `Views/SettingsView.swift`, `Localizable.xcstrings`.

**Interfaces:**
- Consumes: Tasks 19–20; the existing voice input (`AudioRecorder` + transcription as in AskView) for spoken answers.
- Produces:
  - `RecallNotifications.plan(settings: RecallSettings, now: Date, calendar: Calendar) -> [PlannedRequest]`: one daily repeating request each for recap and practice when that one is on, at its time (practice at `practice_hour` from the core); none when off.
  - `HomeOrder.recallFirst(answered: Int, offered: Int) -> Bool`: true when ≥ 50% of the last 10 offers were answered (the home screen leans to what the person uses).
- Behaviour:
  - "Show me" is always visible, as the second button, never locked.
  - Skip ("Not now") calls `sve_recall_skip`.
  - The answer is one tap (choice or gap) or a few words, typed or spoken.
  - Feedback comes from the result keys: "That's right.", "Almost: it was %@, not %@.", "Partly. Here is the whole thing:", "Here is what was said:". Then the memory with ▶.
  - The master switch "Help me remember" (Settings, on by default) gates every in-app moment. Recap (default 20:00) and practice (default 09:00) are separate notification switches, off by default, and use the notification permission reminders already ask for.
  - Off means no prompts at all, anywhere.
- Strings (en/te): every line above, plus "Help me remember", "Daily recap", "Practice", "Show me", "Not now", "Try first", "Just answer", "Practise this", "What do you want help with?", "Remembering what happened", "Learning and keeping what I hear".

- [ ] **Step 1:** swiftc check `recall_check/main.swift` with `RecallLogic.swift` + `RecallModels.swift`:
  - both switches off → `plan` is empty;
  - recap on → one request at 20:00, repeating;
  - practice on with core hour 07:00 → 07:00;
  - `recallFirst(6, 10)` true, `(4, 10)` false;
  - a prompt JSON for each cue decodes.

  Paste the output.
- [ ] **Step 2:** Implement the views and wiring. **Step 3:** Device build, `STRINGS OK`.
- [ ] **Step 4: Commit** `feat(ios): recall woven into notes, chat, Today, recap and practice`.

### Task 22: "Your memory" progress and how-it-asks settings (iOS)

**Precondition:** approved in the Task 21 mockup session.

**Files:**
- Create: `ios/SummaverickEveryday/Views/Recall/YourMemoryView.swift`
- Modify: `Views/Profile/ProfileView.swift` (entry "Your memory"), `Views/SettingsView.swift` (style; "How Summaverick asks": Automatic / Just ask / With hints / Show the answer to complete, mapped to mode `auto|free|cued|errorless`, showing the current automatic level), `Engine/CoreEngine+Recall.swift` (`progress(now:)`), `Localizable.xcstrings`

**Interfaces:**
- Consumes: `sve_recall_progress`, `sve_recall_settings`.
- Behaviour, over 8 weeks:
  - "Remembered without help" per week, against kept memories;
  - "How long memories last for you", the personal curve from `curve` buckets, with "not enough yet" under 5 attempts per bucket;
  - "Promises kept on time";
  - "Days you practised (last 4 weeks)".

  It is encouragement, with no score to fear: no streaks, no red. Charts follow the dataviz guidance (one series each, labelled axes, Telugu labels).
- Strings (en/te): the titles above, plus "Not enough yet", "Automatic", "Just ask", "With hints", "Show the answer to complete", "How Summaverick asks".

- [ ] **Step 1:** swiftc check: progress JSON decodes; buckets with n < 5 show "not enough yet". Paste the output.
- [ ] **Step 2: Implement.** **Step 3:** Device build, `STRINGS OK`. **Step 4: Commit** `feat(ios): Your memory, and a say in how Summaverick asks`.

## Phase F — Device checks

### Task 23: Device checklist and measurements

**Files:**
- Create: `docs/superpowers/2026-11-08-second-brain-device-checks.md`

- [ ] **Step 1:** Write the owner's checklist.
  - Carried from the old Task 14:
    - a 1-hour recording: notes within ~10 minutes of Stop on a cool phone (record the time, with memories now in the section call);
    - chapters jump correctly;
    - the overnight offer appears for a long backlog and its estimate counts memories; "Do it now" works; the task finishes while charging;
    - audio plays after compression and the file is ~10× smaller.
  - Chat:
    - follow-ups keep context; sources open the right moment;
    - Stop works; history persists and deletes;
    - Regenerate on the last answer, Copy on every answer, edit-and-resend the last question;
    - a medicine answer regenerates whole and unchanged;
    - medicine and health questions never get a general answer;
    - Remember this lands in To check.
  - **Notes byte parity on the phone:** on CPU (app in the background) with a non-empty profile (name, routine, a medicine, a preference, a vocabulary word), regenerate notes for a recording that fits one prompt on the base build and on the release build, greedy. Outputs must be identical (ledger requirement).
  - Memories:
    - a real conversation gives a short, correct list;
    - every memory plays its source;
    - Keep / Edit / Wrong work;
    - a health memory waits;
    - deleting a recording removes its memories;
    - a promise shows as one card with its reminder;
    - a same-person question appears for "Rao" and "Dr. Rao".
  - Map: readable in Telugu and English; fold and unfold; every action works; few memories show a list.
  - Recall: prompts appear at note open, in chat, before a person and in the recap and practice when on, and never when off; Skip never repeats at once.
  - **Two-week recall gate (spec §5.7):** after two weeks of the owner's use, read `due_rate_14d` (≥ 0.80 to pass) and the per-moment response rates from "Your memory". Check that the owner did not turn the prompts off. Report both.
  - Release: the owner checks the Gemma terms for EmbeddingGemma (if Task 12 shipped) and uploads nothing else.
- [ ] **Step 2:** Include the measured numbers:
  - smoke timings from Tasks 4, 14 and 20;
  - the accuracy and confidence gate results (Task 8);
  - the meaning gate result (Task 11);
  - the parity results (Tasks 4, 5 and 14).
- [ ] **Step 3: Commit** `docs: device checks for the second brain`.
