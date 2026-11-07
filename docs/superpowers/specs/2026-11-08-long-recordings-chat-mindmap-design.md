# Long recordings, a slimmer model, Mini ChatGPT and mind map — Design

Date: 2026-11-08
Status: draft for approval (builder check done)
Scope: phases 1 and 2 of the roadmap agreed in chat on 2026-11-08. iPhone app (`ios/`) and portable core (`core/`). Builds on branch `feature/personal-assistant` (personal assistant, model auto-download, live transcription).

## 1. Intent

The owner records long sessions (lectures, discussions, hours at a time) in Telugu mixed with English. Today notes for a long recording are slow and the Ask tab answers one question at a time. The owner wants Summaverick to feel like "a proper mini ChatGPT" that also gives NotebookLM-style views of a discussion, all on the phone with Gemma 4 E4B.

Roadmap (agreed): 1) long recordings + slimmer model; 2) Mini ChatGPT + mind map; 3) study mode, "What did I miss?", glossary, notebooks; 4) connections, audio overview. This document covers 1 and 2.

## 2. Goals

1. Long recordings get their notes, chapters and mind map with little work after Stop, because the heavy reading happens section by section during recording and catch-up. Target: a 1-hour recording has notes within about 10 minutes of Stop on a cool phone. A 3-hour recording needs a grouped reduce (groups of sections first, then the whole), so it takes longer; measure and report it rather than promise constant time.
2. Chapters with times for every recording; tap to play from there.
3. Overnight mode: very long recordings can be left to finish while the phone charges.
4. Recordings take about a tenth of the space after transcription, and stay playable.
5. The model download gets smaller: the unused image encoder is removed (safe); the word tables are trimmed to the languages we need if, and only if, output is proven identical.
6. Ask becomes a chat: answers appear as they are written, follow-up questions keep the conversation, past chats are kept and can be reopened or deleted. It answers from recordings with source links, and can also answer general questions.
7. A mind map per recording: main subject in the middle, topics and sub-points around it; tap a point to hear where it was said.

## 3. Non-goals

- Cloud anything, web search, other models (DeepSeek and others were considered and rejected: the strong ones do not fit a phone; the small ones are weaker at Telugu and cannot hear audio).
- Phase 3–4 features (study mode, notebooks, connections, audio overview).
- Fine-tuning Gemma. Editing a mind map by hand.
- Speeding up generation by changing the model's thinking layers (trimming does not change speed; see §5.3).

## 4. Anti-goals

- Wrong information, above all about medicines. All existing medicine safety rules (quote checks, answers built from stored facts, "I didn't hear that in your recordings") apply unchanged to chat. General-knowledge answers must never be presented as coming from the person's recordings, and must not give medical dosing advice.
- A trimmed model that writes Telugu or English differently from the full model.
- Losing a recording or any finished work when the app is closed, the phone gets hot or iOS stops background work (the sidecar progress rule from live transcription extends to sections).
- A chat that sends anything off the phone.

## 5. How it works

### 5.1 Sections (the base for everything else)
- A **section** is about 5 minutes of transcript (a run of consecutive pieces; cut at a piece boundary). As soon as a section's pieces are transcribed — during recording, catch-up, resume or overnight — Gemma writes a **section summary**: a title, 3–6 key points (each with the start second of the piece it came from), names/terms mentioned, and any reminders/tasks hints.
- Section summaries, chapter titles and mind-map labels are written in the notes language (the existing notes-language setting), with names and technical terms kept in the script they were spoken in. Pieces stay indexed in their original language, so search and chat find both. *(Designer's call.)*
- Section summaries are stored in the core database (new table), linked to the note and to piece ranges. Progress is crash-safe: a section is either fully stored or redone.
- **Notes after Stop** (summary, detailed notes, revision questions, title), **chapters** and **the mind map** are built from section summaries, not from the full transcript. For short recordings (one section) behaviour matches today. Existing `reduce_to_fit` stays as the fallback for notes recorded before sections existed (backfill sections on first open, the same way pieces were backfilled).
- Section work uses the same heat rule as transcription (rest at serious, pause at critical) and runs on the CPU in the background, GPU in front when not recording.
- While recording, sections are written only if transcription is not behind by more than one section (transcription has priority).

### 5.2 Chapters
- Built from section titles, merging neighbouring sections on the same topic. Shown at the top of the note: "0:00 Introduction · 12:30 Why China stocks fell · 41:10 Questions". Tapping one plays from that time.

### 5.3 Slimmer model
- Measured on the owner's Mac (unsloth Q4_K_M weights 4.96 GB, ggml-org mmproj Q8_0 0.56 GB): thinking layers 2.59 GB, word tables 2.32 GB (`per_layer_token_embd` 1.94 GB + `token_embd` 0.38 GB, 262k-word vocabulary), image encoder 0.23 GB (unused), audio encoder 0.33 GB.
- **Strip the image encoder** from the projector file. Host the slim projector file where the app downloads from (owner to choose the host; see §7) and update the download spec and size checks. Verify audio transcription output is byte-identical on a fixed set of clips.
- **Language-trimmed vocabulary (experiment, gated; designer's calls on the gate):** test set built from the owner's existing recordings and the Telugu reference clip (at least 20 audio clips, Telugu, English and mixed, plus notes, extraction, chat and a long-notes prompt over many sections); "identical" means byte-for-byte identical greedy output on the whole set; one difference fails the gate. build a script that keeps the tokens needed for Telugu, English, Hindi, digits, punctuation, the chat template and any token the full model emits on a test set; remaps the tokenizer and both word tables. Ship only if, on a fixed test set (Telugu, English, mixed clips; notes, extraction, chat prompts), outputs are identical to the full model with greedy decoding. If not identical, drop it; the image-encoder strip still ships.
- RAM is not the issue (app footprint about 0.9 GB measured, weights are memory-mapped); this is about download size and storage.

### 5.4 Overnight mode and compressed audio
- After Stop, if the expected catch-up is long (more than about 20 minutes of work, estimated from what is left), the note shows "This will take a while. Finish overnight while charging?" Yes: processing stops after the current piece and a `BGProcessingTask` (requires external power, needs no network) continues it while the phone charges. Without Yes it continues as today.
- While waiting, the note shows "Waiting for the charger — finishes overnight" with a "Do it now" button that continues in front. If the overnight task has not run by the next morning (iOS decides when it runs), the note keeps waiting and keeps the button; nothing is lost because progress is saved. *(Designer's call.)*
- Recordings left unfinished at night (any note with work left) are also offered to the overnight task automatically.
- **Owner's decision:** once a note's notes are done (not only the transcript), the WAV is converted to AAC (mono, about 32 kbit/s). The original is deleted only after the AAC file is checked (opens, duration matches within a second). Playback and seeking by time keep working; sidecars are no longer needed by then.

### 5.5 Mini ChatGPT (Ask tab)
- Chat screen: the person's message, Summaverick's answer appearing word by word, with source links ("▶ Dr. Rao visit, 4:12") under answers drawn from recordings. Typed or spoken input (existing). Stop button while answering. Suggested starters stay.
- **Conversations** are stored in the core database; a list of past chats (title from the first question), open to continue, swipe to delete, included in "Delete everything".
- **Follow-ups:** the prompt carries the profile, memory, the retrieved recording material (from pieces and section summaries via FTS, as today) and the recent turns of this conversation, trimmed to fit 8,192 tokens (oldest turns summarised in one line when needed).
- **Two kinds of answers, chosen automatically** *(designer's call)*: if retrieval finds relevant recording material, the answer is from recordings, with sources (existing rules). If nothing relevant is found, Gemma answers from general knowledge, clearly labelled "From Gemma's general knowledge, not your recordings". When both apply, the recordings part comes first with its sources, and any general addition sits under its own label. Recordings are never contradicted silently: if general knowledge differs, it says so ("Your recording says X; generally Y").
- **Health guard** *(follows the existing anti-goal: never medical advice of its own)*: any question about medicines (stored or not), doses, timing, side effects, combining medicines, or symptoms gets no general-knowledge answer. It is answered only from recordings/profile (medicine answers built from stored facts, as today) or with "I didn't hear that in your recordings. Please ask your doctor." Detection errs on the side of blocking: a keyword list in English and Telugu (including common mixed spellings) plus Gemma classifying the question before answering; either one flags it.
- **Medicine answers are not streamed** *(follows the existing rule that medicine answers are built from stored facts)*: they appear whole, after the checks. Only non-medicine answers stream.
- Answers in the profile's language, simple sentences, as today. Any answer can still become a reminder or task via Yes / Edit / No.
- Streaming: the core reports generated text as it goes (progress callback with partial text); the UI updates at most about 10 times a second.

### 5.6 Mind map
- Per recording, built from section summaries: one centre (the recording's subject), 3–7 main topics, each with up to 6 sub-points; each sub-point keeps the start second of its source piece. Stored as JSON with the note; regenerated when notes are regenerated.
- Screen: a "Mind map" view in the note (next to Summary/Notes/Questions). Centre plus branches, pinch to zoom, drag to move, tap a topic to fold/unfold, tap a sub-point to play from its time. Readable in Telugu and English; large touch targets. A mockup is shown to the owner before building.
- Short recordings with too little content show a simple list instead of a sparse map.

## 6. Verify

- Core tests for: section planning from pieces, storing/resuming sections, chapter merging, mind-map JSON validation (bad JSON from Gemma is repaired or retried once, else falls back to a list), conversation storage and prompt trimming, the general-knowledge medicine guard.
- Smoke on the Mac with the real model: a 2-hour long-recording fixture (concatenated clips) produces sections, notes, chapters and a mind map; time after "Stop" measured and reported.
- Trimmed-model gate: the comparison script's report (identical / not identical) is included.
- Device checks with the owner: a 1-hour recording, notes ready within ~10 minutes of Stop on a cool phone; chapters jump correctly; overnight mode finishes while charging; audio still plays after compression; chat follow-ups, sources, stop button, history; mind map readable and tappable.

## 7. Owner decisions

1. **Model host (owner's decision):** the slimmed model files go in a Hugging Face repository under the owner's account. The builder prepares the files and upload steps; the app's download spec points at that repository once the owner has uploaded them. Until then the app keeps the current download.
2. **Audio compression (owner's decision):** compress after notes are done, as in §5.4.

## 8. Left to the builder

Exact section length within 4–6 minutes, prompt wording, database schema, mind map layout algorithm, AAC settings, how streaming text is batched, chat list design within the app's existing style.
