#!/usr/bin/env python3
"""Memory accuracy test set: export a marking sheet, score the owner's marks, self-test.

    testset.py export --db notes.sqlite --audio recordings/ --out private/run1 [--target 100] [--pieces 60]
                      [--previous-marks marks.json] [--skip-run]
    testset.py score  --db private/run1/notes.sqlite --marks marks.json
    testset.py selftest

Python 3 standard library only. The original database is never opened for writing: export works on a copy.
"""
import argparse
import hashlib
import html
import json
import os
import random
import shutil
import sqlite3
import subprocess
import sys
import tempfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
SMOKE = os.path.join(ROOT, "core", "tests", "smoke_gemma.sh")

# Gate values from the plan (Task 8; spec 5.3, 5.9).
SHOWN_CORRECT_MIN = 0.95
MIN_MARKED_MEMORIES = 100
AUC_MIN = 0.70
FLAG_WRONG_MIN = 0.50
FLAG_RIGHT_MAX = 0.20
MIN_MARKED_PIECES = 60
MIN_WRONG_PIECES = 8

REASONS = [("words", "wrong words heard"), ("meaning", "wrong meaning"),
           ("person", "wrong person"), ("date", "wrong date or number")]
REASON_LABEL = dict(REASONS)

# A memory is "shown" when it passed every check and is stored, queued or not (not a medicine one, not marked wrong).


# ---------------------------------------------------------------- database helpers

def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def open_readonly(path):
    """Read-only and immutable, so scoring never changes the file (or its hash)."""
    con = sqlite3.connect("file:%s?mode=ro&immutable=1" % path, uri=True)
    con.row_factory = sqlite3.Row
    return con


def shown_memories(con, note_ids=None):
    """Shown memories with their first source (earliest start in a chosen note), oldest id first."""
    rows = con.execute(
        "SELECT m.id, m.type, m.text, m.who, m.due_date, m.due_text, m.queue, "
        "       s.note_id, s.start_sec, s.quote "
        "FROM memories m JOIN memory_sources s ON s.memory_id = m.id "
        "WHERE m.origin != 'medicine' AND m.review != 'wrong' "
        "ORDER BY m.id, s.start_sec").fetchall()
    out, seen = [], set()
    for r in rows:
        if r["id"] in seen or (note_ids is not None and r["note_id"] not in note_ids):
            continue
        seen.add(r["id"])
        out.append(dict(r))
    return out


def shown_count_for_notes(con, note_ids):
    return len(shown_memories(con, set(note_ids)))


# ---------------------------------------------------------------- sampling

def decile_bins(pieces):
    """Group pieces by conf decile (0..9, by rank); a piece with null conf goes in bin 'null'."""
    bins = {}
    known = sorted((p for p in pieces if p["conf"] is not None), key=lambda p: (p["conf"], p["rowid"]))
    for rank, p in enumerate(known):
        bins.setdefault(rank * 10 // len(known), []).append(p)
    nulls = [p for p in pieces if p["conf"] is None]
    if nulls:
        bins["null"] = nulls
    return bins


def sample_pieces(pieces, count, seed=1):
    """`count` pieces spread evenly over the conf deciles (and the null bin); a short bin gives its share away."""
    rng = random.Random(seed)
    bins = decile_bins(pieces)
    pools = {k: rng.sample(v, len(v)) for k, v in bins.items()}
    keys = sorted(pools, key=str)
    chosen = []
    while len(chosen) < count and any(pools[k] for k in keys):
        for k in keys:                      # one from each bin per round: even, and a short bin drops out
            if pools[k] and len(chosen) < count:
                chosen.append(pools[k].pop())
    return sorted(chosen, key=lambda p: (p["note_id"], p["start_sec"], p["rowid"]))


# ---------------------------------------------------------------- export

def run_smoke(db_copy, note_ids):
    env = dict(os.environ, SVE_DB=db_copy, SVE_NOTES=",".join(str(i) for i in note_ids))
    for name in ("SVE_GEMMA", "SVE_MMPROJ"):
        if name not in env:
            print("note: %s is not set" % name, file=sys.stderr)
    # Build the smoke binary once and reuse it for every batch.
    env.setdefault("SVE_SMOKE_BIN", os.path.join(os.path.dirname(db_copy), "smoke_gemma"))
    result = subprocess.run(["bash", SMOKE, "memories"], env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    sys.stdout.write(result.stdout[-4000:])
    if result.returncode != 0:
        sys.exit("smoke_gemma.sh memories failed (exit %d)" % result.returncode)


def copy_database(src, work_dir):
    """Copy notes.sqlite (+ -wal, -shm if present) into work_dir; the original is only read."""
    os.makedirs(work_dir, exist_ok=True)
    dst = os.path.join(work_dir, "notes.sqlite")
    for suffix in ("", "-wal", "-shm"):
        if os.path.exists(src + suffix):
            shutil.copyfile(src + suffix, dst + suffix)
    return dst


def settle_database(path):
    """Fold the WAL into the file and drop it, so one file (one hash) is the whole database."""
    con = sqlite3.connect(path)
    con.execute("PRAGMA wal_checkpoint(TRUNCATE)")
    con.execute("PRAGMA journal_mode=DELETE")
    con.close()
    for suffix in ("-wal", "-shm"):
        if os.path.exists(path + suffix):
            os.remove(path + suffix)


def load_previous(path):
    """Marks from an earlier sheet, keyed by (text, quote) for memories and by piece text for pieces."""
    if not path:
        return {}, {}
    with open(path) as f:
        prev = json.load(f)
    mem = {(m["text"], m["quote"]): {"v": m["verdict"], "r": m.get("reason")}
           for m in prev.get("memories", {}).values() if m.get("verdict")}
    pcs = {p["text"]: p["verdict"] for p in prev.get("pieces", {}).values() if p.get("verdict")}
    return mem, pcs


def cmd_export(args):
    os.makedirs(args.out, exist_ok=True)
    work = copy_database(args.db, os.path.join(args.out, "work"))
    # Newest notes first, only notes that have transcript pieces.
    con = sqlite3.connect(work)
    notes = [r[0] for r in con.execute(
        "SELECT n.id FROM notes n WHERE EXISTS (SELECT 1 FROM transcript_pieces p WHERE p.note_id = n.id) "
        "ORDER BY n.created_at DESC, n.id DESC")]
    con.close()
    chosen = []
    for nid in notes:
        chosen.append(nid)
        if not args.skip_run:
            run_smoke(work, [nid])
        settle_database(work)
        con = open_readonly(work)
        have = shown_count_for_notes(con, chosen)
        con.close()
        print("note %d: %d shown memories so far (%d notes)" % (nid, have, len(chosen)))
        if have >= args.target:
            break
    # Reopen read-write only to be sure nothing is left in a WAL, then hash the finished copy.
    settle_database(work)
    final = os.path.join(args.out, "notes.sqlite")
    shutil.move(work, final)
    shutil.rmtree(os.path.join(args.out, "work"), ignore_errors=True)
    digest = sha256_file(final)

    con = open_readonly(final)
    memories = shown_memories(con, set(chosen))
    notes_info = {r["id"]: dict(r) for r in con.execute(
        "SELECT id, title, lang, created_at, audio_path FROM notes WHERE id IN (%s)" % ",".join("?" * len(chosen)), chosen)}
    all_pieces = [dict(r) for r in con.execute(
        "SELECT rowid, note_id, idx, start_sec, text, conf FROM transcript_pieces WHERE note_id IN (%s) "
        "AND trim(text) != ''" % ",".join("?" * len(chosen)), chosen)]
    con.close()
    pieces = sample_pieces(all_pieces, args.pieces)

    # Audio: .m4a (or whatever the phone stored), found by file name in --audio, copied beside the sheet.
    audio_dir = os.path.join(args.out, "audio")
    os.makedirs(audio_dir, exist_ok=True)
    audio_rel = {}
    for nid, info in notes_info.items():
        base = os.path.basename(info["audio_path"] or "")
        src = os.path.join(args.audio, base) if base else ""
        if base and os.path.isfile(src):
            shutil.copyfile(src, os.path.join(audio_dir, base))
            audio_rel[nid] = "audio/" + base
        else:
            print("warning: no audio file for note %d (%s)" % (nid, base or "no audio_path"))

    prev_mem, prev_pcs = load_previous(args.previous_marks)
    carried = 0
    for m in memories:
        m["carry"] = prev_mem.get((m["text"], m["quote"]))
        carried += m["carry"] is not None
    for p in pieces:
        p["carry"] = prev_pcs.get(p["text"])

    data = {
        "db_sha256": digest,
        "reasons": REASONS,
        "notes": [{"id": nid, "title": notes_info[nid]["title"], "audio": audio_rel.get(nid),
                   "memories": [m for m in memories if m["note_id"] == nid]}
                  for nid in chosen if nid in notes_info],
        "pieces": [dict(p, audio=audio_rel.get(p["note_id"])) for p in pieces],
    }
    sheet = os.path.join(args.out, "sheet.html")
    with open(sheet, "w") as f:
        f.write(render_sheet(data))
    print("notes: %d  shown memories: %d  (carried-over marks: %d)  pieces: %d" % (len(chosen), len(memories), carried, len(pieces)))
    print("db copy: %s\nsha256: %s\nsheet: %s" % (final, digest, sheet))
    if len(memories) < args.target:
        print("warning: only %d shown memories, target was %d" % (len(memories), args.target))


# ---------------------------------------------------------------- sheet

def render_sheet(data):
    blob = json.dumps(data, ensure_ascii=False).replace("</", "<\\/").replace("<!--", "<\\!--")
    return SHEET_TEMPLATE.replace("__TITLE__", html.escape("Memory accuracy sheet")).replace("__DATA__", blob)


SHEET_TEMPLATE = r"""<!DOCTYPE html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>__TITLE__</title>
<style>
body{font:16px/1.45 -apple-system,system-ui,sans-serif;margin:0;background:#f4f4f2;color:#1b1b1b}
header{position:sticky;top:0;z-index:5;background:#fff;border-bottom:1px solid #ccc;padding:8px 16px;display:flex;gap:16px;align-items:center;flex-wrap:wrap}
header b{font-size:15px}.bar{flex:1;min-width:120px;height:8px;background:#ddd;border-radius:4px;overflow:hidden}.bar i{display:block;height:100%;background:#2d7d46;width:0}
button{font:inherit;padding:6px 14px;border:1px solid #555;border-radius:6px;background:#fff;cursor:pointer}button.save{background:#1b1b1b;color:#fff}
main{max-width:860px;margin:0 auto;padding:12px 16px 80px}h2{margin:28px 0 6px}h3{margin:22px 0 6px;font-size:16px;color:#444}
.help{font-size:13px;color:#555}
.card{background:#fff;border:2px solid transparent;border-radius:8px;padding:10px 14px;margin:10px 0;box-shadow:0 1px 2px #0002}
.card.focus{border-color:#3b6fd4}.card.right{border-left:6px solid #2d7d46}.card.wrong{border-left:6px solid #c0392b}
.tag{display:inline-block;font-size:12px;background:#e8e8e4;border-radius:4px;padding:0 6px;margin-right:6px}
.txt{font-size:17px;margin:4px 0}.meta{font-size:13px;color:#555}.quote{font-size:14px;color:#333;border-left:3px solid #bbb;padding-left:8px;margin:6px 0;white-space:pre-wrap}
.row{display:flex;gap:18px;align-items:center;flex-wrap:wrap;margin-top:6px}.row label{cursor:pointer}
.why{display:none;gap:12px;flex-wrap:wrap;margin-top:6px;font-size:14px}.card.wrong .why{display:flex}
audio{width:100%;max-width:420px;height:34px}textarea{width:100%;box-sizing:border-box;min-height:48px;font:inherit}
.carry{font-size:12px;color:#8a5a00}
</style></head><body>
<header><b id="count"></b><div class="bar"><i id="fill"></i></div>
<span class="help">Keys: J/K next/previous, R right, W wrong, 1-4 reason, H/G piece heard right/wrong</span>
<button class="save" id="save">Save marks</button></header>
<main id="main"></main>
<script>
const DATA = __DATA__;
const KEY = "svemarks:" + DATA.db_sha256;
const store = JSON.parse(localStorage.getItem(KEY) || '{"memories":{},"pieces":{},"missed":{}}');
const persist = () => { localStorage.setItem(KEY, JSON.stringify(store)); progress(); };
const el = (tag, cls, text) => { const e = document.createElement(tag); if (cls) e.className = cls; if (text != null) e.textContent = text; return e; };
const cards = [];

function audioEl(src, start) {
  if (!src) return el("div", "meta", "(no audio file found for this recording)");
  const a = el("audio"); a.controls = true; a.preload = "none"; a.src = src + "#t=" + (start || 0); return a;
}
function radio(name, value, label, checked, onchange) {
  const l = el("label"); const r = document.createElement("input");
  r.type = "radio"; r.name = name; r.value = value; r.checked = checked; r.onchange = onchange;
  l.append(r, " " + label); return l;
}
function mmss(s) { return Math.floor(s / 60) + ":" + String(s % 60).padStart(2, "0"); }

function memoryCard(m, note) {
  const key = String(m.id);
  if (!store.memories[key] && m.carry) store.memories[key] = { v: m.carry.v, r: m.carry.r, carried: true };
  const st = () => store.memories[key] || (store.memories[key] = {});
  const card = el("div", "card");
  const head = el("div", "meta"); head.append(el("span", "tag", m.type), document.createTextNode("#" + m.id + (m.queue ? " (waiting in the review queue)" : " (shown on the map)")));
  card.append(head, el("div", "txt", m.text));
  const bits = [];
  if (m.who) bits.push("who: " + m.who);
  if (m.due_date) bits.push("due: " + m.due_date);
  if (m.due_text) bits.push("due words: “" + m.due_text + "”");
  if (bits.length) card.append(el("div", "meta", bits.join("   |   ")));
  card.append(el("div", "quote", "“" + m.quote + "”  (" + mmss(m.start_sec) + ")"), audioEl(note.audio, m.start_sec));
  const row = el("div", "row"), why = el("div", "why");
  const paint = () => {
    const s = st(); card.classList.toggle("right", s.v === "right"); card.classList.toggle("wrong", s.v === "wrong");
    card.querySelectorAll("input").forEach(i => { i.checked = (i.name === "v" + key ? i.value === s.v : i.value === s.r); });
  };
  row.append(radio("v" + key, "right", "Right (R)", false, () => { st().v = "right"; delete st().r; delete st().carried; persist(); paint(); }),
             radio("v" + key, "wrong", "Wrong (W)", false, () => { st().v = "wrong"; delete st().carried; persist(); paint(); }));
  DATA.reasons.forEach(([code, label], i) =>
    why.append(radio("r" + key, code, label + " (" + (i + 1) + ")", false, () => { st().r = code; persist(); paint(); })));
  card.append(row, why);
  if (m.carry) card.append(el("div", "carry", "Marked in an earlier round (same text and quote); change it if it is wrong."));
  card.setVal = (v) => { st().v = v; if (v === "right") delete st().r; delete st().carried; persist(); paint(); };
  card.setReason = (r) => { if (st().v === "wrong") { st().r = r; persist(); paint(); } };
  card.isDone = () => { const s = store.memories[key]; return !!s && (s.v === "right" || (s.v === "wrong" && !!s.r)); };
  paint(); cards.push(card); return card;
}

function pieceCard(p) {
  const key = String(p.rowid);
  if (!store.pieces[key] && p.carry) store.pieces[key] = p.carry;
  const card = el("div", "card");
  card.append(el("div", "meta", "piece " + p.rowid + " | recording " + p.note_id + " | " + mmss(p.start_sec)), el("div", "txt", p.text), audioEl(p.audio, p.start_sec));
  const row = el("div", "row");
  const paint = () => { const v = store.pieces[key]; card.classList.toggle("right", v === "right"); card.classList.toggle("wrong", v === "wrong");
    card.querySelectorAll("input").forEach(i => i.checked = i.value === v); };
  const set = v => { store.pieces[key] = v; persist(); paint(); };
  row.append(radio("p" + key, "right", "heard right (H)", false, () => set("right")), radio("p" + key, "wrong", "heard wrong (G)", false, () => set("wrong")));
  card.append(row);
  card.setVal = v => set(v === "right" ? "right" : "wrong");
  card.setReason = () => {};
  card.isDone = () => !!store.pieces[key];
  card.pieceMode = true; paint(); cards.push(card); return card;
}

const main = document.getElementById("main");
main.append(el("h2", null, "Part 1: the memories"),
  el("div", "help", "Play the recording at the quote. Mark Right only if the memory is true to what was said. If Wrong, pick why. Write anything the recording said that is missing in the box under it."));
DATA.notes.forEach(note => {
  main.append(el("h3", null, "Recording " + note.id + ": " + note.title));
  note.memories.forEach(m => main.append(memoryCard(m, note)));
  const t = el("textarea"); t.placeholder = "Missed memories in this recording (one per line)";
  t.value = store.missed[note.id] || ""; t.oninput = () => { store.missed[note.id] = t.value; persist(); };
  main.append(t);
});
main.append(el("h2", null, "Part 2: was it heard right?"),
  el("div", "help", "Listen to the piece and compare it with the text. Heard right means the words are what was said."));
DATA.pieces.forEach(p => main.append(pieceCard(p)));

let at = -1;
function focusCard(i) {
  if (i < 0 || i >= cards.length) return;
  if (at >= 0) cards[at].classList.remove("focus");
  at = i; cards[at].classList.add("focus"); cards[at].scrollIntoView({ block: "center", behavior: "smooth" });
}
cards.forEach((c, i) => c.addEventListener("click", () => { if (at >= 0) cards[at].classList.remove("focus"); at = i; c.classList.add("focus"); }));
document.addEventListener("keydown", e => {
  if (e.target.tagName === "TEXTAREA" || e.ctrlKey || e.metaKey || e.altKey) return;
  const k = e.key.toLowerCase(), c = cards[at];
  if (k === "j") focusCard(at + 1); else if (k === "k") focusCard(Math.max(at - 1, 0));
  else if (c && k === "r" && !c.pieceMode) c.setVal("right"); else if (c && k === "w" && !c.pieceMode) c.setVal("wrong");
  else if (c && k === "h" && c.pieceMode) c.setVal("right"); else if (c && k === "g" && c.pieceMode) c.setVal("wrong");
  else if (c && "1234".includes(k) && !c.pieceMode) c.setReason(DATA.reasons[+k - 1][0]);
  else return;
  e.preventDefault();
});

function progress() {
  const done = cards.filter(c => c.isDone()).length;
  document.getElementById("count").textContent = done + " / " + cards.length + " marked";
  document.getElementById("fill").style.width = (100 * done / Math.max(cards.length, 1)) + "%";
}
document.getElementById("save").onclick = () => {
  const out = { db_sha256: DATA.db_sha256, memories: {}, pieces: {}, missed: store.missed };
  DATA.notes.forEach(n => n.memories.forEach(m => {
    const s = store.memories[String(m.id)]; if (!s || !s.v) return;
    out.memories[m.id] = { verdict: s.v, reason: s.v === "wrong" ? (s.r || null) : null, text: m.text, quote: m.quote };
  }));
  DATA.pieces.forEach(p => { const v = store.pieces[String(p.rowid)]; if (v) out.pieces[p.rowid] = { verdict: v, text: p.text }; });
  const a = document.createElement("a");
  a.href = URL.createObjectURL(new Blob([JSON.stringify(out, null, 1)], { type: "application/json" }));
  a.download = "marks.json"; a.click();
};
persist(); focusCard(0);
</script></body></html>
"""


# ---------------------------------------------------------------- score

def auc_low_conf_is_wrong(wrong, right):
    """Mann-Whitney AUC of 'lower conf means heard wrong'; a tie counts as half."""
    wins = sum(1.0 if w < r else 0.5 if w == r else 0.0 for w in wrong for r in right)
    return wins / (len(wrong) * len(right))


def decile_thresholds(values):
    """Candidate thresholds: the conf value at each decile boundary (10%..90%) of the marked pieces."""
    s = sorted(values)
    return sorted({s[min(len(s) - 1, len(s) * k // 10)] for k in range(1, 10)})


def best_threshold(wrong, right):
    """Among decile boundaries (flag conf < t), prefer those flagging >=50% wrong and <=20% right, then the biggest gap."""
    best = None
    for t in decile_thresholds(wrong + right):
        fw = sum(c < t for c in wrong) / len(wrong)
        fr = sum(c < t for c in right) / len(right)
        meets = fw >= FLAG_WRONG_MIN and fr <= FLAG_RIGHT_MAX
        cand = (meets, fw - fr, t, fw, fr)
        if best is None or cand[:2] > best[:2]:
            best = cand
    return best  # (meets, gap, t, flagged_wrong, flagged_right)


def pct(a, b):
    return "%.1f%%" % (100.0 * a / b) if b else "n/a"


def score(con, marks, out=print):
    """Print the gate lines. Returns (shown_correct_pass, conf_pass)."""
    mem_marks = marks.get("memories", {})
    types = {r["id"]: r["type"] for r in con.execute("SELECT id, type FROM memories")}
    right = wrong = 0
    by_type, by_reason = {}, {}
    for mid, m in mem_marks.items():
        if m.get("verdict") not in ("right", "wrong"):
            continue
        t = by_type.setdefault(types.get(int(mid), "unknown"), [0, 0])
        t[0 if m["verdict"] == "right" else 1] += 1
        if m["verdict"] == "right":
            right += 1
        else:
            wrong += 1
            reason = m.get("reason") or "no reason given"
            by_reason[reason] = by_reason.get(reason, 0) + 1
    marked = right + wrong
    ok = marked > 0 and right / marked >= SHOWN_CORRECT_MIN and marked >= MIN_MARKED_MEMORIES
    out("SHOWN-CORRECT: %d/%d = %s  GATE: %s (>= 95%%)" % (right, marked, "%.1f%%" % (100.0 * right / marked) if marked else "n/a",
                                                          "PASS" if ok else "FAIL"))
    if marked < MIN_MARKED_MEMORIES:
        out("  note: %d marked, the gate needs at least %d marked memories" % (marked, MIN_MARKED_MEMORIES))
    out("BY TYPE (right/marked):")
    for k in sorted(by_type):
        r, w = by_type[k]
        out("  %-9s %d/%d = %s" % (k, r, r + w, pct(r, r + w)))
    out("BY REASON (wrong memories):")
    for code, label in REASONS:
        out("  %-24s %d" % (label, by_reason.get(code, 0)))
    if by_reason.get("no reason given"):
        out("  %-24s %d" % ("no reason given", by_reason["no reason given"]))
    missed = sum(1 for text in marks.get("missed", {}).values() for line in text.splitlines() if line.strip())
    out("MISSED (reported): %d" % missed)

    # Pieces: conf of every marked piece, read from the database.
    conf = {str(r["rowid"]): r["conf"] for r in con.execute("SELECT rowid, conf FROM transcript_pieces")}
    heard_wrong, heard_right, no_conf = [], [], 0
    for pid, p in marks.get("pieces", {}).items():
        if p.get("verdict") not in ("right", "wrong"):
            continue
        c = conf.get(str(pid))
        if c is None:
            no_conf += 1
        else:
            (heard_wrong if p["verdict"] == "wrong" else heard_right).append(c)
    n_marked = len(heard_wrong) + len(heard_right)
    enough = n_marked >= MIN_MARKED_PIECES and len(heard_wrong) >= MIN_WRONG_PIECES and len(heard_right) > 0
    conf_ok = False
    out("PIECES: %d marked with conf (%d heard wrong, %d heard right), %d without conf left out" %
        (n_marked, len(heard_wrong), len(heard_right), no_conf))
    if heard_wrong and heard_right:
        auc = auc_low_conf_is_wrong(heard_wrong, heard_right)
        meets, _, t, fw, fr = best_threshold(heard_wrong, heard_right)
        out("AUC (low conf picks out heard-wrong): %.3f  (>= 0.70)" % auc)
        out("BEST THRESHOLD t = %.4f (flag conf < t): flags %s of wrong pieces, %s of right pieces" %
            (t, pct(round(fw * len(heard_wrong)), len(heard_wrong)), pct(round(fr * len(heard_right)), len(heard_right))))
        conf_ok = enough and auc >= AUC_MIN and meets
    else:
        out("AUC: n/a (needs both heard-right and heard-wrong pieces)")
    if not enough:
        out("  note: the gate needs at least %d marked pieces with at least %d marked wrong" % (MIN_MARKED_PIECES, MIN_WRONG_PIECES))
    out("CONF-GATE: %s" % ("PASS" if conf_ok else "FAIL"))
    return ok, conf_ok


def cmd_score(args):
    with open(args.marks) as f:
        marks = json.load(f)
    digest = sha256_file(args.db)
    if marks.get("db_sha256") != digest:
        sys.exit("marks.json was made for a different database (sha256 %s, this one is %s)" % (marks.get("db_sha256"), digest))
    con = open_readonly(args.db)
    score(con, marks)
    con.close()


# ---------------------------------------------------------------- selftest

def cmd_selftest(_args):
    with tempfile.TemporaryDirectory() as tmp:
        path = os.path.join(tmp, "t.sqlite")
        con = sqlite3.connect(path)
        con.executescript("""
            CREATE TABLE notes(id INTEGER PRIMARY KEY, title TEXT, lang TEXT, created_at INTEGER, audio_path TEXT);
            CREATE TABLE transcript_pieces(note_id INTEGER, idx INTEGER, start_sec INTEGER, text TEXT, conf REAL, PRIMARY KEY(note_id, idx));
            CREATE TABLE memories(id INTEGER PRIMARY KEY, type TEXT, text TEXT, who TEXT, due_date TEXT, due_text TEXT,
                                  queue INTEGER DEFAULT 0, origin TEXT DEFAULT 'recording', review TEXT DEFAULT 'new');
            CREATE TABLE memory_sources(memory_id INTEGER, note_id INTEGER, start_sec INTEGER, quote TEXT, PRIMARY KEY(memory_id, note_id, start_sec));
            INSERT INTO notes VALUES (1, 'Test', 'en', 1, 'x.m4a');
            INSERT INTO memories(id,type,text) VALUES (1,'fact','A'),(2,'promise','B'),(3,'fact','C'),(4,'fact','medicine one'),(5,'fact','dismissed');
            UPDATE memories SET origin='medicine' WHERE id=4; UPDATE memories SET review='wrong' WHERE id=5;
            INSERT INTO memory_sources VALUES (1,1,0,'qa'),(2,1,5,'qb'),(3,1,9,'qc'),(4,1,1,'qm'),(5,1,2,'qd');
            INSERT INTO transcript_pieces VALUES (1,0,0,'p0',0.2),(1,1,5,'p1',0.4),(1,2,9,'p2',0.8),(1,3,12,'p3',0.9);
        """)
        con.commit()
        con.close()
        db = open_readonly(path)
        shown = shown_memories(db)
        assert [m["id"] for m in shown] == [1, 2, 3], "shown excludes medicine and wrong-reviewed memories"
        piece_ids = {r["text"]: r["rowid"] for r in db.execute("SELECT rowid, text FROM transcript_pieces")}
        # Low-conf pieces p0, p1 heard wrong; p2, p3 heard right: a perfect separation, AUC 1.0.
        marks = {"memories": {"1": {"verdict": "right"}, "2": {"verdict": "wrong", "reason": "person"}, "3": {"verdict": "right"}},
                 "pieces": {str(piece_ids["p0"]): {"verdict": "wrong"}, str(piece_ids["p1"]): {"verdict": "wrong"},
                            str(piece_ids["p2"]): {"verdict": "right"}, str(piece_ids["p3"]): {"verdict": "right"}},
                 "missed": {"1": "the plumber is coming\n\nthe rent is due"}}
        lines = []
        score(db, marks, out=lines.append)
        db.close()
        print("\n".join(lines))
        assert lines[0] == "SHOWN-CORRECT: 2/3 = 66.7%  GATE: FAIL (>= 95%)", lines[0]
        assert "MISSED (reported): 2" in lines
        assert any(l.startswith("AUC") and "1.000" in l for l in lines)
        assert lines[-1] == "CONF-GATE: FAIL"            # 4 pieces is below the minimum count
        # Mann-Whitney with ties: wrong {0.2, 0.5}, right {0.5, 0.9}: wins 0.2<0.5, 0.2<0.9, tie, 0.5<0.9 = 3.5 / 4.
        assert abs(auc_low_conf_is_wrong([0.2, 0.5], [0.5, 0.9]) - 0.875) < 1e-9
        pieces = [{"rowid": i, "note_id": 1, "start_sec": i, "text": "t", "conf": None if i < 5 else i / 100} for i in range(105)]
        picked = sample_pieces(pieces, 33)
        assert len(picked) == 33 and sum(p["conf"] is None for p in picked) == 3, "null conf has a bin of its own"
    print("selftest ok")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    e = sub.add_parser("export", help="run the memories smoke on a copy of the db and write sheet.html")
    e.add_argument("--db", required=True, help="notes.sqlite copied from the phone (never modified)")
    e.add_argument("--audio", required=True, help="directory holding the recordings (matched by file name)")
    e.add_argument("--out", required=True, help="output directory (use tools/memory/private/...)")
    e.add_argument("--target", type=int, default=100, help="stop adding notes once this many memories are shown")
    e.add_argument("--pieces", type=int, default=60, help="transcript pieces to sample for part 2")
    e.add_argument("--previous-marks", help="marks.json from an earlier round; identical text+quote keeps its mark")
    e.add_argument("--skip-run", action="store_true", help="do not run the smoke; use the memories already in the db")
    e.set_defaults(fn=cmd_export)
    s = sub.add_parser("score", help="compute the two gates from marks.json")
    s.add_argument("--db", required=True, help="the notes.sqlite that export wrote (out/notes.sqlite)")
    s.add_argument("--marks", required=True)
    s.set_defaults(fn=cmd_score)
    t = sub.add_parser("selftest", help="check scoring on a tiny database")
    t.set_defaults(fn=cmd_selftest)
    args = ap.parse_args()
    args.fn(args)


if __name__ == "__main__":
    main()
