# Notes accuracy (Task N7)

The owner flagged note 19 ("District Collectors and Aspirants' Dreams in Service"). The audit found that the
notes step reversed and changed statements and invented an action item. Another note's saved vocabulary
("S&P 500 index", "IPO", "AI bubble", accepted from note 8) leaked into the section terms and hints. Sections
were capped at 3 to 6 points, so stretches of the recording were skipped. ASR garbles fast code-mixed speech.
Commit `c0c87e1` changes the core as follows.

## What changed

| | Before (2095125) | After |
|---|---|---|
| A. Vocabulary | The same list everywhere: ASR "Spell these words this way: …", sections/memories the same line, notes system prompt "Words to spell this way: …" | ASR line unchanged (hearing help). Section, memory and notes prompts: "Spell these words this way (not words of this recording: use one only if the transcript says it; never add it as a topic, term, hint or point): …" (`spelling_vocab_line`, `kVocabOnlyIfSaid`). The learning feature is untouched. |
| B. Notes and questions | "Use only what is in the text." | Added rules: skip unclear or garbled passages, do not guess; an action item only when a speaker asks for it or commits to it; keep who said what (a host's question is not the guest's claim); never reverse a statement or drop a negation; keep numbers, names and terms exactly as in the transcript. Output language rules unchanged. |
| C. Low-confidence pieces | One greedy hearing | A piece with conf < 0.80 (`sve_should_retry_piece`) is heard once more with the same samples, prompt and previous text, and the higher conf is kept. Log: `piece %d conf %.2f: transcribed again (%.2f)`. In `sve_transcribe_next_piece` the `%d` is the piece's start second (that call has no piece index); in `sve_transcribe_audio_pieces` it is the piece number. |
| D. Sections | "3 to 6 points", parser cap 6, output cap 512 tokens | "Cover every part, in order, to the last line: about one point for each [t=SEC] line, up to 10 points, each one short sentence." and "Skip anything unclear or garbled instead of guessing what it means; fewer points are fine then." Parser cap 10, output cap 1024 tokens. |

**C deviates from the brief: the second hearing is sampled, not greedy.** Transcription is already greedy. A greedy
repeat with the same samples and the same context is deterministic: piece 25 of note 19, heard twice on the Mac
CPU, gave identical text and conf 0.8160 both times. A literal greedy retry would double the cost of every
low-confidence piece and could never change anything. The second hearing therefore uses the transcription's
light repetition penalty plus top-p 0.9, min-p 0.05, temperature 0.3 and a fixed seed (42). This sampler is
deterministic and ignores `g_sve_force_greedy`. The higher conf still decides.

## Method

- The harness is in scratch: `q1/n7/harness/driver_n7.cpp`, built against `core/src` of either tree with
  `build.sh`. It runs the app's exact core calls and source rule (`ProcessingPipeline.generateOutputs`):
  1. `sve_plan_sections` over the stored pieces (end = duration, final).
  2. `sve_section_summarize` per range, stored with `sve_note_set_sections`.
  3. `sve_transcript_fits`, which picks the source: transcript, or sections when it does not fit.
  4. Title: `sve_title_from_sections` or `sve_suggest_title`.
  5. The three outputs: `sve_generate_from_sections` or `sve_generate`, lang `en` (the app's default notes
     language; the notes are stored as `en`).
- Every model call's exact prompt and output is detokenized from the core's token log into `prompts.txt`.
- Data: a `.backup` copy of the phone DB (`q1/notes.sqlite` → `q1/n7/base.sqlite`). Each run works on its own
  copy, which is deleted afterwards. The vocabulary rows (memory ids 2–4) are in the copy, as on the phone.
  The phone and repo DBs were not touched.
- Model: E4B Q4_K_M, n_ctx 8192, CPU, 4 decode threads / 8 batch threads.
- Sampling: the app's settings. Notes use seed 42; sections are sampled first, then greedy on retry. Each set
  also ran fully greedy (`SVE_GREEDY`) for a like-for-like diff.
- Notes 19 and 16 fit one prompt, so their notes come from the transcript. Note 17 (81 min) does not, so its
  notes and title come from its sections.
- Outputs: `q1/n7/{before,after}/{sampled,greedy}/note{19,16,17}/` with `sections.txt`/`.json`, `title.txt`,
  `summary.txt`, `notes.txt`, `questions.txt`, `prompts.txt`, `source.txt` and `timing.json`.
- Section-prompt iterations on note 19's three windows (sections only): `q1/n7/iter2..iter4`. The committed
  prompt is iteration 4.

Note 17 is about finance: its transcript says "S&P" once and "IPO" may appear. "AI bubble" is never said in
notes 16, 17 or 19.

## Smoke check A (note 19's three section windows, vocabulary present)

| run | "S&P 500 index" / "IPO" / "AI bubble" in sections |
|---|---|
| before, sampled | section 0 terms: "S&P 500 index" |
| before, greedy | section 0 terms: "S&P 500 index" |
| first draft of A (instruction only "use one only if that word is said …"), greedy | still listed "S&P 500 index" |
| after (final wording), sampled and greedy, iterations 2–4 | none |

## C: cost (Step 3)

Not measured: the first attempt was stopped before this step, and Revision 1 runs no model on the Mac.

## Section length (D)

Replaced in Revision 1 (below): sections are back to 3 to 6 points.

## Reviewer verdict on c0c87e1: revise (summary in Revision 1 below; the table was not filled in)

Counts from the reviewer's reading of each transcript against both versions (sampled set; greedy in brackets
if it differs).

| note | version | unsupported claims | inversions | invented action items | vocabulary leaks | missing major points |
|---|---|---|---|---|---|---|
| 19 | before | | | | | |
| 19 | after | | | | | |
| 16 | before | | | | | |
| 16 | after | | | | | |
| 17 | before | | | | | |
| 17 | after | | | | | |

Audit examples (after version):

| example | fixed? |
|---|---|
| "advocate / doctor" inversion | |
| "CAC registration" for CEC resignation | |
| "two more departments" for "too many departments" | |
| "Academic vs. Practical Focus" (host's question as an answer) | |
| Action item "philosophy in English" | |
| "60 years, 20 years" interpretation | |
| Section 0 "S&P 500 index" / "IPO" / "AI bubble" | |
| Section 0 skips pieces 3–6 (intro, dissent) | |

Rule from the brief: the change is kept only if the after version is no worse on every count and better on the
audit's examples.

## Revision 1 (after the reviewer's verdict on c0c87e1)

The reviewer read note 19 (Mac, before/after) and the phone's new note 20 against their transcripts. B helped a
little. A's wording alone did not stop "AI bubble" (another note's vocabulary) from being listed as a section 4 term
of note 20, a note that never says it. D made garbled sections worse (section 2 of note 19, both pieces conf 0.75,
went from 5 nonsense points to 9), and the sections still stopped early. Note 20's detailed notes were cut mid-sentence
at the 700-token cap. The revision:

| | c0c87e1 | Revision 1 |
|---|---|---|
| Vocabulary | Prompt wording only ("not words of this recording: use one only if …") | Filtered in code. The section, memory, notes and title prompts (system block and vocabulary line) list a saved term only if that note's material says it (`term_said`: case-insensitive; a Latin-script term must stand as whole words, so "IPO" is not said in "IPOs", while a Telugu ending such as "IPOలో" still counts; Telugu terms match as substrings). After a section summary is parsed, every term, hint or point naming a saved term the section does not say is dropped (`drop_unsaid_vocab`; a summary left with no point falls back). The transcription prompt keeps the full list as hearing help. The wording is now "spellings only: use one only if the transcript says it; …". |
| Notes and questions rules | B | B, plus: "Never expand an abbreviation the speaker did not expand." and "Leave out advertisements and sponsor messages." |
| Unclear pieces | The section prompt said "skip anything unclear or garbled" | A piece heard with conf < 0.80 is marked `[unclear] ` in the section prompt and in the notes material (`sve_notes_material`, which the app passes to `sve_generate` and `sve_transcript_fits`). No conf (older notes, NaN) is not marked. The section, notes, questions, summary and chunk prompts say: "Text marked [unclear] was hard to hear: do not state anything from it as fact; skip it or say the passage was unclear." The stored transcript, the title and the section fallback stay unmarked. C (one second hearing of a low-conf piece, higher conf kept) is unchanged; its log now says which hearing was kept. |
| Section points | Up to 10, "about one point for each [t=SEC] line", output cap 1024 | Back to 3 to 6 (parser cap 6, output cap 512): "3 to 6 points, each one sentence. Draw points from the beginning, middle and end of this part, not only its start." Sections also get the abbreviation and advertisement rule. |
| Notes output cap | 700 tokens at n_ctx 8192 | 1400 (`notes_max_out`: a quarter of the context below 5600; unchanged below 2800). The prompt scaffolding allowance goes from 250 to 400 tokens (`kNotesScaffold`), because the detailed-notes instructions with the rules are about 1250 characters. At n_ctx 8192 with a 300-token system prompt, the material budget is 6092 tokens (was 6942). A transcript between those sizes now goes to the sections path. `sve_transcript_fits` uses the same arithmetic, so it stays honest. A reply that still reaches the cap is trimmed to its last whole sentence or bullet (`trim_cut_reply`): a cut last line is dropped or cut after its last finished sentence, and trailing empty headings and a question without its answer go too. |

Verification: on the owner's phone (pending). No model was run for this revision; the owner regenerates notes 19 and
20 with the new build. Things to check there: no "AI bubble"/"S&P 500"/"IPO" in note 19's or note 20's sections and notes;
note 20's notes reach "Open questions"; section 2 of note 19 does not state its garbled lines as facts; no
"CAC (Civil Action Committee)"; the Oxford AI ad is not a key point; the sections have points from late in each section.
