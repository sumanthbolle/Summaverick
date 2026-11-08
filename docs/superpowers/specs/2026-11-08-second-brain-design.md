# Second Brain: memory layer, living mind map, recall — Design

Date: 2026-11-08
Status: draft for approval (builder check done)
Replaces: §5.6 (per-recording mind map) of `2026-11-08-long-recordings-chat-mindmap-design.md`. That document's §5.5 (Mini ChatGPT) stays and is extended here (§5.7). Phase 1 of that document is built and on the owner's phone.

## 1. Intent

Owner, 2026-11-08: position Summaverick as "the first-ever AI assistant that boosts the memory power of whoever uses it". Two kinds of people:
- **People whose memory lets them down** (forgetting what the doctor said, what was promised, who someone is, whether a medicine was taken).
- **People improving their memory** (students, professionals: remember lectures and meetings, learn faster).

The mind map becomes a **secondary brain** that powers action, a catalyst for the person's own memory rather than a replacement. What is captured from recordings and how it is processed "is very very important and should be way accurate". Owner: "let's not make this unfinished product": both groups are served fully, not one now and one later.

The owner's model (pasted 2026-11-08): people do not store a perfect recording; they attend selectively, compress meaning, connect it to what they know, keep a short working buffer and later consolidate what matters. **Keep the raw transcript for fidelity, and build a compact, editable, high-value memory layer above it.**

## 2. Goals

1. **Memory layer.** After every recording, a small set of memories is extracted: facts, decisions, promises (who owes what to whom, by when), questions left open, people, and ideas. Each one is tied to the exact words and time it came from.
2. **Way accurate.** No memory exists without words in the recording that back it. Anything uncertain is shown as uncertain and waits for the person.
3. **Second-brain map.** One living map across all recordings: people, topics, decisions, promises and open questions, linked. Tap any point to hear where it was said, or to act on it.
4. **Action from the map.** From any memory: make a reminder or task (Yes / Edit / No, as today), ask about it in chat, mark it done or wrong, or practise it.
5. **Boost the person's own memory.** Recall practice (try to remember first, then see the answer) for improvers; gentle "here is what happened" recaps and "who is this?" help for people whose memory lets them down. Both groups get both; the home screen leans to what the person uses.
6. **Find by meaning**, not only by exact words, in Telugu, English and mixed speech.

## 3. Non-goals

- Cloud anything, web search, other large models. Speaker identification by voice (not reliable on the phone; "with whom" comes from names spoken and the person's own tags).
- Medical claims: Summaverick helps people remember; it does not diagnose, treat or claim to improve any medical condition. The marketing line "boosts memory power" is the owner's; in-app copy says "helps you remember".
- Editing the map's layout by hand. Sharing memories with other people (later).

## 4. Anti-goals

- **A wrong memory.** A memory the recording does not support, a promise attributed to the wrong person, a wrong date, number, name or medicine. Worse than a missing memory.
- A memory presented as certain when the transcript itself was unclear.
- Memories the person cannot correct or delete. Anything leaving the phone.
- Recall practice that nags (people who struggle with memory must never feel tested or scolded).
- All existing medicine safety rules (quote checks, answers built from stored facts, "I didn't hear that in your recordings", the health guard of the chat design) apply unchanged.

## 5. How it works

The owner's human model mapped to the app. The left column is the owner's; the rest is the design.

| Human | In Summaverick |
|---|---|
| Attention | 28 s pieces cut at silence (exists). New: each piece is marked for "something to remember" signals (names, numbers, dates, promises, decisions, questions) while sections are written. |
| Working memory | Live transcript and previous-piece context (exists). |
| Comprehension | Section summaries every ~5 min (exists). New: each key point is typed as fact / decision / promise / question / idea / person. |
| Episodic memory | Recording, pieces with times, chapters (exists). |
| Semantic memory | New memory layer (§5.2), above the existing `memory`, `topics`, `suggestions` tables. |
| Consolidation | New: after notes are done, memories are merged with existing ones (same person, same fact, updated promise), and contradictions are flagged. |
| Retrieval | Keyword search (exists) plus search by meaning (§5.5). |
| Metacognition | New: every memory has its source quote, time, a confidence level and a review state (§5.3). |

### 5.1 Processing a recording (order after Stop, all on the phone)
1. Transcript pieces (exists, live while recording).
2. Section summaries and chapters (exists). Section key points gain a `type` field (fact, decision, promise, question, idea, person).
3. Notes, summary and questions (exists; unchanged output for recordings that fit one prompt, proven byte-identical).
4. Reminders, tasks and profile suggestions (exists).
5. **New: memory extraction** per section: candidate memories with type, text, who (if a name is spoken), when/due (if said), the quote copied word for word, and its start second.
6. **New: grounding check** (§5.3) on every candidate. Failures are dropped, never shown.
7. **New: consolidation** against existing memories (§5.4).
8. **New: map and meaning index update** (§5.5, §5.6).
Each step is crash-safe and resumable like transcription and sections (sidecar / stored progress); heat and background rules are the same as today.

### 5.2 Memory items
- Types: **fact**, **decision**, **promise** (who, to whom, what, by when), **open question**, **person** (name, how they relate to the owner, where they came up), **idea**. Medicines stay in their own table and rules.
- Each memory: text in the notes language (names and terms in the script spoken), type, quote(s) with recording and second, people involved, date said, due date if any, confidence, review state (`new`, `kept`, `corrected`, `wrong`), importance (see below), first and last seen, and links to related memories.
- **Importance** (designer's call): higher for promises and decisions, for things with a date or number, for people the owner has met more than once, for things the owner kept or practised; lower over time if never used. Used to order the map, recaps and practice. Never used to hide or delete: the person decides.
- The person can open any memory, hear its source, edit its text, mark it wrong (removed from map, recaps and chat), or delete it. Deleting a recording deletes memories that came only from it; memories with other sources keep those.

### 5.3 Accuracy: the grounding check (designer's calls)
- **Quote must exist.** The quote must be found in that recording's transcript (normalised for spaces and punctuation; Telugu compared as written). Not found: dropped.
- **Facts must come from the quote.** Every name, number, date and time in the memory must appear in its quote (the same token check the medicine checks use, extended to all memories). A due date may be computed from a spoken relative date ("next Friday") only against the recording's date, and is shown with the words spoken.
- **Who must be said.** A promise's "who" must be a name or pronoun in or next to the quote; otherwise "who" is left empty, never guessed.
- **Transcript confidence.** The core records how sure the model was of each transcript piece (average token probability while transcribing). Memories from low-confidence pieces are marked "I may have heard this wrong. Listen?" and go to the review queue, not straight to the map.
- **Gemma double-check.** A second short pass asks Gemma whether the quote supports the memory (yes/no). "No" drops it.
- **Review queue.** New memories from the last recording appear as a short list with Keep / Edit / Wrong, the same pattern as Yes / Edit / No today. Memories that passed every check and are high confidence appear on the map at once, marked "new"; the person can still correct them. Medicines and anything health-related always wait for the person.
- **Measured, not assumed.** Before shipping, a test set from the owner's recordings with hand-checked memories: report how many extracted memories are supported (target: at least 95% of shown memories correct) and how many real ones were found. The gate is "shown memories correct"; recall is reported, not gated.

### 5.4 Consolidation
- Same person: names matched across recordings (exact, then spelling variants in Telugu/English via a small transliteration table, then Gemma "same person?" with both contexts). Uncertain matches are asked, never merged silently ("Is Rao from Tuesday the same as Dr. Rao?").
- Same fact said again: merged, with both sources kept and "last said" updated.
- Changed fact or promise (a new date, a different number): the newer one becomes current, the older one is kept as history, and the change is shown ("Was Friday, now Monday, said on 9 Nov").
- Contradictions that cannot be resolved by time are flagged for the person.

### 5.5 Find by meaning
- A small on-device embedding model adds meaning search next to keyword search, for chat retrieval, "related" links and the map. Candidate: EmbeddingGemma 300M (multilingual), about 200 MB quantised, run with the same llama.cpp. **Gate:** on a Telugu/English/mixed test set of questions over the owner's recordings, keyword + meaning together must find the right piece more often than keyword alone. If not, it is not shipped and keyword search stays.
- Downloaded with the model, same Wi-Fi and storage rules. Embeddings are stored in the core database.

### 5.6 The second-brain map
- **One living map** across all recordings, plus a focused view per recording (the old per-recording mind map is this view).
- Centre: the person ("You"), or the recording when opened from a note. Around it: People, Topics, Promises, Decisions, Open questions. Each branch shows its most important items first, with a count of the rest.
- Lines link memories that share a person, a topic, or meaning (from §5.5).
- Tap a point: card with text, quote, recording and time (▶ play), who, date, confidence. Actions: **Remind me / Make a task** (Yes / Edit / No), **Ask about this** (opens chat with it), **Practise this**, **Done** (promises, open questions), **Wrong**, **Edit**.
- Pinch to zoom, drag, tap to fold; large touch targets; readable in Telugu and English. A mockup is shown to the owner before building. Few memories: a simple list instead of a sparse map.

### 5.7 Boosting the person's own memory
- **Try to recall first.** After a recording, an optional "What do you remember?" card: the person says or types what they recall, then sees what they missed and got right (compared against the memories). Never scored harshly; "You remembered 4 of 6. Here are the other 2."
- **Spaced practice** (for improvers, opt-in per recording or topic): kept memories come back as short questions at growing intervals (1, 3, 7, 14, 30 days), adjusted by how well they were recalled. One small daily set, never more than the person chose.
- **Daily recap** (for everyone, opt-in): a short evening or morning card: "Today: you met Dr. Rao; you promised Ravi the report by Friday; one question was left open." Tap to hear.
- **"Who is this?"**: ask by name or by "the doctor I saw last week"; answered from person memories with sources.
- **Before you meet**: when a reminder or calendar-free task mentions a person, the card shows what was said with them last time.
- Home screen (Today) shows, in order: due promises and reminders, the review queue, today's practice (if on), the recap. Each part can be turned off.

### 5.8 Chat (extends the Mini ChatGPT design)
- Chat answers use memories first (with sources), then pieces, then general knowledge (labelled), as already designed. Regenerate the last answer, Copy, and edit-and-resend the last question (owner request). "Remember this" on any answer creates a memory, which goes through the same review.

### 5.9 Builder-check calls (designer's calls, 2026-11-08)
- **Checked fields are copied, not translated.** The memory's display text is in the notes language, but every name, number, date and time it relies on is also stored as the exact words of the quote (like `when_text` today), and the check runs on those. Added: Telugu tables for numbers (0–100, hundreds, thousands, lakh), weekdays, months and relative days (ఈరోజు, రేపు, నిన్న, వచ్చే వారం …), so "Friday" is checked against శుక్రవారం and 10 against పది.
- **Telugu text matching.** Before any comparison, text is Unicode NFC-normalised and zero-width joiners/non-joiners are removed. A name matches a Telugu word that starts with it and adds only a case ending from a fixed list (కి, కు, ని, ను, తో, గారు, గారికి, లో, ది, …). Tested with real suffixed names from the owner's recordings.
- **Extraction reads the raw pieces** of a section (with [t=SEC] markers), not the summary. A quote may run across two neighbouring pieces; it is matched against the section's joined text.
- **Cost and timing.** Memory extraction is folded into the existing section-summary call (one model pass per section, extra JSON fields; the summary part's wording is unchanged), so it adds little to processing. The yes/no double-check runs only for candidates with a name, number, date or promise. Order after Stop: notes first (unchanged target), then memories, which may finish in the background or overnight; the "finish overnight?" estimate counts them. During recording, memories are written with sections, under the same "transcription has priority" rule.
- **Transcript confidence** is one signal, built cheaply (token probabilities in the transcription loop, a column on pieces). It is kept only if, on the test set, low confidence actually picks out wrong transcript lines; otherwise dropped. Recordings from before have no value and get the normal checks without a "may have heard wrong" mark.
- **Old recordings** get memories too, in the background or overnight, newest first.
- **One card per thing.** A promise that also produces a task or reminder suggestion is shown as one card (the memory, with its Remind me / Make a task action already proposed). The existing `suggestions` flow stays the action mechanism; the existing `memory` table (facts, preferences, vocabulary) is migrated into the new layer.
- **Health-related** = mentions a stored medicine, any word from the chat health-guard list (English and Telugu), or doctor, hospital, symptom, test result or dose words. These always wait for review.
- **Notifications.** The recap and practice are opt-in, each with a time (recap default 8 pm, practice default 9 am), using the notification permission reminders already ask for. Spoken recall answers use the existing voice input.

## 6. Verify
- Core tests: grounding check (quote found / not found, numbers, dates, names, Telugu text), consolidation (same person, update, contradiction), importance ordering, spaced-practice scheduling, memory deletion when a recording is deleted.
- Accuracy report on the owner's recordings (§5.3): shown-memory correctness ≥ 95%, recall reported.
- Meaning-search gate report (§5.5).
- Device checks with the owner: a real conversation gives a short correct memory list; every memory plays its source; the map is readable and every action works; recall practice and recap appear when turned on and never when off.

## 7. Owner decisions
1. Serve both groups fully (2026-11-08: "let's not make this unfinished product").
2. Accuracy over quantity: a missing memory is acceptable, a wrong one is not (from "should be way accurate").
3. **Accuracy test set (owner, 2026-11-08):** the builder extracts about 100 memories from the owner's real recordings and shows each next to its quote; the owner marks each right or wrong (about 30-40 minutes). The 95% gate is measured on that set.
4. **Meaning model (owner, 2026-11-08):** add EmbeddingGemma (about 200 MB, Wi-Fi download with the main model), shipped only if it passes the §5.5 gate. The owner checks the Gemma terms before release.

## 8. Left to the builder
Prompt wording, schema, how confidence is computed from token probabilities and its threshold (measured on the owner's recordings), map layout algorithm, practice question wording, the transliteration table's contents, batching of the background work.
