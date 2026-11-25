# Cheaper transcription pieces (2026-11-09)

The goal was to make each 28-second transcription piece cheaper on the phone CPU while keeping
Gemma 4 E4B and its accuracy. Numbers, amounts and names matter most.

## How it was measured

- The 24 test pieces from notes 16 and 17, run on the Mac CPU with 4 threads and greedy decoding, through
  the app's own `sve_transcribe_next_piece`.
- Accuracy is compared with the earlier E4B Mac transcripts. The gate: the number differences
  (digits, amounts, Telugu number words) may be no more than what the phone and the Mac already
  disagree on for the same pieces, which is 8 number tokens in 3 pieces. The character error rate (CER)
  is reported too.
- The Mac stands in for the phone, so times are given as ratios (new / old). The phone check comes
  in a later task.
- `SVE_TIMING=1` now logs each piece's split:
  `[sve] piece timing: encode … s, prefill … s (… tok), decode … s (… tok)`.

## Where the time goes (per 28 s piece, Mac CPU, 4 threads)

| Step | Time | Share |
|---|---|---|
| Audio encoder (mmproj) | 1.04 s | 6% |
| Prefill (about 700 audio tokens plus about 130 prompt tokens, 832 in all) | 11.82 s | 65% |
| Writing the transcript (decode, about 108 tokens) | 5.18 s | 29% |
| Total | 18.0 s | |

Most of the time goes to the prefill, and the audio itself is most of the prefill. The prompt text
around it is small.

## Candidates

Each candidate was run alone. Ratios are against a fresh run of the old code in the same session
(433 s for the 24 pieces), which reproduced the earlier E4B Mac transcripts exactly. For (e) and (f)
the base is the code with (a) already in (420 s).

| Candidate | Time (new / old) | Accuracy vs the E4B Mac transcripts | Kept? |
|---|---|---|---|
| (a) Audio encoder on more threads than decoding (8 on the Mac) | 0.977 (encode 1.04 s to 0.59 s) | All 24 identical: 0 number differences, CER 0% | **Kept** |
| (b) No previous-piece context (was the last 400 bytes) | 0.958 (prefill 832 to 763 tokens) | CER 14.2%; 8 number tokens in 3 pieces, right at the limit. Two of them are real wrong values: "on 50 numbers" became "150 numbers", and "ఒక ఐదు 10 15 నిమిషాల" lost the five. The phone and the Mac both had them right. | Not kept |
| (c) Lower cap on output tokens per piece (now 768) | 1.00 | Unchanged: no piece came near the cap (the longest wrote 174 tokens) | Not kept: it saves nothing, and the densest real piece on the phone (about 1.6 KB of text) is about 460 tokens, so a lower cap could cut off real speech |
| (d) Smaller audio projector than Q8_0 | — | — | Skipped: no smaller E4B audio projector exists on Hugging Face. The only smaller one, `pleasen/Gemma-4-E4B-IT-QAT` Q4_0, is vision-only. |
| (e) Prefill on 6 threads, decoding stays on 4 | 0.90 (prefill 11.8 s to 9.1 s) | 23 of 24 identical; the one that changed is the romanized piece 17@1823, which was already unstable. Its numbers are the same. 0 number differences, CER 0.8%. | Yes, as the rule below |
| (e) Prefill on 8 threads, decoding stays on 4 | 0.75 (prefill 11.8 s to 7.2 s) | All 24 identical: 0 number differences, CER 0% | **Kept**, as the rule below |
| (f) Prefill batch (ubatch) 256 instead of 512 | 0.97 | All 24 identical | Not kept: under 5% |
| (f) Prefill batch and ubatch 1024 instead of 512 | 0.98 | All 24 identical | Not kept: under 5% |

The kept rule for (a) and (e): the prefill and the audio encoder use as many threads as the device
has cores, at most 8, and never fewer than decoding. Decoding stays on 4. That gives 8 on the Mac.
An iPhone (2 performance + 4 efficiency cores, 6 in all) gets **6**. Only (a) and (e) were kept, so
those are the two that were combined.

## Result

The final run with both kept changes: 298 s against 433 s, so each piece takes **0.69× the time**
on the Mac (about 31% less). The transcripts are **identical**: 24 of 24 the same, 0 number
differences, CER 0%. Per piece: encode 0.59 s, prefill 6.8 s, decode 5.0 s, about 12.4 s in all
(it was 18.0 s).

The same prefill threads also apply to the prefill of summaries and notes, so those start faster too.
They are still sampled as before.

## For the phone check (not measured on the phone yet)

- The phone gets 6 threads for the prefill and the encoder, and 4 for decoding. On the Mac, 6 prefill
  threads saved 10% and 8 saved 25%. On the phone, two of those six threads run on efficiency cores,
  so the gain may be smaller, or nothing.
- The earlier finding that 6 threads are slower on the phone was for everything on 6 threads. That is
  mostly memory-bound decoding, which stays on 4 here.
- Check the `encode` and `prefill` figures in the `SVE_TIMING=1` logs against today's phone logs.
  If they are not lower, set `n_threads_batch` back to `n_threads` (one line in `Llm::load`).
  The encoder follows it.
