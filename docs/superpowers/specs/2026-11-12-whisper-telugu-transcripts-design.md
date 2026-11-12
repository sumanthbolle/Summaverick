# Telugu transcripts with Whisper — design

Status: draft for the owner's approval (2026-11-12).

## Why

The owner, 2026-11-11: "it feels that there is no improvement in transcribing. literally lot of time it is taking
even for small recordings.." Gemma 4 E4B writes the transcript today, 28 s pieces at a time. On the iPhone 16e
it takes about as long as the recording or longer, and it sometimes loops ("కాక, సమా" over and over in note 25).

A spike on the owner's phone (branch `spike/whisper-telugu`, commits 864a2a5…029a2de, not merged) tested
whisper.cpp with a Telugu fine-tune, on note 25 (10 min of Telugu conversation) and note 22 (10 min of English
talk):

| Setting | Phone | 10 min of audio took | Text kept vs Gemma |
|---|---|---|---|
| Gemma 4 E4B (today) | — | about as long as the audio or longer | 100% (3729 chars) |
| Whisper te-medium, 10 s pieces | cooler | 6.2 min | 109% |
| Whisper te-medium, 10 s pieces, encoder sized to the piece | hot | 4.1 min | 90% |
| Whisper te-medium, 16 s pieces, encoder sized to the piece | hot | 3.5 min | 81% |

- "Text kept" leaves out the lines where Whisper looped. Length stands in for quality, so a read-through by the
  owner is part of the device checks below.
- Whisper's Telugu reads cleaner than Gemma's: for example, "కంట్రోల్ లోకల్ లో … ఉండేటోళ్లు" against
  Gemma's "కంట్రల్ లలా లోకలుల లలా … ఉండటోలు".
- Two problems showed up in the spike:
  - English speech comes out in Telugu letters ("ఆల్రైట్ సో దిస్ త్రీ…").
  - On a ticking noise, Whisper loops ("టిక్ టిక్ టిక్…"), in 3–4 lines per 10 minutes.

## Goals

1. The transcript of a 30-minute Telugu class is done within about 12 minutes of Stop on a hot phone with the app
   open, and sooner when the phone is cool.
2. The Telugu is at least as complete as Gemma's and cleaner.
3. English, and English-heavy pieces, still come out in English letters.
4. Everything stays on the phone, as now.

## Non-goals

- Live transcription while recording. It stays off by default (83a7603); this design only changes the work after
  Stop.
- Replacing Gemma for the summary, notes, questions, memories or Ask. Gemma keeps all of those.
- Other Indian languages. The same path takes another fine-tune later.

## Anti-goals

- **Whisper and Gemma in memory at the same time.** Whisper needs about 1.1 GB and Gemma about 4.9 GB, which
  doesn't fit on an 8 GB phone.
- **A transcript worse than today's.** If Whisper fails or isn't downloaded, the note is transcribed by Gemma as
  it is today.
- **AI wording in the app** (the app voice rule).

## Design

### 1. The model

- **The model:** `vasista22/whisper-telugu-medium` (Apache-2.0), converted to ggml and quantized to q8_0, 823 MB.
- **The engine:** whisper.cpp (MIT), built from source against the ggml already inside the app's llama
  xcframework, so there are no two copies of ggml. The spike shows this works.
  - whisper.cpp's files are compiled at -O3 even in Debug, because Debug -O0 is about 10× slower.
  - The ANEForge (Neural Engine) stubs stay, as in the spike.
- **Download:** it's a second `ModelPart` in `ModelManager`, which fetches it after Gemma by itself in the same
  background session.
  - Until it's present, transcripts use Gemma.
  - The setup screen counts it in the download size.
- **Hosting:** see open question 1 below.

### 2. How a recording is transcribed

1. **Pieces at pauses.** The audio is cut into pieces of 4–10 s at the quietest moment, using 20 ms loudness
   smoothed over 300 ms, as in the spike.
2. **Language per piece.** For each piece, Whisper detects the language between Telugu and English
   (`whisper_lang_auto_detect`, compared on those two).
   - Telugu pieces are decoded as `te`.
   - English pieces are marked for Gemma (step 5).
   - **This is unproven:** whether the fine-tune still detects English reliably. It's the first device check.
     If it doesn't, the fallback is the recording's language setting (see open question 2).
3. **Decoding settings:**
   - greedy decoding;
   - `no_timestamps = true` (the fine-tune was trained without them, and with them the first letter breaks);
   - temperature fallback off;
   - `no_context = true`;
   - `audio_ctx` sized to the piece (50 frames per second plus 64, at most 1500).
4. **Cleanup in code:**
   - A run of the same word or short phrase repeated more than 3 times in a row is cut to one. This handles the
     ticking loops.
   - A piece whose text is only such a run is dropped.
5. **English pieces:** after Whisper is unloaded, Gemma transcribes the pieces marked English, as today, with the
   piece's audio. Then Gemma goes on to the summary and notes.
6. **Into the note:** Whisper's pieces are grouped into transcript pieces of about 28 s each.
   - This keeps `TranscriptPiece`, the sections, and `start_sec` working as they do now.
   - A piece's confidence is the mean token probability, so the existing "[unclear]" marking (below 0.80) keeps
     working. The threshold is checked on the phone.

### 3. Memory and order

- **One model at a time.** Whisper loads, transcribes the whole recording, and is freed. Only then does Gemma
  load. The free memory is checked before each load, as for Gemma now.
- **Ask during a transcript:** Ask waits until Whisper finishes its current piece, at most about 10 s. Whisper is
  then freed and Gemma loaded. The transcript continues from the next piece afterwards, and pieces already written
  are kept (the engine v2 rule: saved steps are never redone).
- **A recording starting** pauses Whisper at the next piece boundary, as for Gemma now.

### 4. The background and heat

- **The GPU in the background:** iOS blocks it, so in the background Whisper runs on the CPU.
  - The CPU speed of Whisper medium on the phone isn't measured yet (a device check).
  - If it's slower than the audio, background work keeps Gemma's CPU path, and Whisper runs only in front or on
    the charger overnight.
- **The heat ladder** applies between pieces, as now.

### 5. In the engine v2 step list

- `transcribe_piece` steps get a backend field: `whisper` or `gemma`.
  - Whisper steps run in a batch while Whisper is loaded.
  - Gemma steps (English pieces, and every piece when Whisper is missing) run after it is freed.
- Each Whisper piece's result is saved as it ends, so a cut-off loses at most one piece of about 10 s.

## What the person sees

- Nothing new on screen, except notes arriving sooner.
- Downloads show one total size.
- No new text, so nothing to translate. If a note ever needs to say which way it was transcribed, it doesn't.
  That's an internal detail.

## Device checks (iPhone 16e, never the Mac)

1. **Language detection:** note 22 (English) is detected as English, and note 25 (Telugu) as Telugu, in 95% or
   more of pieces.
2. **A 30-minute Telugu class, app open, warm phone:** the transcript is done within 12 minutes of Stop, and the
   owner reads it and judges it at least as good as Gemma's.
3. **Mixed recording:** English pieces come out in English letters.
4. **Ticking or noise:** no line repeats a word more than 3 times.
5. **Memory:** Whisper and Gemma are never loaded at the same time (logged peak).
6. **Locked phone:** Whisper's CPU speed is measured. The background path follows section 4.
7. **Whisper not downloaded:** the note is transcribed by Gemma as today.

## Order of building

1. whisper.cpp in the core, plus `sve_whisper_transcribe` (pieces at pauses, cleanup, confidence), with C tests
   on a short WAV. The C tests run on the Mac with no model loaded, under the battery rule.
2. The language check per piece, then device check 1. That check decides between auto-detect and the recording's
   language.
3. Pipeline: Whisper first, then English pieces through Gemma, one model at a time. Device checks 2, 3, 4, 5
   and 7.
4. The download part and the setup screen size.
5. The background CPU path, device check 6, and the rule in section 4.

## Calls made in writing this (the builder may revisit; the owner can overrule)

1. **The step list.**
   - The pause cuts are computed in code first, from loudness alone, in well under a second. They're saved with
     the note, and the step list is built from them, one step per Whisper piece.
   - The resume key is (note, piece index) over that saved list, so a cut-off never changes the cuts. Saved
     steps are never redone.
2. **English pieces through Gemma:**
   - Neighbouring English pieces are merged into windows of at most 28 s, cut on piece edges.
   - Each window goes to `sve_transcribe_audio_pieces`'s single-piece call with the previous piece's text as
     `prev_text`.
   - The final transcript is in time order, whichever model wrote each part.
3. **The time goal includes everything:** loading Whisper, all pieces, the swap to Gemma, and the English pieces.
   - 10 s pieces with the piece-sized encoder measured 4.1 min per 10 min hot, which is about 12.3 min for 30.
   - So the gate is 14 min hot for an all-Telugu class, and 12 min warm.
   - 16 s pieces are faster but dropped 10% more text, so 10 s stays.
4. **The swap cost** (loading Gemma after Whisper, and Whisper after an Ask) is logged in device check 2 and
   counted in the gate.
   - If an Ask costs more than 30 s of extra loading, Whisper isn't reloaded for that note's remaining pieces;
     Gemma finishes them.
5. **The core API:**
   - Whisper gets its own context, separate from the Gemma `sve_ctx`:
     - `sve_whisper_ctx *sve_whisper_load(const char *path, int use_gpu)`
     - `sve_whisper_free(sve_whisper_ctx *)`
     - `char *sve_whisper_piece(sve_whisper_ctx *, const char *wav_path, double start_s, double end_s)`, which
       returns JSON `{"lang":"te","text":"…","conf":0.91}`, plus a pure `sve_whisper_cuts(...)` for the pause cuts.
   - In Swift, `CoreEngine` holds it beside the Gemma context, and the one-model-at-a-time rule lives there.
   - The backend follows `StepBackend`: the GPU in front, the CPU behind.
6. **Checking the download:** the size and SHA-256 are fixed in `ModelManager`, and the file is verified before
   its first load. A wrong file is deleted and downloaded again.
7. **Existing notes keep their Gemma transcripts.** There's no automatic redo. A "Write the transcript again"
   option can come later.
8. **Confidence:** the "[unclear]" threshold for Whisper is set from device check 2. Pieces the owner marks as
   wrong while reading set it, and it may differ from Gemma's 0.80.
9. **ggml:** the spike built whisper.cpp against the app's ggml (llama xcframework b12119) and ran on Metal with
   no missing ops. Whisper.cpp is pinned to the spike's version.

## Open questions for the owner

1. **Hosting the converted model (823 MB):** on a Hugging Face account of yours (my suggestion: a public repo with
   the Apache-2.0 notice and credit to vasista22), or converted on the phone from the original files? Converting on
   the phone would mean a 1.5 GB download and a slow first run.
2. **If language detection isn't reliable:** is a per-recording choice (Telugu / English / both) on the Record
   screen fine, or should English always go through Gemma when unsure? Going through Gemma is slower but needs no
   choice from the person.
