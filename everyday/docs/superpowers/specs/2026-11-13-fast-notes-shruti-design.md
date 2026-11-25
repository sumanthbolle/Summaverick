# Fast notes: Shruti transcripts and fewer Gemma steps — design

Status: owner approved the direction 2026-11-13 ("Yes" to Shruti as the transcript engine, speed work before web
search); awaiting approval of this document.

## Why

The owner, 2026-11-13: "am worried these notes generation are not upto the mark performance wise. who will wait for
this long".

Note 38 on the owner's iPhone 16e (10.6 min recording, phone warm, app in front):

| Step | Time |
|---|---|
| Transcript (Gemma 4 E4B, CPU, 28 s pieces) | 19 min (10:43 to 11:02) |
| Reminders and tasks (extract) | 1.5 min |
| Sections, title, summary, notes, questions (9 steps) | still running 9 min later |
| Rests from the heat ladder inside that | about 5 min of the first 9 |

About 30 min or more for a 10-minute recording. Earlier notes were worse: 3:43 took about 15 min of transcript,
5:50 took about 37 min plus 13 min of notes.

A spike (branch `spike/shruti-telugu`, 63e5a55 and 9a9a74b, not merged) ran Shruti Telugu on the same phone while
it was already hot (thermal 2), CPU, 4 threads:

| Note | Audio | Shruti took | Times faster than the audio |
|---|---|---|---|
| 25 (Telugu) | 39.7 min | 7.7 min | 5.1 |
| 22 (English) | 26.1 min | 3.3 min | 7.8 |
| 34 | 6.1 min | 1.3 min | 4.6 |

- Load 0.6–0.75 s, about 250 MB of memory over the app's baseline, heat unchanged (2 before, 2 after).
- English comes out in Latin letters. Telugu in Telugu letters, with loanwords (manager, boss, issue) in Latin.
- About the same amount of text as Gemma. Gemma is more exact on terms and punctuated; Shruti has no punctuation and
  mishears some terms ("cement management" for "incident management").
- The owner accepted that trade for speed (2026-11-13).

Whisper was tried first and dropped by the owner (2026-11-13): broken language detection on the Telugu fine-tune,
slow, hot.

## Goals

1. A 10-minute recording has its transcript within about 2 minutes of Stop, and its summary, notes and questions
   within about 8 minutes, on a warm phone with the app in front.
2. A 40-minute class: transcript within about 8 minutes, notes within about 20.
3. The phone does not reach thermal serious from the transcript.
4. Everything stays on the phone. Nothing new leaves it.
5. The person sees no new control. The transcript simply arrives sooner.

## Non-goals

- Gemma fixing Shruti's doubtful stretches. Possible later (the pending-piece machinery from the Whisper build is
  reusable); not in this design.
- SraVaani or any other recognizer.
- Web search in Ask. Its own design follows this one.
- Changing what the notes contain (summary, notes, questions, memories, insight stay as they are).

## Anti-goals

- **A note with no transcript.** If Shruti's files are missing or broken, or Shruti fails on a piece, that piece is
  transcribed by Gemma as today.
- **Worse notes from the faster path.** Merging Gemma steps must not drop content the stepped path keeps (sections
  of long recordings, the cut-off safety of saved steps).
- **AI wording in the app** (app voice rule). No "AI transcript", "model", "Shruti" in the person's view.
- **Medicine answers from the model.** Unchanged: medicine and health answers stay built in code.

## Design

### 1. Shruti in the app

- **Model:** Shruti Telugu sherpa-onnx pack (udgamlabs, Apache-2.0): preprocess.onnx, encode.onnx,
  uncached_decode.onnx, cached_decode.onnx, tokens.txt, about 54 MB, the same files and sha256 as the spike.
- **Delivery (my call):** bundled inside the app, not downloaded. 54 MB is small next to Gemma's download, it works
  on first launch, and no hosting is needed. The Apache-2.0 notice and credit to udgamlabs go in the app's
  acknowledgements.
- **Engine:** sherpa-onnx 2.0.1 iOS framework (k2-fsa, Apache-2.0, onnxruntime static), fetched by
  `ios/get-sherpa-onnx-xcframework.sh` with its pinned sha256, as in the spike. The C API from Swift, as in the
  spike's `ShrutiBench.swift`: Moonshine config, greedy search, 16 kHz, CPU.
- **Threads:** 4 when the heat ladder is at full or eased, 2 at minimal, as Gemma's thread rule does now. The
  builder may tune this from device runs.

### 2. How a recording is transcribed

- After Stop, the recording is cut into pieces of at most 28 s at pauses, with the app's existing pause cuts
  (`sve::plan_group`), as in the spike. No audio is dropped as silence.
- Each piece goes to Shruti; the text and the piece's start second are stored as pieces are now, so sections,
  chapters, the transcript view and Ask keep working unchanged.
- A piece Shruti fails on, or returns empty for while the audio has speech, goes to Gemma after the rest are done.
- The transcript is saved and shown as soon as all pieces are in, before Gemma loads.
- Notes already transcribed are not transcribed again.

### 3. While recording (my call, needs device check)

Gemma's live transcription is off because it heated the phone (83a7603). Shruti is cheap enough to try live:
pieces are transcribed while the recording runs, through the existing live-piece path (`LiveTranscriber`,
`LiveParts`), so at Stop only the last piece is left. It is on only if the device check shows the phone stays below
thermal serious through a 30-minute recording; otherwise it stays off and section 2 does all the work.

### 4. Gemma writes less

The notes stage today: extract, section summaries, title, then summary, notes and questions in saved steps
(9 steps for note 38), then memories and insight.

- **Short recordings (transcript fits one prompt, my call: up to about 15 minutes):** one Gemma pass writes the
  title, summary, notes and questions together, in a fixed format the app splits and stores. Sections still come
  from the transcript pieces for chapters, without a Gemma call per section where the recording has one section.
- **Long recordings:** the stepped path stays (cut-off safety matters more there), but steps that can share one
  prompt are merged where the device runs show no loss.
- Extract (reminders, tasks) stays first and separate: it must finish even if notes are cut off.
- Memories and insight stay after the notes, as now.
- The exact prompt and format are the builder's, checked against the stepped output on notes 22, 25, 34 and 38.

### 5. Heat

- The transcript no longer heats the phone, so Gemma starts cooler and rests less. No change to the heat ladder in
  this design; the device check measures the rests.
- If the rests still take more than a third of the notes time on a warm phone, that is reported to the owner, not
  changed silently.

### 6. What the person sees

- The transcript appears about 2 minutes after Stop for a 10-minute recording.
- The note's progress line and Live Activity show the same stages as now; the transcript stage is just short.
- When the app is not in front, the existing "transcript ready" and "notes ready" notifications fire as now (silent
  at night, owner rule).

## Device checks (iPhone 16e, never the Mac)

1. Notes 22, 25, 34, 38 transcribed by the app's Shruti path: times, heat, memory; the owner reads the transcripts.
2. A new 10-minute recording, end to end: transcript time after Stop, notes time, rests.
3. One-pass notes vs stepped notes on notes 22 and 38: the owner compares.
4. A 30-minute recording with live Shruti on: thermal through the recording, transcript time at Stop.
5. Shruti's files removed by hand: the note is transcribed by Gemma, nothing breaks.

## Order of building

1. sherpa-onnx framework and Shruti files in the app build (from the spike); Shruti transcription behind the
   existing transcript step, with the Gemma fallback.
2. Device check 1 and 2.
3. One-pass notes for short recordings. Device check 3.
4. Live Shruti while recording. Device check 4.
5. Device check 5, docs/ARCHITECTURE.md and the acknowledgements.

## Answers to the builder check (2026-11-13, my calls unless marked)

1. **One-pass cutoff and pass/fail.** The cutoff is the transcript's token count fitting one prompt with room for
   the answer, not minutes; "about 15 minutes" is only the expected size. Pass means the one-pass notes cover the
   same topics and give at least as many questions as the stepped notes, as the owner reads them. If one-pass is
   worse, the stepped path (which stays in the code) is used for every note and the merge is dropped; no flag for
   the person.
2. **The time goals are aims, not hard limits.** If device check 2 misses them, the builder reports the measured
   times and where they went (transcript, Gemma, rests) to the owner. Changing the heat ladder is the owner's
   decision, not done silently.
3. **Shruti text downstream.** No punctuation step: Gemma reads unpunctuated text, and Ask and sections work from
   the stored pieces. Reminders and tasks from extract are suggestion cards the person accepts (the Yes gate), and
   health-flagged memories wait for the person's confirmation (owner rule), so a misheard time or name is caught
   there. Medicine answers come from the person's medicine list, built in code, never from transcripts.
4. **Background.** Required, as for today's transcript: Shruti runs on the CPU, which works with the phone locked
   (measured earlier), under the existing continued-processing task and Live Activity.
5. **Memory.** Shruti is unloaded before Gemma loads and loaded again (under 1 s) when needed. During a live
   recording it stays loaded while Gemma is not.
6. **Old notes** stay as they are; no re-transcribe. Which engine wrote each piece is logged, not stored; no
   schema change.
7. **Live pieces are final.** At Stop only the audio after the last live piece is transcribed.

## Calls made in writing this (the owner can overrule)

- Shruti bundled in the app, not downloaded.
- One-pass notes up to about 15 minutes of recording.
- Live Shruti while recording, only if the heat check passes.
- Threads 4 / 2 by heat level.
- No Gemma fixing of doubtful stretches yet.
