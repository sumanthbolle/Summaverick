# Summaverick Personal Assistant — Design

Date: 2026-11-06
Status: draft for approval
Scope: iPhone app (`ios/`) and the portable core (`core/`). Android and the pocket device come later and must be able to reuse the core work unchanged.

## 1. Intent

Summaverick already turns a recording into a transcript and notes on the phone, with Gemma 4 E4B hearing Telugu, English and mixed speech. This project turns it into a **personal assistant over everything the person has recorded**: it organises notes, suggests reminders and action items, surfaces insights, and talks to the person simply in their own language, using what it knows about them.

Who it is for: students, children and older people (lectures, classes, doctor visits, family discussions). The owner tests on an iPhone 16e with Gemma 4 E4B.

Why: people forget what was said in a lecture or a consultation. The assistant makes sure the important parts (the tablet timing, the assignment date, the key idea) come back at the right time, correctly, without anything leaving the phone.

## 2. Goals

1. **Three-tab app**: Home, Deep dive, Ask Summaverick, with a large Record button in the centre of the tab bar.
2. **Suggestions, never silent actions**: after each recording Gemma proposes reminders, to-dos and profile updates; each needs the person's Yes; Edit lets them fix anything before saving.
3. **Personal profile**: name, preferred language, daily routine, medicines, and how they like to be spoken to.
4. **Learning memory**: Summaverick remembers corrections and confirmed facts and uses them as context on every Gemma run.
5. **Correctness first**: answers come only from the person's recordings, profile and memory, each with a link to its source; otherwise it says it does not know.

## 3. Non-goals (this project)

- Web search or any online lookup.
- Cloud processing of any kind. The only network use stays the model download.
- Retraining or fine-tuning Gemma on the phone. "Learning" means stored memory fed into prompts.
- Android app, pocket recorder, hub. (Keep data and logic in `core/` so they can follow.)
- Spoken (text-to-speech) replies. Text replies only in v1.
- Calendar integration. Reminders are local notifications owned by the app.

## 4. Anti-goals (would make this a failure even if it works)

- Wrong information, above all about medicines. Summaverick must never invent a dose, time or instruction, and never add medical advice of its own; it only repeats what was recorded or what the person entered.
- Creating or changing a reminder, task or profile fact without the person tapping Yes.
- Hidden learning: everything it has learned must be visible and deletable.
- Sending anything off the phone.

## 5. Constraints

- On-device only; works offline after the model is downloaded.
- Gemma 4 E4B Q4_K_M with audio projector, loaded with an 8,192-token context (`ModelSpec.contextTokens`). On iPhone 16e, roughly real-time speed; every extra 1,000 prompt tokens costs a few seconds.
- Personal Apple developer team: no Extended Virtual Addressing entitlement; Increased Memory Limit is available and already enabled.
- iOS 17+, SwiftUI, existing architecture: Swift UI layer → `CoreEngine` actor → C API `core/include/sve_core.h` → `core/src` (llama.cpp + SQLite FTS5 store). New data and AI logic belong in `core/` behind the C API, so Android and the device can reuse them.
- Users include older people and children: large touch targets, plain words, Telugu and English UI strings for everything new.
- Existing Qwen models must keep working for transcript→notes. Assistant features (extraction, insights, Ask across notes) may require Gemma 4; with other models they are hidden or show "needs Gemma 4".

## 6. What the person sees (decided)

Mockups from the session: Home / Deep dive / Ask, with the Record button in the middle of the tab bar.

### 6.1 Home
- Greeting with name and day ("Good morning, Lakshmi · Thursday · 2 things need you today").
- **Waiting for you**: pending suggestions from recent recordings (reminders, to-dos, profile changes) as cards with **Yes / Edit / No**.
- **Reminders** due today and the next few days.
- **To do**: open action items with checkboxes.
- **Insight**: one card at a time (e.g. "Gold came up in 3 lectures; each time the advice was 'may dip further'. Quiz me →").
- An "Ask Summaverick…" bar that jumps to the Ask tab.
- Processing status (existing `StatusBanner`) and model nudge stay visible here.

### 6.2 Deep dive
- Segmented control: **Topics · People · Timeline**.
- Topics and People are assigned by Gemma automatically after each recording (low-risk, so no Yes needed) and can be renamed, merged or reassigned by the person.
- Opening a topic or person shows: a combined summary across its recordings, key terms (with Telugu meaning where relevant, e.g. బంగారం = gold), a revision quiz, open tasks, and the list of recordings.
- Timeline is the existing chronological list with search.
- Each recording opens the existing note screen (`NoteDetailView`).

### 6.3 Ask Summaverick
- Chat in Telugu or English, typed or spoken (spoken input is transcribed by Gemma, same as recordings).
- Answers in the profile's preferred language, in short simple sentences.
- Every factual answer shows its source: "▶ Dr. Rao visit, 4:12", which opens that recording at about that point.
- If nothing recorded covers the question: "I didn't hear that in your recordings."
- Any answer can become a reminder or task through the same Yes / Edit / No card.
- Suggested chips: "Quiz me", "What's due?", "Summarise this week".

### 6.4 First-launch setup and Profile
- A short friendly setup: name, preferred language, routine (wake, meals, classes/work, bed), medicines (name, dose, timing). Every step skippable.
- Editable later in Settings → Profile.
- "What Summaverick has learned" page listing every memory item with edit and delete.

## 7. How it works (decided)

### 7.1 After each recording
The existing pipeline (transcribe → title → summary/notes/questions) gains one more stage, **extract**, run by Gemma on the transcript plus profile:
- reminders (what, when, repeat rule, source moment),
- to-dos (what, who, due date if stated),
- profile updates (e.g. a new or changed medicine),
- topic and people labels,
- vocabulary candidates (names and terms it heard).

Reminders, to-dos and profile updates become **pending suggestions**. Topics/people are applied directly. Nothing becomes a notification until the person taps Yes.

### 7.2 Source links
Gemma transcribes in ~28-second pieces; the core knows each piece's start time. The transcript is stored with those start times so any answer, suggestion or quote can link back to "recording X at about mm:ss" (accuracy: within one piece).

### 7.3 Memory as context on every run
- **Always included** (target ≤ 500 tokens): name, language, routine, medicines, speaking style, vocabulary.
- **Included when relevant**: memory items and note excerpts found by the existing SQLite FTS search using the current question or transcript.
- The same assembled context feeds notes, extraction, insights and Ask.

### 7.4 Learning from corrections
- Yes / Edit / No decisions are recorded. Repeated edits of the same kind (e.g. "after dinner" moved to 9 PM twice) become a suggested preference.
- Fixing a word in a transcript or answer adds it to vocabulary, used in the next transcription prompt and in notes.
- Statements in Ask like "I don't take the evening tablet any more" produce a profile-change suggestion.
- All learned items live in the store, appear on the "What Summaverick has learned" page, and can be edited or deleted.

### 7.5 Reminders
Local notifications (`UNUserNotificationCenter`), scheduled only after Yes. Times are resolved against the routine ("after dinner" → the profile's dinner time). Tapping a notification opens the source recording.

### 7.6 Insights
Generated after each recording and at most once a day from recent notes: recurring topics, upcoming deadlines, and revision prompts that link to the quiz. One insight on Home at a time; the person can dismiss it.

### 7.7 Storage
New tables in the core SQLite store, exposed through the C API (JSON in/out, like the existing note functions): profile, memory items, vocabulary, suggestions, reminders, tasks, topics/people and their note links, transcript pieces with start times, insights. Everything stays in the app's Application Support folder and is removed by "Delete all notes and recordings".

### 7.8 Safety rules in Gemma's instructions
- Use only the provided recordings, profile and memory.
- Never invent names, numbers, doses, times or dates.
- Never give medical advice; only repeat what was recorded or entered.
- If unsure or not found, say so.

## 7.8a Medicine safety in code (owner's decision)

Prompt rules (7.8) are not enough on their own; code enforces them:
- Every dose, time, frequency, duration and medicine name in a proposed reminder or profile change is checked against the exact words of its source transcript piece (or the profile entry it came from). If a value cannot be found there, the suggestion is flagged "Please check" with the mismatched field highlighted, or dropped if nothing matches.
- The Yes card shows the exact quoted words from the recording beside what Gemma understood.
- Ask answers about medicines are built from stored profile and transcript facts (quoted, with source links), not written freely by Gemma.

## 7.8b Reminders from the profile (owner's decision)

Medicines entered in setup or Settings → Profile produce a reminder suggestion straight away (Yes / Edit / No), timed against the routine ("after dinner" → dinner time). The profile stores repeat rules (e.g. twice daily, for 5 days, until a date). These reminders link to the profile entry instead of a recording.

## 7.9 Decisions from review (designer's calls, not yet confirmed by the owner)

- **Languages.** Two separate settings. *Notes language* (default English, keeping important Telugu terms in brackets, as the owner asked) controls notes, summaries and questions; it replaces the old Apple-only "Language of recordings" picker when Gemma is selected. *Profile preferred language* controls how Summaverick talks: Ask answers, suggestion card text, insights, reminder text. The app's own buttons and labels follow the iPhone's language through standard iOS localisation (String Catalog) with English and Telugu, covering new and existing screens.
- **Deleting.** Deleting a note deletes its transcript pieces, pending suggestions, topic/person links and any memory items learned only from it. Confirmed reminders and tasks stay, marked "from a deleted recording". When only the audio has expired, source links and notifications open the transcript at that piece instead of playing audio. The retention setting is unchanged.
- **What needs a Yes.** The same rule everywhere: anything Gemma *proposes* (reminders, to-dos, profile changes, vocabulary candidates, facts to remember) goes through Yes / Edit / No. Anything the person *does themselves* (fixing a word, editing the profile, editing a memory item) applies directly, because their own edit is the Yes. Topic/person labels apply directly (low risk, editable).
- **Order and timing.** After transcription the pipeline runs *extract* first, so a doctor's reminder appears as early as possible, then title, summary, notes, questions. Processing runs while the app is open, as today; background processing is out of scope. Topic and person summaries are generated when first opened and cached until a new recording joins that topic.
- **Whose facts.** One profile per phone. Profile suggestions are only made from statements clearly about the person themselves ("your tablet", "you should"); medicines discussed in general (a pharmacology lecture) or about someone else are not proposed as profile changes. The caregiver case (recording on behalf of a parent) is a later project.

## 8. Left to the builder

- Exact table schemas, C API names and JSON shapes.
- Prompt wording, and whether extraction uses one Gemma call or several; JSON reliability technique (e.g. llama.cpp grammar sampling).
- Relevance ranking details beyond FTS (keyword search is enough for v1).
- Reusing the always-included prompt prefix across runs (KV-cache reuse) as a speed optimisation.
- Visual styling within the existing brand components.
- Order of implementation, as long as each step leaves the app working.
- Migration of existing notes (they can be re-extracted on demand with "Generate notes").
