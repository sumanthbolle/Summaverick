# Shared core for a second brain — design

Status: written 2026-11-14 from the owner's description, builder check answered; awaiting the owner's approval.

## Why

The owner, 2026-11-13/14: Summaverick should be "a secondary brain for everyone, their own brain child". Voice is
the only way in today. Tomorrow someone may not record at all and instead "note on their own, it could be drawing
or writing with a pen or simply writing them as a plain note or whiteboard", import discussions as voice or text
documents, and "describe situation to 'ask summaverick' and that should do the job". "Summaverick is fully local
and private"; "summaverick for everyday and everyone".

Their example: starting UPSC preparation, they write a notebook and a checklist, and ask Summaverick to keep
reminding them, draft the notes and track the checklist items.

On structure: "core building components must be shared always… must be properly architected, because this will
be used as run-time easy RAG for ask summaverick to reply instantly." Every new note must be mapped into the
knowledge graph, people and the app's structure.

On speed: "speed has no limits, fast is good always and at the same time output also should be that good." The
owner judges by measured time on the phone (memory: performance is the measure). Note 41 (107-minute recording,
2026-11-13) showed the cost of today's order: transcript ready at 44 min, then reminders and tasks (extract) ran
47 min and found nothing, and iOS cut processing off at 99 min with no summary written.

The owner also asked for a light, fast model for raw input and a stronger one for notes and the graph. gpt-oss-20b
was reviewed and cannot run on the phone (needs 14 GB; the iPhone 16e has 8 GB, the app about 5–6 GB).

## Goals

1. One kind of note, whatever its source: recording, whiteboard, imported text document, typed note. Every note
   goes through the same builder and lands in the same list, note screen, Ask and map.
2. The builder writes, for every note: clear notes (title, summary, notes), a checklist when the source has one,
   suggested reminders and tasks (waiting for the person's Yes), people, facts, decisions, promises, questions,
   topics, and the note's place in a topic hierarchy.
3. Everything Ask needs is built when a note arrives (the index), so Ask does no heavy work before answering:
   keyword and meaning search over notes, sections, pieces and memories, plus the graph's links.
4. Whiteboard: the person writes or draws on the phone screen with a finger; Summaverick turns it into clear notes
   in the background and notifies them when the notes are ready to look over (owner approved the four-screen flow,
   mockup `.superpowers/brainstorm/…/whiteboard-flow.html`: write, done, notified, look over).
5. The summary never waits behind reminders, tasks, sections or memories.
6. Fast, with output as good as now. Measured targets (my call; the owner gave no number):
   - Ask: first words within 2 s of sending, on a phone with the app in front.
   - Whiteboard: clear notes ready within 1 minute of Done.
   - Typed note or imported document up to about 3,000 words: clear notes within 1 minute.
   - Recording: the agreed target stands, a 10-minute recording has its summary within 2 minutes of Stop.
   "As good" is judged by the owner side by side with today's output on the same notes.
7. Fully on the phone. Nothing leaves it.

## Non-goals

- Telugu or other scripts on the whiteboard. English first; other languages later (owner).
- Cloud models, or any model that needs a computer (gpt-oss).
- A new look. Screens follow the existing look and voice rules.
- Web search in Ask (its own design, parked).
- Live sync with other apps or a pen device.

## Anti-goals

- **A board, document or typed note lost** because processing failed or iOS cut it off. The source is saved first,
  always.
- **Reminders or tasks set without the person's Yes.** The Yes gate stays for every source.
- **Health memories reaching Ask or the map unconfirmed**, and medicine answers from the model. Unchanged.
- **Ask slower or worse than today** for recordings while the core changes.
- **Two builders**: one path for recordings and another for everything else. That is the thing to avoid.
- **AI wording** in the person's view.

## Design

### 1. One note

- A note gets a source kind: `recording`, `board`, `document`, `typed`. Existing notes are `recording`.
- Every note has text pieces. For recordings a piece has a start second, as now. For other sources a piece has a
  position instead (board number, paragraph or page); the start second is empty. Chapters, "jump to moment" and
  memory sources use the start second when there is one and the position otherwise.
- The original is kept with the note: the audio, the board drawing (PencilKit data and an image), the document
  file or the typed text.
- Existing recordings, their pieces, sections and memories stay as they are; only the source kind is added.

### 2. Fast readers, one per source

Each source has a fast first step that turns it into text pieces. Only this step differs by source.

| Source | Reader |
|---|---|
| Recording | Shruti, Gemma for the pieces it leaves (as now) |
| Board | Apple's on-device handwriting reader (Vision text recognition, English) |
| Document | the text itself (plain text and PDF text first; builder may add others) |
| Typed | the text itself |

Drawings on a board (boxes, arrows) are kept as the image; only the text is read. Reading diagrams with Gemma's
image input is a later step, not this design.

### 3. One builder, in this order

1. **Clear notes:** title, summary, notes, questions (one pass when the text fits, saved steps when not, as now),
   plus a checklist when the source has list-like lines. The person is notified here: "ready to look over".
2. **Index:** keyword index (as now) and meaning vectors for the note's pieces, sections, summary and notes.
3. **Reminders and tasks:** suggestions waiting for Yes (today's extract), after the notes.
4. **People, memories, topics and hierarchy:** memories and people as now; each topic gets a parent topic, so the
   map and Ask can follow UPSC > Polity > Laxmikanth.
5. **Insight**, last.

Every step is saved as it finishes, so a cut-off continues where it stopped (as now). Sections stay for long
recordings.

### 4. The index and Ask

- The meaning model already in the core (EmbeddingGemma 300M, CPU, 256 dims; never loaded on iOS today) is turned
  on and fed by the builder at step 2. Vectors are kept for pieces, sections, summaries and memories.
- Ask combines keyword and meaning search (the core's existing blend, reciprocal rank fusion), adds the matching
  memories and the graph's neighbours of the matched topics and people, and writes the answer from that.
- Ask never waits for the builder: a note whose index is not built yet is found by keywords only.
- Old notes get their vectors in the idle and overnight backfill.

### 5. The faster model

- The builder's text steps need a model faster than Gemma 4 E4B on the phone. Candidates, measured on the iPhone
  16e before any is adopted: Apple's on-device model (Foundation Models; English, needs Apple Intelligence on) and
  one or two small open models of about 1–4B parameters that run in the app's llama.cpp.
- The measure: the same notes (22, 38, a board, a document) through each, time and the owner's side-by-side read.
- Gemma 4 E4B stays for Telugu, for audio pieces Shruti leaves, and wherever the faster model is not good enough.
  Which model writes which step is decided from the measurements and brought to the owner.

### 6. What the person sees

- The whiteboard flow as in the mockup.
- One list of notes, with a small mark for the source. The note screen shows the same parts for every source; the
  original (audio, board, document) is one tap away.
- Import of text documents from Files and the share sheet.
- Notifications as now: "ready to look over", silent at night.

## Order of building (each its own plan, measured on the phone before the next)

1. Builder order: summary first, extract after (goal 5). Measure on a 10-minute recording.
2. One note and the index: source kind, pieces without seconds, meaning search on, Ask combined search.
3. Faster model: measure the candidates, adopt per step.
4. Whiteboard.
5. Document import and typed notes.
6. Topic hierarchy in the map.

## Answers to the builder check (2026-11-14, my calls unless marked)

1. **Background.** The owner wants the work to happen in the background and a notification when notes are ready
   (2026-11-14), and a fix that only works with the app in front is not a fix for the person (owner, 2026-11-13).
   Every target is measured both with the app in front and with the phone locked. Background work never loses a
   note: it continues in the overnight task when iOS cuts it off. Background targets (owner, 2026-11-14, "go with B and B"): with the phone locked, three times the in-front
   targets: board notes within 3 minutes of Done, a 10-minute recording's summary within 6 minutes of Stop.
   Both are measured and reported.
2. **Apple's model and the core.** Prompts and parsing stay in `core/`. A faster model is a backend behind the
   core's generate call: llama.cpp models inside the core, Apple's model through a Swift callback the core calls.
   One builder, one set of prompts. Whether Apple's model runs in the background is part of the measurement.
3. **Summary first on long recordings.** Goal 5 means the summary never waits behind reminders, tasks, memories or
   insight. Long recordings still build the summary from sections (cut-off safety); the faster model is what
   shortens that wait.
4. **Checklist (owner, 2026-11-14: option B).** Live tick boxes inside the note. Items are stored as tasks (the
   existing tasks list), tied to their note; the person ticks them in the note, and one Yes sets a reminder for the
   open ones. Ask knows which items are open. Built so a later step can show open items from all notes on Today
   and let later notes or Ask tick items after a Yes (option C), without a rebuild; that step is not in this
   design.
5. **Edits after Done.** Editing a board, typed note or document runs the builder again on that note. Suggestions
   already accepted or turned down are kept and not offered again; confirmed memories are kept; vectors for the
   changed pieces are rebuilt.
6. **Ask's 2 s.** Measured with the models already loaded. The meaning model (300M) stays loaded while Ask is open.
   Ask uses the faster model when one is adopted, Gemma otherwise; the first question after a cold start is
   reported separately.
7. **Health and medicine from boards and documents.** Medicine changes found there are suggestions waiting for Yes,
   as from recordings, showing the board image or the document line they came from. Nothing is added to the
   medicine list without Yes; medicine answers stay built in code. Health memories wait for confirmation, as now.
8. **Topic tree.** Shared across notes: the builder places each topic under a parent, reusing existing topics. The
   person can move, rename and merge topics, as with people today.
9. **Model files.** Hosted like the earlier choice for Whisper (owner, 2026-11-12): on the owner's Hugging Face,
   credited and with the original license. The repo name is the owner's to give before release; plans use a
   placeholder until then.

## Left to the builder

- Exact schema changes and their migration.
- How checklist lines are recognised.
- The vector store layout (brute force is fine at phone sizes; the builder may add more).
- Which document formats beyond plain text and PDF.

## Calls made in writing this (the owner can overrule)

- The speed targets in goal 6.
- The builder order: notes first, reminders and tasks after.
- English handwriting through Apple's reader, not Gemma.
- Gemma stays as the fallback and for Telugu.
- The order of building, starting with the builder order because note 41 showed it is the largest wait.
