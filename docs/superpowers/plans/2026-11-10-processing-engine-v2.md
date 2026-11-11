# Processing Engine v2 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Notes finish on a locked phone and under any load. They do this through small saved steps, with
several steps decoded together where the phone measures a gain, and batches that report progress through one
notice that updates in place.

**Architecture:**
- Detailed notes and revision questions are written one section at a time (about 5 minutes of transcript each,
  using the existing sections). Each section's result is stored the moment it ends and is assembled in code,
  with a heading per part. A cut-off therefore costs one step at most.
- The Swift scheduler picks the next step by priority, gives way to Ask and recording at every step boundary,
  and reports progress in steps.
- The core gains parallel sequences, used only behind a measured phone gate. Batch notices replace each other
  under one identifier.

**Already built, so not tasks here:**
- Spec §6: section summaries are already written live during recording, by `SectionWriter` into the sidecar.
- Spec §2, the work besides the model: compression and saving already run off the model queue.
- Task 6 checks both on the phone.

**Tech Stack:**
- C++17 core with llama.cpp and SQLite (`core/`); tests via `bash core/tests/run.sh`.
- SwiftUI iOS app (`ios/SummaverickEveryday`); Foundation-only swiftc checks in the scratchpad style already
  used for ModelLogic.

**Spec:** `docs/superpowers/specs/2026-11-10-processing-engine-v2-design.md` (approved 2026-11-10).

## Global Constraints

- **No model on the Mac:** never load or run the model on the Mac (the owner's battery rule). Gates and timing
  are measured on the iPhone 16e (device 00008140-001668892E22801C), and only after the owner says "install".
- **No install, no push:** do not install and do not push without the owner. Commit on `feature/personal-assistant`.
- **Voice:** follow `docs/superpowers/2026-11-09-voice-guide.md`.
  - No AI wording ("AI", "model", "generating").
  - A pause is never called "failed": "Paused. I'll finish this by myself."
  - Every new string goes in `Localizable.xcstrings` with English and Telugu (te). Write the catalog with
    `json.dumps(d, ensure_ascii=False, indent=2) + "\n"`.
- **Batch notice:** counts only, with no note name (owner's choice): "Making notes for %d recordings · %d done",
  ending with "All %d are ready."
- **Notes layout:** notes are assembled part by part with a heading per part (owner's choice).
- **Heat ladder stays in charge:** parallel sequences never raise the level; minimal or paused means N = 1 or 0.
- **Parallel N table:** GPU in front: full 4, eased 2, minimal 1, paused 0. CPU in background: full 2, eased 1,
  minimal 1, paused 0. Ships only behind `kParallelGate` (off until the phone gate passes).
- **Verification for every task:**
  - core 316+ tests pass;
  - the device build `cd ios && xcodebuild -project SummaverickEveryday.xcodeproj -scheme SummaverickEveryday -destination 'id=00008140-001668892E22801C' -derivedDataPath /private/tmp/claude-501/-Users-sumanth-bolle-Downloads-summaverick-everyday-ios/0a2e198b-c172-4f76-84bc-6ed75626fdb9/scratchpad/dd-dev -allowProvisioningUpdates build` succeeds;
  - then `python3 tools/check_strings.py <that path>` prints `STRINGS OK`.

**Spec interpretation (ruling):** the spec's "list of steps" is derived from what is stored, not kept in a new
`jobs` table.
- A step is done when its result row exists: a piece, a section summary, or a section output.
- Attempts are counted in one small table, so "failed 3 times for a real reason" is known.
- Why: the stored results are already the truth. A second table could disagree with them after a kill.

## Review Focus

1. **A section with no speech** (silence or music) must give an empty part that is skipped in the assembly. It
   must not leave a "None" heading or a stuck step. Test in Task 1.
2. **Deleting or regenerating a note while a step runs:** the stored result must be refused, with no orphan
   rows. Test in Task 1 (store) and Task 2 (stale write is ignored).
3. **Sections re-planned after more pieces arrive** (live recording, then Stop): outputs of a section whose range
   changed must be redone, not reused. Test in Task 1 (the key includes `first_piece` and `last_piece`).
4. **The language switch** (te/en) on a regenerate must not mix old-language parts. Test in Task 1 (the key
   includes `lang`).
5. **Two batches started back to back**, for example a second import while the first runs: one notice whose
   total grows, never two notices. Test in Task 4.

---

### Task 1: Section outputs in the core (store, generate one, assemble)

**Files:**
- Create: `core/src/sve_assemble.h/.cpp`, `core/tests/test_assemble.cpp`
- Modify:
  - `core/src/sve_store.h/.cpp`: table and wipe;
  - `core/src/sve_core.cpp`: C wrappers;
  - `core/include/sve_core.h`;
  - `core/tests/run.sh`;
  - `tools/make_xcodeproj.py` (or the repo's generator), if new .cpp files need linking.

**Interfaces:**
- Produces:
  - **Table `section_outputs`:**
    `section_outputs(note_id INTEGER NOT NULL REFERENCES notes(id) ON DELETE CASCADE, idx INTEGER NOT NULL, kind INTEGER NOT NULL, lang TEXT NOT NULL, first_piece INTEGER NOT NULL, last_piece INTEGER NOT NULL, text TEXT NOT NULL, created_at INTEGER NOT NULL, PRIMARY KEY(note_id, idx, kind))`.
    The table is cleared when the note's sections are replaced with different ranges, and by
    `sve_note_clear_transcript`.
  - **Table `step_attempts`:** `step_attempts(note_id, idx, kind, attempts, last_error, PRIMARY KEY(note_id,idx,kind))`,
    with the same cascade.
  - **`char *sve_section_output(sve_ctx*, int64_t note_id, int idx, sve_output_kind kind, const char *lang, sve_progress_cb, void*)`:**
    - It writes detailed notes (`SVE_OUT_DETAILED_NOTES`) or revision questions (`SVE_OUT_REVISION_QUESTIONS`)
      for one section, from that section's pieces, with that section's summary as a guide.
    - It stores the result (an empty string for a section with no speech) and returns it.
    - Output caps: notes 360 tokens, questions 200 tokens. Prompts reuse `detailed_notes_prompt` /
      `revision_questions_prompt` with the line "This is part %d of %d of the recording."
    - It returns NULL on cancel, with no model, or when the note is gone. Nothing is stored then.
    - It increments `step_attempts` before running and records `last_error` on failure.
  - **`char *sve_section_outputs_state(sve_ctx*, int64_t note_id, const char *lang)`:** returns
    `[{"idx","kind","done":bool,"attempts"}]` for every section × {notes, questions}. A row is done only when
    its `lang`, `first_piece` and `last_piece` match the current section.
  - **`std::string assemble_notes(const json &sections, const json &outputs)`** (pure, in sve_assemble):
    - one `## <section title>` heading per section, in order;
    - each section's notes go under its heading; empty parts are skipped;
    - every section's `## Action items` lines are merged into one final `## Action items` (deduplicated;
      `None` only when every part says none, using the existing `says_no_tasks`);
    - the result goes through the existing `finish_output` checks.
  - **`std::string assemble_questions(const json &outputs, int max = 12)`** (pure):
    - questions are numbered 1…n across parts;
    - near-duplicates are dropped: Jaccard ≥ 0.7 on `match_tokens`;
    - parts are taken round-robin, so every part is represented when capped.
  - **C wrappers:** `char *sve_assemble_output(sve_ctx*, int64_t note_id, sve_output_kind kind, const char *lang)`.
    It returns the assembled text, or NULL when any part is missing.

- [ ] **Step 1: Write failing tests** (`core/tests/test_assemble.cpp`, plus store cases):
  - `assemble_notes_heads_each_part_and_merges_action_items`: 2 sections, titled "Collector's role" and
    "Elections".
    - Part 1 has `## Action items\n- [ ] Read Article 243`.
    - Part 2 has `## Action items\n- [ ] No action items were stated`.
    - Assert both headings appear in order, exactly one `## Action items`, and that it contains "Read Article
      243" and not "No action items".
  - `assemble_notes_all_none_gives_None`.
  - `assemble_notes_skips_empty_part`: section 2's text is "", so there is no heading for it.
  - `assemble_questions_dedupes_and_round_robins`: 3 parts × 6 questions, with 1 duplicate across parts.
    Assert 12 numbered questions, no duplicate, and every part present.
  - Store:
    - `section_outputs_done_needs_matching_range_and_lang`: stored with range [0,10) "te", then sections
      re-planned to [0,12), so the state shows done=false; a lang "en" query also shows done=false.
    - `section_outputs_deleted_with_note`.
    - `section_output_refused_for_deleted_note`.
- [ ] **Step 2: Run** `bash core/tests/run.sh`. Expected: the new tests FAIL to compile or fail.
- [ ] **Step 3: Implement** the tables, the pure assembly, `sve_section_output` (the model call follows
  `sve_section_summarize`'s pattern: sampled, then one greedy retry), the state query and the wrapper.
- [ ] **Step 4: Run** `bash core/tests/run.sh`. Expected: all pass (316 + new). Run the device build and
  `STRINGS OK`.
- [ ] **Step 5: Commit** `feat(core): write notes and questions one part at a time and put them together`.

### Task 2: The pipeline writes notes in steps (iOS)

**Files:**
- Modify:
  - `ios/SummaverickEveryday/Engine/ProcessingPipeline.swift`: `generateOutputs`, `writeOutput`, regenerate paths;
  - `Engine/CoreEngine.swift`: wrappers `sectionOutput(noteID:idx:kind:lang:shouldStop:onProgress:)`,
    `sectionOutputsState(noteID:lang:)` and `assembleOutput(noteID:kind:lang:)`;
  - `Engine/ModelLogic.swift`: pure `NoteSteps`;
  - `Views/NoteDetailView.swift`: the missing-part line;
  - `Localizable.xcstrings`.
- Test: a new swiftc check `steps_v2_check` (same harness as `steps_check`).

**Interfaces:**
- Consumes: Task 1's C API.
- Produces: `enum NoteSteps` (pure) with:
  - `static func plan(sections: Int, state: [StepState], hasSummary: Bool) -> [Step]`. The order is:
    1. the short summary, if missing (from the section summaries, the existing `generateFromSections` for
       `.shortSummary`);
    2. then, for each section in order, notes then questions, for the steps not done;
    3. then two assemble steps (no model call).

    `Step = .summary | .part(idx: Int, kind: OutputKind) | .assemble(OutputKind)`.
  - `static func progress(done: Int, total: Int) -> Double`: for `ContinuedWork`, monotonic.
  - `static func gaveUp(_ s: StepState) -> Bool`: true when attempts ≥ 3 and `last_error` is not a cut-off.
- **Behaviour:**
  - The "2 of 3" notice is posted when the summary is stored. "3 of 3" is posted when both assembled outputs are
    stored. `StepCount` counts the summary, notes and questions as parts.
  - **Every note uses sections:** a recording under 5 minutes gets one section after Stop, through the existing
    final plan. Remove the transcript-only notes path for new runs, and keep it only as a fallback when sections
    cannot be planned.
  - **Cut-off** (abort, expiry, GPU refusal): the running step is dropped, nothing is stored, and the next run
    starts at that step.
  - **Real failure, 3 times:** the note shows "I couldn't write part %d. Try again" for that part only. Other parts
    still assemble, with the missing part's heading followed by "(not written yet)".
  - **Regenerate:** clears that note's `section_outputs` for the kind regenerated, then runs the steps.
- [ ] **Step 1: swiftc check `steps_v2_check`:**
  - 3 sections, nothing done: the plan is `[.summary, .part(0,notes), .part(0,questions), .part(1,notes), …,
    .assemble(notes), .assemble(questions)]`.
  - Sections 0–1 done: the plan starts at `.part(2,notes)`.
  - Progress never decreases when done goes up.
  - `gaveUp` is true for 3 attempts with error "decode" and false for 3 cut-offs.

  Paste the output.
- [ ] **Step 2: Implement** the pipeline change and wrappers. A stored result for a note deleted meanwhile is
  ignored (Task 1 refuses it); the run ends quietly.
- [ ] **Step 3:** Device build, `STRINGS OK`. Strings (en/te): "I couldn't write part %d. Try again",
  "(not written yet)", "Part %d of %d".
- [ ] **Step 4: Commit** `feat(ios): notes written in small saved steps, so a cut-off costs one step`.

### Task 3: Scheduler with priorities; Ask and Record go first (iOS)

**Files:**
- Create: `ios/SummaverickEveryday/Engine/StepScheduler.swift` (pure parts plus a thin actor-free wrapper).
- Modify:
  - `Engine/ProcessingPipeline.swift`: `processWaitingNotes`, the notes run loop, and the Ask entry;
  - `Engine/ChatStore.swift` / the Ask call site, to request the model.
- Test: swiftc check `scheduler_check`.

**Interfaces:**
- Consumes: Task 2 `NoteSteps`.
- Produces: `enum StepScheduler` (pure) with
  `static func next(notes: [NoteWork], openNoteID: Int64?, recording: Bool, askWaiting: Bool) -> Pick?`.
  - `NoteWork` is `(id, createdAt, origin: .recording | .imported(batchOrder: Int), steps: [Step])`.
  - `Pick` is `.ask | .step(noteID, Step) | .none` (recording, with no live work for the scheduler).
  - Order:
    1. ask;
    2. the open note's next step;
    3. the newest recording's next step;
    4. the summary steps of all notes, newest first;
    5. the other recordings' steps, newest first;
    6. imports in batch order.
- **Behaviour:**
  - The pipeline asks the scheduler between steps.
  - **When Ask is tapped during a step:** if the step has run under 20 s, it is cancelled and restarted later.
    Otherwise Ask waits for the step to end. Steps are at most about 90 s locked; in front on the GPU they are
    15–30 s.
  - Recording start pauses at the next boundary, as now.
  - The existing heat gate runs before each step.
- [ ] **Step 1: swiftc check `scheduler_check`:**
  - Ask beats everything.
  - The open note beats newer notes.
  - Every note's summary comes before any note's parts.
  - Imports keep batch order and come after recordings.
  - Recording returns `.none`.

  Paste the output.
- [ ] **Step 2: Implement** the scheduler and wire the loop. Log every pick with
  `[sve] step: note %lld %@ (%d of %d)`.
- [ ] **Step 3:** Device build, `STRINGS OK`.
- [ ] **Step 4: Commit** `feat(ios): one queue for all notes; Ask and Record always go first`.

### Task 4: Batch notice and lock-screen batch bar (iOS)

**Files:**
- Modify:
  - `Engine/NotesNotifier.swift`;
  - `Engine/ModelLogic.swift`: pure `BatchNotice`;
  - `Engine/LiveActivityLogic.swift` / RecordingActivities: the batch state;
  - `Engine/ProcessingPipeline.swift`: batch membership;
  - `Localizable.xcstrings`.
- Test: swiftc check `batch_check`.

**Interfaces:**
- Consumes: Task 3 picks; `NotesNotifier.post` (returns Bool).
- Produces: `struct BatchNotice` (pure) with
  `static func text(total: Int, done: Int) -> (title: String, body: String)?`.
  - It returns nil when total < 2.
  - While running: "Making notes for %d recordings" / "%d done".
  - At the end: "All %d are ready." / "Tap to see them."
- **A batch** is every note waiting at once (2 or more). A note added while the batch runs joins it, and the
  total grows.
- **The batch notice** uses one fixed identifier, `batch.current`, so each post replaces the last. Its
  `threadIdentifier` is `batch`. Per-note notices use the same thread, so iOS groups them under it.
- **On the lock screen:** a batch bar with done/total. It ends 10 minutes after "All ready", or when the app is
  opened.
- [ ] **Step 1: swiftc check `batch_check`:**
  - total 1 gives nil;
  - (12,4) gives "Making notes for 12 recordings" / "4 done";
  - (12,12) gives the "All 12 are ready." text;
  - a batch that grows from 3 to 5 keeps one identifier.

  Paste the output.
- [ ] **Step 2: Implement.** Strings in en/te: the four above.
- [ ] **Step 3:** Device build, `STRINGS OK`.
- [ ] **Step 4: Commit** `feat(ios): one notice for a batch of notes, updated in place`.

### Task 5: Parallel sequences in the core, behind a gate

**Files:**
- Modify:
  - `core/src/sve_llm.h/.cpp`: `n_seq_max`, and a multi-sequence decode loop;
  - `core/src/sve_core.cpp`;
  - `core/include/sve_core.h`;
  - `ios/SummaverickEveryday/Engine/ModelLogic.swift`: `ParallelChoice`;
  - `Engine/ProcessingPipeline.swift`: run up to N part steps at once.
- Test: `core/tests/test_parallel.cpp` (pure slot logic); swiftc check `parallel_check`.

**Interfaces:**
- Consumes: Task 1 `sve_section_output` semantics; Task 3 picks.
- Produces:
  - **`int sve_section_outputs_parallel(sve_ctx*, int64_t note_id, const char *steps_json /* [{"idx","kind"}] */, const char *lang, int n_par, sve_progress_cb, void*)`:**
    - It prefills each step's prompt into its own sequence (seq_id 0…n_par-1).
    - It decodes all live sequences in one `llama_decode` per token round.
    - It stores each step's result as it ends, then starts the next waiting step in the freed slot.
    - It returns the number stored, or -2 on cancel (finished ones stay stored).
    - Same caps and retry rule as `sve_section_output`.
  - **Pure `SlotPlan`** (in sve_llm or a new `sve_slots.h`):
    `int slots_that_fit(int64_t free_bytes, int64_t kv_bytes_per_slot, int wanted)` returns min(wanted,
    floor(free / per_slot)), and at least 1.
  - **The context** is created with `n_seq_max = 4` and a KV size for 4 slots × (prompt + cap). When memory is
    short, it falls back to n_seq_max 1 and logs.
  - **Swift `ParallelChoice.n(onGPU: Bool, level: HeatLadder.Level, gate: Bool) -> Int`:** the Global Constraints
    table when the gate is on, else 1 (0 when paused). `kParallelGate = false` until Task 6 records a pass.
- [ ] **Step 1: Write failing tests:**
  - `slots_that_fit_caps_by_memory`: 1 GB free, 300 MB per slot, wanted 4, gives 3; 100 MB free gives 1.
  - swiftc `parallel_check`:
    - gate off gives 1;
    - GPU full with the gate on gives 4;
    - CPU eased with the gate on gives 1;
    - paused gives 0.
- [ ] **Step 2: Run.** Expected: FAIL. **Step 3: Implement.** **Step 4: Run.** Expected: PASS. Device build,
  `STRINGS OK`.
- [ ] **Step 5: Commit** `feat(core): write several parts at once in one batch (off until measured)`.

### Task 6: Device checks and the parallel gate (controller with the owner)

**Files:**
- Create: `docs/superpowers/2026-11-10-engine-v2-device-check.md`.

- [ ] **Step 1:** After the owner says "install", run the spec's six device checks. Record each with its log
  lines: `[sve] step:` picks, stored parts, cut-offs, notice texts, and times from Stop to "2 of 3" and to
  "3 of 3".
- [ ] **Step 2: Parallel gate.** With `SVE_TIMING=1`, regenerate one note's parts with N = 1, 2 and 4, on the GPU
  in front and then locked (CPU). Record the total tokens per second and the thermal state.
  - Pass: at least 30% over N = 1, and no hotter level.
  - If it passes, set `kParallelGate = true` for the cases that passed, in a separate commit.
  - If it fails, the gate stays off and the report says so.
- [ ] **Step 3: Commit** the report (and the gate change if passed).

---

## Addendum (owner approved 2026-11-11: "yes go ahead with all three speed-ups")

The first night on the phone (note 25: 40 minutes, locked) gave 16 parts in 12.5 minutes, about 47 s each, with no
failures. What is left to speed up: the GPU is only used when the app is open, nothing is written during the
recording, and every part pays its own prefill. Task 5 is replaced by Task 9 below, which is the same idea
scoped to the two outputs of one section.

### Task 7: "Finish now" mode (iOS)

- **Files:** create `Views/FinishNowView.swift`. Modify `Views/NoteDetailView.swift` (the button),
  `Views/RecordView.swift` (offer it after Stop), `Engine/ModelLogic.swift` (pure `FinishNow.estimateMinutes`),
  `Localizable.xcstrings`.
- **Offer:** after Stop with the app in front, and on any note whose notes are still being written, offer
  "Finish now · about %d min". It also appears as a notification action, "Finish now", which opens the app into
  the mode.
- **What the mode does:**
  - A full-screen dark view: the note title, "Writing your notes · %d of %d", a bar, and "Keep Summaverick open.
    You can put the phone face-up on the desk."
  - It sets `UIApplication.shared.isIdleTimerDisabled = true` and lowers the screen brightness to 0.1. Both are
    restored on leaving, on finishing, and when the scene goes to the background.
  - The app stays active, so the work runs on the GPU.
- **Ending:**
  - When "3 of 3" is reached, it shows "Your notes are ready", restores brightness, and lets the idle timer
    sleep the screen again.
  - A Leave button, or locking the phone, returns to background work with nothing lost.
- **Heat ladder:** at minimal or paused, the mode says "The phone is warm, so I'm resting" and keeps going at the
  ladder's pace.
- **`FinishNow.estimateMinutes(partsLeft: Int, onGPU: Bool) -> Int`:**
  - GPU 12 s per part, CPU 50 s per part (measured 2026-11-11), rounded up, at least 1. Recalibrate in Task 6.
- **swiftc check `finishnow_check`:**
  - (16 parts, GPU) gives 4;
  - (16, CPU) gives 14;
  - (0, GPU) gives 1;
  - the brightness is restored in every exit path. Test this through a pure `FinishNowSession` state machine:
    enter, finish, leave, background.
- **Commit:** `feat(ios): finish now: keep the app open with a dim screen and the notes finish in minutes`.

### Task 8: Notes during the recording (core + iOS)

- **Core:**
  - `char *sve_section_output_text(sve_ctx*, const char *pieces_json, const char *summary_json, int idx, sve_output_kind kind, const char *lang, sve_progress_cb, void*)`.
    It works like `sve_section_output` but takes the material directly, stores nothing, and omits "of N"
    (the line is "This is part %d of the recording.").
  - `int sve_section_output_put(sve_ctx*, int64_t note_id, int idx, sve_output_kind kind, const char *lang, int first_piece, int last_piece, const char *text, int cut)`.
    It stores an output written earlier, only if the note's section `idx` has the same range. It returns 1 when
    stored, 0 on a range mismatch, and -1 on error.
  - Tests: put with a matching range is stored and done; put with a mismatched range gives 0.
- **iOS:**
  - **When:** while recording with the app in front (`.active`) and the heat level full or eased, after each live
    section summary (`SectionWriter`), write that section's notes and questions with `sve_section_output_text`.
    Keep them in the sidecar (`TranscriptSidecar`, new field `parts: [{idx, kind, lang, first, last, text, cut}]`).
  - **At Stop:** after the note's sections are stored, `put` each sidecar part. Mismatches are dropped and
    written again by the normal steps.
  - **Priority:** live transcription keeps priority. A part is written only when the transcription queue is empty.
    It is cancelled at once when a piece is waiting or the app leaves the front.
  - **Locked:** while recording locked (CPU), no parts are written during the recording.
- **swiftc check:** the sidecar round-trips parts; the rule "parts only when active, heat ≤ eased, and no piece
  waiting" holds.
- **Commit:** `feat: notes written while you record, so only the last minutes remain at Stop`.

### Task 9: Notes and questions of a part written together (core)

- **`int sve_section_outputs_pair(sve_ctx*, int64_t note_id, int idx, const char *lang, sve_progress_cb, void*)`:**
  - It writes and stores both kinds for one section.
  - It prefills the shared prefix once (system, then the section material and guide) into sequence 0, copies it
    to sequence 1 with `llama_memory_seq_cp`, appends each kind's instruction, and then decodes both sequences in
    one `llama_decode` per token round until both end or reach their caps.
  - It returns the number stored (0–2), or -2 on cancel. Finished outputs stay stored.
  - Attempts and failures are counted per kind, as now.
  - The context needs `n_seq_max >= 2`. If the context can't hold two sequences, it falls back to two
    `sve_section_output` calls.
  - The prompt layout changes so the kind-specific instruction comes last. The existing checks
    (`finish_output`, question parsing) are unchanged.
- **iOS:** `writeParts` calls the pair for a section when both kinds are due, and the single call otherwise. A
  `SVE_TIMING` log line prints the seconds per pair against the last single parts.
- **Tests:** the pure parts (prompt split into a shared prefix and a suffix per kind, with the prefix identical
  for both kinds), plus the fallback choice. The speed is measured on the phone in Task 6.
- **Commit:** `feat(core): a part's notes and questions written together, sharing one reading of the part`.

### Order and conflicts

- Task 9 (core) runs now, in parallel with the second-brain Task 17 fix round (iOS Views).
- Then Task 7, then Task 8, because both touch the views and `Localizable.xcstrings`.
