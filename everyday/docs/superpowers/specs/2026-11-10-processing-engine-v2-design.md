# Processing engine v2: small saved steps, parallel work, batches, clear notices — design

Status: approved by the owner 2026-11-10 ("yes count should be fine with a heading..").

## Why

The owner, 2026-11-10:
- "why this is taking so long.. experience wise this is not a very good one, we might need to rework on certain
  logics.."
- "implement parallel and async processing -> this should work seamlessly regardless of load.. implement batch
  wise processing with clear notifications of processing".

The same day, note 24 (a 7.7-minute recording) showed the cause on the phone.
- **Symptom:** its notes never finished on the locked phone.
- **Cause:** each run of iOS background time (about 5 minutes) restarted the whole notes call from the beginning,
  and on 2 CPU cores that call needed more than 5 minutes.
- **The same notes with the app open:** the GPU wrote them in 2.8 minutes, and the summary in 24 seconds.

Imports (the import design, 2026-11-10) will bring batches of 10+ recordings, so the queue has to scale.

## Goals

1. **No lost work.** Every model call is a small step of 60–90 seconds at most on the slowest path (locked, 2
   cores). Its result is saved the moment it ends. Any background time, however short, moves the note forward.
2. **Parallel where the phone allows it:**
   - Several steps decode together in one model batch (parallel sequences), since generation on the phone is
     limited by memory speed and extra sequences cost little.
   - Work that isn't the model runs at the same time as model steps: audio conversion, compression, indexing,
     parsing memories and saving.
3. **Batches:** many recordings or imports run as one batch, with one clear notice that updates in place,
   "4 of 12 done", plus a notice per note as each one is ready.
4. **Any load:** recording, Ask, a hot phone, a low battery, a locked screen and an app kill all slow the work
   down but never break it or lose anything.

## Non-goals

- Two copies of the model in memory: the phone has 8 GB and Gemma needs about 4.9 GB.
- Cloud help.

## Anti-goals

- Fans-on heat: parallel sequences never override the heat ladder.
- A wall of notifications: one batch notice updates in place, and note notices group under it.
- Ask waiting minutes behind a batch: Ask goes first between steps.

## Design

### 1. Work as a saved list of steps (SQLite, in the core)

- A table `jobs(id, note_id, kind, part, idx, input_ref, status, attempts, result_ref, cost_estimate,
  created_at, updated_at)`.
  - `kind` is one of: transcribe_piece, section_summary, notes_section, questions_section, short_summary,
    title, memories_section, embed.
  - `status` is one of: waiting, running, done, failed_real.
- **A note becomes a graph of steps:**
  - transcript pieces (about 28 s of sound each, as now);
  - then sections of about 3–5 minutes of transcript;
  - per section: a section summary, its notes, its questions and its memories;
  - then the short summary (from the section summaries), which is assembled first so "2 of 3" comes early;
  - then the detailed notes, assembled in code from the sections' notes with no extra model call;
  - then the revision questions, also assembled in code.
- **Restart rule:** a step that is cut off restarts by itself, and only that step. Saved steps are never redone.
  A step that fails 3 times for a real reason is marked so, and the note shows exactly what is missing, with
  "Try again".

### 2. Scheduler (Swift, on top of the core)

- **Priority, highest first:**
  1. live transcription while recording;
  2. Ask;
  3. the note open on screen;
  4. the newest recording;
  5. the summary steps of every note;
  6. other notes' steps;
  7. imports in their batch order;
  8. memories;
  9. meaning search indexing.
- **What runs together:** the scheduler picks up to N steps to decode together. N comes from where the work runs
  and from the heat level:

  | Where the work runs | full | eased | minimal | paused |
  |---|---|---|---|---|
  | GPU, app open | 4 | 2 | 1 | 0 |
  | CPU, app in the background (2 cores) | 2 | 1 | 1 | 0 |

  These numbers are measured on the iPhone 16e before shipping and may change.
- **Work besides the model** (conversion, compression, saving, notifications) runs at the same time on its own
  queue and never waits on the model.
- **Recording start:** starting a recording pauses model steps at the next step boundary, so nothing is lost.
  Live transcription keeps its share.

### 3. Parallel sequences in the core

- `Llm` gets `n_seq_max = 4`. Each step runs as its own sequence with its own KV slot: prefill once, then decode
  all running sequences together in one `llama_decode` per token round. A sequence that ends frees its slot for
  the next waiting step.
- **Memory check:** before raising N, the core checks the free memory against each slot's KV size. A smaller N
  is always safe.
- **Gate (measured on the phone, never the Mac):** the total tokens per second with N=2 and N=4, against N=1,
  on GPU and on CPU. Parallel sequences ship only where they beat N=1 by 30% or more without going hotter than
  N=1. Otherwise N stays 1, and the small saved steps still fix the lost-work problem.

### 4. Background time

- A continued-processing task is asked for at Record, as now.
- **iOS cuts the time off:** the running step is dropped, and its sequence restarts on the next run.
- **The overnight task** runs through the same list of steps, so a cut-off costs at most one step (about 90 s).
- **Progress** is reported in steps done out of steps total, so iOS's own progress bar moves smoothly.

### 5. Notifications

- **Per note,** as now, but sooner:
  - "1 of 3 ready · transcript";
  - "2 of 3 · summary";
  - "3 of 3 · notes ready".

  The summary arrives soon after the transcript, because section summaries run as soon as their pieces exist,
  while the recording is still going.
- **Per batch** (2 or more notes, for example an import), one notice updates in place under the same thread:
  - "Making notes for 12 recordings · 4 done" (counts only, owner's choice);
  - at the end: "All 12 are ready. Tap to see them."
- **On the lock screen:** one Live Activity for the batch, showing a bar, the counts and the current note.
- **A pause** is never shown as "failed": "Paused. I'll finish this by myself" (with the reason when known:
  phone hot, low battery, waiting for charger).
- **A real failure** names the part and offers "Try again". The other parts still show.

### 6. During recording

- Transcript pieces are already written live. Section summaries now also run during recording, at a low
  priority: 1 sequence, with the heat ladder in charge.
- When the person taps Stop, only the last section and the short summary remain. The target is "2 of 3" within
  1 minute of Stop for a 30-minute class with the app open.

## How we know it works (device checks)

1. **A 7.7-minute recording, locked from Record to the end:** all 3 parts arrive with no app open, using only
   background time.
2. **A 30-minute recording, app open:** "2 of 3" arrives within 1 minute of Stop, and "3 of 3" within 4 minutes.
3. **A batch of 10 imports, half an hour of sound each:** one batch notice that updates; every note done
   overnight on the charger; no repeated steps in the log.
4. **The app killed mid-step:** on the next run, only that step repeats.
5. **Ask during a batch:** the answer starts within 10 seconds.
6. **The heat ladder:** at minimal, N drops to 1, and nothing fails.

## Order of building

1. The list of steps and the section-wise notes, with assembly in code (this fixes the lost work; N=1).
2. The scheduler with priorities, batches and batch notices.
3. Section summaries during recording.
4. Parallel sequences in the core, behind the phone gate.

## Owner's answers (2026-11-10)

1. Notes assembled section by section, with a heading per part of the class: yes.
2. Batch notice: counts only, no note name.
