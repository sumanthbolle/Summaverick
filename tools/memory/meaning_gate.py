#!/usr/bin/env python3
"""Meaning-search gate (Task 11): does keyword + meaning search find the right piece more often than keyword alone?

    meaning_gate.py check    --db notes.sqlite --questions private/questions.jsonl
    meaning_gate.py run      --db notes.sqlite --questions private/questions.jsonl --embedder embeddinggemma.gguf
                             [--gemma gemma.gguf] [--results results.jsonl]
    meaning_gate.py score    --questions private/questions.jsonl --results results.jsonl
    meaning_gate.py selftest

Questions: one JSON object per line, {"q", "lang": "te"|"en"|"mixed", "note_id", "gold_start_secs": [1 or 2]}.
At least 15 per language group (45 in all). A question may not copy 3 or more consecutive words of a gold piece.

run loads models. It is not run on the Mac (battery rule): the phone produces the results file (see
meaning/README.md) and `run --results <file>` (or `score`) only scores it. Without --results, run works on a copy
of --db through `core/tests/smoke_gemma.sh gate`.

Python 3 standard library only.
"""
import argparse
import hashlib
import json
import os
import shutil
import sqlite3
import subprocess
import sys
import tempfile
import unicodedata

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
SMOKE = os.path.join(ROOT, "core", "tests", "smoke_gemma.sh")

LANGS = ("te", "en", "mixed")
MIN_PER_LANG = 15
COPY_RUN = 3          # this many consecutive words of a gold piece in a question rejects it
TOP = 5               # hit@5
MARGIN_OVERALL = 3    # hybrid hits >= keyword hits + 3
MAX_GROUP_LOSS = 1    # and never more than 1 worse in a language group


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(1 << 16), b""):
            h.update(b)
    return h.hexdigest()


def words(text):
    """Lowercased words, punctuation and symbols stripped from their ends (Telugu vowel signs stay in the word)."""
    out = []
    for w in text.split():
        w = "".join(c for c in w if not unicodedata.category(c)[0] in "PS").lower()
        if w:
            out.append(w)
    return out


def copied_run(question, gold_text, n=COPY_RUN):
    """The first run of n consecutive words of the question that the gold text also has in a row, or None."""
    q, g = words(question), words(gold_text)
    runs = {tuple(g[i:i + n]) for i in range(len(g) - n + 1)}
    for i in range(len(q) - n + 1):
        if tuple(q[i:i + n]) in runs:
            return " ".join(q[i:i + n])
    return None


def load_questions(path):
    qs = []
    with open(path, encoding="utf-8") as f:
        for n, line in enumerate(f, 1):
            line = line.strip()
            if line:
                try:
                    qs.append(json.loads(line))
                except json.JSONDecodeError as e:
                    raise SystemExit(f"{path}:{n}: not JSON ({e})")
    return qs


def gold_texts(db, note_id, starts):
    """{start_sec: text} of the note's pieces at those seconds (read only)."""
    con = sqlite3.connect(f"file:{db}?mode=ro", uri=True)
    try:
        rows = con.execute("SELECT start_sec, text FROM transcript_pieces WHERE note_id=? ORDER BY idx", (note_id,)).fetchall()
    finally:
        con.close()
    return {s: t for s, t in rows if s in set(starts)}


def check_questions(qs, texts_of):
    """Problems as strings ([] = all good). texts_of(note_id, starts) -> {start_sec: text}."""
    problems = []
    counts = {l: 0 for l in LANGS}
    for i, q in enumerate(qs, 1):
        where = f"question {i}"
        if not isinstance(q, dict) or not isinstance(q.get("q"), str) or not q["q"].strip():
            problems.append(f"{where}: no question text")
            continue
        if q.get("lang") not in LANGS:
            problems.append(f"{where}: lang must be one of {', '.join(LANGS)}")
        else:
            counts[q["lang"]] += 1
        gold = q.get("gold_start_secs")
        if not isinstance(q.get("note_id"), int) or not isinstance(gold, list) or not 1 <= len(gold) <= 2 \
                or not all(isinstance(s, int) for s in gold):
            problems.append(f"{where}: needs note_id and 1-2 gold_start_secs")
            continue
        texts = texts_of(q["note_id"], gold)
        for s in gold:
            if s not in texts:
                problems.append(f"{where}: no piece at {s} s in note {q['note_id']}")
                continue
            run = copied_run(q["q"], texts[s])
            if run:
                problems.append(f"{where}: REJECT copies \"{run}\" from its gold piece at {s} s; write it as a paraphrase")
    for l in LANGS:
        if counts[l] < MIN_PER_LANG:
            problems.append(f"only {counts[l]} {l} questions (at least {MIN_PER_LANG})")
    return problems


def cmd_check(a):
    qs = load_questions(a.questions)
    problems = check_questions(qs, lambda n, s: gold_texts(a.db, n, s))
    for p in problems:
        print(p)
    print(f"{len(qs)} questions, sha256 {sha256(a.questions)}")
    print("CHECK OK" if not problems else f"CHECK FAIL ({len(problems)} problems)")
    return 0 if not problems else 1


def load_results(path):
    meta, rows = {}, {}
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            j = json.loads(line)
            if "meta" in j:
                meta = j["meta"]
            else:
                rows[j["i"]] = j
    return meta, rows


def results_problems(qs, meta, rows, questions_sha, embedder=None):
    """Why a results file is not of these questions (and this embedder): [] when it is."""
    problems = []
    if meta.get("questions_sha256") != questions_sha:
        problems.append(f"results are of questions sha256 {meta.get('questions_sha256')}, not {questions_sha}")
    if embedder and meta.get("embedder") != os.path.basename(embedder):
        problems.append(f"results are of embedder {meta.get('embedder')}, not {os.path.basename(embedder)}")
    for i, q in enumerate(qs):
        r = rows.get(i)
        if r is not None and r.get("q") != q["q"]:
            problems.append(f"result {i} is for another question: {r.get('q')!r}")
    return problems


def hit(pieces, note_id, gold):
    return any(p[0] == note_id and p[1] in gold for p in pieces[:TOP])


def score(qs, rows):
    """{"groups": {lang: (n, keyword, hybrid)}, "total": (n, keyword, hybrid), "pass": bool, "missing": [i]}"""
    groups = {l: [0, 0, 0] for l in LANGS}
    missing = []
    for i, q in enumerate(qs):
        r = rows.get(i)
        if r is None:
            missing.append(i)
            continue
        g = groups[q["lang"]]
        g[0] += 1
        g[1] += hit(r["keyword"], q["note_id"], q["gold_start_secs"])
        g[2] += hit(r["hybrid"], q["note_id"], q["gold_start_secs"])
    total = [sum(g[k] for g in groups.values()) for k in range(3)]
    ok = not missing and total[2] >= total[1] + MARGIN_OVERALL and \
        all(g[2] >= g[1] - MAX_GROUP_LOSS for g in groups.values())
    return {"groups": {l: tuple(g) for l, g in groups.items()}, "total": tuple(total), "pass": ok, "missing": missing}


def print_score(s, meta):
    print(f"{'group':<8}{'n':>4}{'keyword@5':>11}{'hybrid@5':>10}{'diff':>6}")
    for l in LANGS:
        n, k, h = s["groups"][l]
        print(f"{l:<8}{n:>4}{k:>11}{h:>10}{h - k:>+6}")
    n, k, h = s["total"]
    print(f"{'all':<8}{n:>4}{k:>11}{h:>10}{h - k:>+6}")
    if s["missing"]:
        print(f"no result for questions {s['missing']}")
    for key in ("embedder", "where", "embed_sec_per_item", "embedded", "vectors", "index_bytes", "peak_rss_mb", "with_gemma"):
        if key in meta:
            print(f"{key}: {meta[key]}")
    print(f"MEANING-GATE: {'PASS' if s['pass'] else 'FAIL'}")


def cmd_score(a):
    qs = load_questions(a.questions)
    meta, rows = load_results(a.results)
    qsha = sha256(a.questions)
    print(f"questions sha256 {qsha}")
    problems = results_problems(qs, meta, rows, qsha, getattr(a, "embedder", None))
    if problems:
        for p in problems:
            print(p)
        print("MEANING-GATE: FAIL (the results do not match; nothing scored)")
        return 1
    s = score(qs, rows)
    print_score(s, meta)
    return 0 if s["pass"] else 1


def cmd_run(a):
    qs = load_questions(a.questions)
    problems = check_questions(qs, lambda n, s: gold_texts(a.db, n, s))
    if problems:
        for p in problems:
            print(p)
        raise SystemExit("the questions do not pass check; nothing measured")
    if a.results:
        return cmd_score(a)
    # Loads the embedder (and Gemma with --gemma) on this machine: for a host where that is allowed, not the Mac.
    with tempfile.TemporaryDirectory() as tmp:
        work = os.path.join(tmp, "notes.sqlite")
        for suf in ("", "-wal", "-shm"):
            if os.path.exists(a.db + suf):
                shutil.copyfile(a.db + suf, work + suf)
        results = os.path.join(tmp, "results.jsonl")
        env = dict(os.environ, SVE_DB=work, SVE_EMBEDDER=a.embedder, SVE_QUESTIONS=a.questions, SVE_RESULTS=results,
                   SVE_QUESTIONS_SHA256=sha256(a.questions))
        if a.gemma:
            env["SVE_GEMMA"] = a.gemma
        else:
            env.pop("SVE_GEMMA", None)
        subprocess.run(["bash", SMOKE, "gate"], env=env, check=True)
        a.results = results
        return cmd_score(a)


def selftest():
    fails = []

    def expect(ok, what):
        if not ok:
            fails.append(what)

    expect(copied_run("Where did Ravi keep the locker key?", "Ravi said he will keep the locker key in the diary.") ==
           "keep the locker", "three words in a row are caught")
    expect(copied_run("Where is the key for the locker?", "Ravi will keep the locker key in the diary.") is None,
           "the same words out of order pass")
    expect(copied_run("KEEP, the locker!", "keep the locker") == "keep the locker", "case and punctuation ignored")
    expect(copied_run("మందుల గురించి డాక్టర్ ఏమన్నారు", "డాక్టర్ గారు మందుల గురించి చెప్పారు") is None, "Telugu, two words only")
    expect(copied_run("మందుల గురించి డాక్టర్ ఏమన్నారు", "ఆయన మందుల గురించి డాక్టర్ తో") == "మందుల గురించి డాక్టర్",
           "Telugu run of three")

    pieces = {(1, 0): "Ravi said he will keep the locker key in the diary.", (1, 30): "We go to Guntur on Friday."}

    def texts_of(n, starts):
        return {s: pieces[(n, s)] for s in starts if (n, s) in pieces}

    qs = [{"q": f"question {i}", "lang": l, "note_id": 1, "gold_start_secs": [0]} for l in LANGS for i in range(MIN_PER_LANG)]
    expect(check_questions(qs, texts_of) == [], "a good set passes")
    bad = qs + [{"q": "will keep the locker", "lang": "en", "note_id": 1, "gold_start_secs": [0]},
                {"q": "x", "lang": "en", "note_id": 1, "gold_start_secs": [99]},
                {"q": "x", "lang": "fr", "note_id": 1, "gold_start_secs": [0]}]
    p = check_questions(bad, texts_of)
    expect(len(p) == 3 and "REJECT" in p[0] and "no piece at 99" in p[1] and "lang" in p[2], f"bad rows found: {p}")
    expect(any("only 14 te" in x for x in check_questions(qs[1:], texts_of)), "too few in a group")

    def rows_for(kw_hits, hy_hits):
        rows = {}
        for i, q in enumerate(qs):
            kw = [[1, 0]] if i in kw_hits else [[1, 30]]
            hy = [[2, 5], [1, 30], [1, 30], [1, 30], [1, 0]] if i in hy_hits else [[1, 30]] * 5 + [[1, 0]]
            rows[i] = {"i": i, "keyword": kw, "hybrid": hy}
        return rows

    s = score(qs, rows_for(set(), {0, 1, 2}))
    expect(s["pass"] and s["total"] == (45, 0, 3), f"3 more hits pass: {s}")
    s = score(qs, rows_for(set(), {0, 1}))
    expect(not s["pass"], "2 more hits fail")
    # te loses 2 (0, 1) while en gains 5: overall +3 but a group is worse by more than 1.
    s = score(qs, rows_for({0, 1}, set(range(15, 20))))
    expect(not s["pass"] and s["groups"]["te"] == (15, 2, 0), f"a group 2 worse fails: {s}")
    s = score(qs, rows_for({0}, set(range(15, 19))))
    expect(s["pass"], f"a group 1 worse with +3 overall passes: {s}")
    r = rows_for(set(), set(range(45)))
    del r[3]
    expect(not score(qs, r)["pass"], "a missing result fails")
    expect(hit([[1, 9]] * 5 + [[1, 0]], 1, [0]) is False, "only the top 5 count")

    # The results file must be of these questions, in this order, and of this embedder.
    rows = rows_for(set(), set())
    for i, q in enumerate(qs):
        rows[i]["q"] = q["q"]
    meta = {"questions_sha256": "abc", "embedder": "eg-q4.gguf"}
    expect(results_problems(qs, meta, rows, "abc", "/models/eg-q4.gguf") == [], "a matching file has no problems")
    expect(results_problems(qs, meta, rows, "abc") == [], "no --embedder: not compared")
    expect(len(results_problems(qs, meta, rows, "def")) == 1, "another questions file's sha256")
    expect(len(results_problems(qs, meta, rows, "abc", "eg-q8.gguf")) == 1, "another embedder")
    rows[7]["q"] = "a different question"
    p = results_problems(qs, meta, rows, "abc")
    expect(len(p) == 1 and "result 7" in p[0], f"a row of another question: {p}")
    expect(len(results_problems(qs, {}, rows, "abc", "eg-q4.gguf")) == 3, "no meta at all")

    for f in fails:
        print("FAIL", f)
    print("SELFTEST OK" if not fails else f"SELFTEST FAIL ({len(fails)})")
    return 1 if fails else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("check")
    c.add_argument("--db", required=True)
    c.add_argument("--questions", required=True)
    r = sub.add_parser("run")
    r.add_argument("--db", required=True)
    r.add_argument("--questions", required=True)
    r.add_argument("--embedder", required=True)
    r.add_argument("--gemma")
    r.add_argument("--results", help="score this results file (from the phone) instead of running the models")
    s = sub.add_parser("score")
    s.add_argument("--questions", required=True)
    s.add_argument("--results", required=True)
    s.add_argument("--embedder", help="the GGUF the results must be of (its file name is compared)")
    sub.add_parser("selftest")
    a = ap.parse_args()
    if a.cmd == "check":
        return cmd_check(a)
    if a.cmd == "run":
        return cmd_run(a)
    if a.cmd == "score":
        return cmd_score(a)
    return selftest()


if __name__ == "__main__":
    sys.exit(main())
