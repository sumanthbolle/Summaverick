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

| Candidate | Time (new / old) | Accuracy vs the E4B Mac transcripts | Kept? |
|---|---|---|---|
| (a) Audio encoder on up to 8 threads; decoding stays on 4 | 0.977 (encode 1.04 s to 0.59 s) | All 24 transcripts identical: 0 number differences, CER 0% | **Kept** |
| (b) No previous-piece context (was the last 400 bytes) | 0.958 (prefill 832 to 763 tokens) | CER 14.2%; 8 number tokens in 3 pieces, right at the limit. Two of them are real wrong values: "on 50 numbers" became "150 numbers", and "ఒక ఐదు 10 15 నిమిషాల" lost the five. The phone and the Mac both had them right. | Not kept |
| (c) Lower cap on output tokens per piece (now 768) | 1.00 | Unchanged: no piece came near the cap (the longest wrote 174 tokens) | Not kept: it saves nothing, and the densest real piece on the phone (about 1.6 KB of text) is about 460 tokens, so a lower cap could cut off real speech |
| (d) Smaller audio projector than Q8_0 | — | — | Skipped: no smaller E4B audio projector exists on Hugging Face. The only smaller one, `pleasen/Gemma-4-E4B-IT-QAT` Q4_0, is vision-only. |

Kept in the end: (a) only. With nothing else kept there was nothing to combine. Ratios are against
a fresh run of the old code in the same session (433 s for the 24 pieces). That run reproduced the
earlier E4B Mac transcripts exactly.

## Result

The final run with the change built in: 420 s against 433 s, so each piece takes **0.97× the
time** on the Mac (about 3% less), with **identical transcripts**: 24 of 24 the same, 0 number
differences, CER 0%. The output does not change, because ggml computes each value on a single thread, so
the thread count does not change the numbers.

This is small. The big costs are the prefill and the decode, and neither of them can shrink much
without changing what the model hears or writes.

## For the phone check

- The phone has 6 cores, so the encoder gets 6 threads there (the rule is up to 8, never fewer than
  the model's 4). The earlier finding that 6 threads are slower was for the whole run. That run is
  mostly memory-bound decode. The encoder is a short compute burst. Check the `encode` figure in
  `SVE_TIMING=1` logs: if it is not lower than with 4 threads, set the encoder back to the model's
  threads (one line in `sve_load_audio`).
- Not tried here, but worth a look: the prefill split (`n_ubatch` 512 against about 830 tokens) and
  prefill threads. Prefill is 65% of each piece. More prefill threads on the phone is a thread-count
  question that the Mac cannot answer.
