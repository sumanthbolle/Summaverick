# Meaning-search gate (Task 11)

The question: does keyword + meaning search (EmbeddingGemma 300M, fused with the keyword search by reciprocal
rank fusion) find the right transcript piece more often than the keyword search the app uses today? If it does
not, nothing ships: no download, the core code stays dormant and `sve_search_json` stays the keyword order.

The models run on the owner's iPhone, never on the Mac (battery rule). The Mac only checks the questions and
scores the results file the phone writes. Nothing here has been run yet.

## 1. Questions (Mac, no model)

`tools/memory/private/questions.jsonl` (git-ignored), one object per line:

    {"q": "...", "lang": "te" | "en" | "mixed", "note_id": 17, "gold_start_secs": [120]}

- At least 45: 15 Telugu, 15 English, 15 mixed, over the owner's recordings from Task 8
  (`tools/memory/private/notes.sqlite`).
- Each has 1 or 2 gold pieces (the `start_sec` of pieces of that note).
- Each is a paraphrase written from the transcript. It may not copy 3 or more consecutive words of its gold
  piece (case and punctuation ignored).

Check them:

    python3 tools/memory/meaning_gate.py check --db tools/memory/private/notes.sqlite \
        --questions tools/memory/private/questions.jsonl

It prints every problem, the file's sha256, and `CHECK OK` or `CHECK FAIL`. Then the owner reviews the
questions (owner checkpoint). The sha256 goes into the gate report
(`docs/superpowers/2026-11-08-meaning-gate-report.md`) before anything is measured, and the file is not edited
after that.

## 2. The phone run

Done on the iPhone 16e, plugged in, by a debug-only path in the app (to be added by the controller; it calls
only the core C API). It works on a copy of the Task 8 database and the checked questions file, both copied
into the app's Documents folder with `xcrun devicectl device copy to ...`:

1. Open the copy with `sve_init`. For every question, `sve_search_json(ctx, q, 60)` with no embedder loaded,
   and keep its first 5 `"kind":"piece"` rows as `[note_id, start_sec]`. That is the keyword result: the same
   FTS query and order as `keyword_query` + `search_pieces` in Ask today.
2. Load Gemma as the app does (CPU, 4 threads), then `sve_load_embedder(ctx, <gguf>, 4)` (always CPU).
   First the smallest EmbeddingGemma 300M GGUF quant at or under about 200 MB; Q8_0 only if that one fails.
3. Call `sve_embed_pending(ctx, 64, cb, user)` until it returns 0, timing it: seconds per embedded item.
   Stop with an error on -1 (no embedder, or a batch where nothing could be embedded: the embedder is broken),
   and on -2 (cancelled); never loop on a negative value.
4. For every question, `sve_search_json(ctx, q, 60)` again: the first 5 pieces are the hybrid result.
5. Write `meaning_results.jsonl` in Documents:

       {"meta": {"embedder": "<gguf file name>", "questions_sha256": "<sha256 of the questions file>",
                 "where": "iphone", "embed_sec_per_item": 0.0, "embedded": 0,
                 "vectors": 0, "index_bytes": 0, "peak_rss_mb": 0, "with_gemma": true}}
       {"i": 0, "q": "...", "keyword": [[17, 120], ...], "hybrid": [[17, 90], ...]}

   `i` is the question's line number, counted from 0 (blank lines skipped), and `q` its text as in the file. `vectors` and `index_bytes` are
   `SELECT COUNT(*), SUM(length(vec)) FROM embeddings`. `peak_rss_mb` is the app's peak resident memory with
   both models loaded.

`core/tests/smoke_gemma.sh gate` does the same steps and writes the same file on a host where models may run
(`SVE_DB`, `SVE_EMBEDDER`, `SVE_QUESTIONS`, `SVE_RESULTS`, `SVE_QUESTIONS_SHA256`, optional `SVE_GEMMA`). It is not used on the Mac.

## 3. Score (Mac, no model)

Copy `meaning_results.jsonl` back with `xcrun devicectl device copy from ...`, then:

    python3 tools/memory/meaning_gate.py run --db tools/memory/private/notes.sqlite \
        --questions tools/memory/private/questions.jsonl --embedder <gguf file name> \
        --results meaning_results.jsonl

With `--results`, `run` loads no model: it checks the questions again and scores the file. The file must be of
these questions: its `questions_sha256` must be the questions file's, every row's `q` must be that question's
text, and its `embedder` must be the `--embedder` file name; otherwise nothing is scored and it prints
`MEANING-GATE: FAIL`. It prints hit@5
(a gold piece in the top 5 pieces) per language group for keyword and hybrid, the phone's figures, and
`MEANING-GATE: PASS` or `MEANING-GATE: FAIL`.

Pass: hybrid hits >= keyword hits + 3 overall, and hybrid is not worse than keyword by more than 1 question in
any language group. A question with no result counts as a fail of the whole gate.

`python3 tools/memory/meaning_gate.py selftest` checks the copy rule and the scoring with made-up data.

## 4. Report

`docs/superpowers/2026-11-08-meaning-gate-report.md`: the questions' sha256 (written first), the quant tried,
the table and verdict, seconds per item on the phone, index size, peak memory with both models, and the note
that EmbeddingGemma is under the Gemma terms, which the owner checks before release (Task 23).
