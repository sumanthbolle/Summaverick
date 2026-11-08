#!/usr/bin/env python3
"""Memory accuracy test set: export a marking sheet, score the owner's marks, self-test.

    testset.py export --db notes.sqlite --audio recordings/ --out private/run1 [--target 100] [--pieces 60]
                      [--previous-marks marks.json] [--skip-run]
    testset.py score  --db private/run1/notes.sqlite --marks marks.json
    testset.py selftest

Python 3 standard library only. The original database is never opened for writing: export works on a copy.
"""
import argparse
import collections
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
import urllib.parse

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

def run_smoke(db_copy, note_id, position, total):
    """Run the memories smoke on one note of the copy, echoing its output as it comes. Returns (ok, tail lines)."""
    env = dict(os.environ, SVE_DB=db_copy, SVE_NOTES=str(note_id))
    for seed in ("SVE_SEED_FIXTURE", "SVE_SEED_WAV"):   # seeding would add notes to the copy
        env.pop(seed, None)
    if "SVE_GEMMA" not in env:
        print("note: SVE_GEMMA is not set", file=sys.stderr)
    # Build the smoke binary once and reuse it for every note.
    env.setdefault("SVE_SMOKE_BIN", os.path.join(os.path.dirname(db_copy), "smoke_gemma"))
    print("=== note %d (%d/%d): running the memories smoke" % (note_id, position, total), flush=True)
    tail = collections.deque(maxlen=40)
    proc = subprocess.Popen(["bash", SMOKE, "memories"], env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    for line in proc.stdout:
        tail.append(line.rstrip("\n"))
        sys.stdout.write("    " + line)
        sys.stdout.flush()
    return proc.wait() == 0, list(tail)


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
    """Marks from an earlier sheet: memories keyed by (text, quote), pieces by (text, audio file name, start_sec)."""
    if not path:
        return {}, {}
    with open(path) as f:
        prev = json.load(f)
    mem = {(m["text"], m["quote"]): {"v": m["verdict"], "r": m.get("reason")}
           for m in prev.get("memories", {}).values() if m.get("verdict")}
    pcs = {(p["text"], p.get("audio_name"), p.get("start_sec")): p["verdict"]
           for p in prev.get("pieces", {}).values() if p.get("verdict")}
    return mem, pcs


def read_state(out_dir):
    path = os.path.join(out_dir, "state.json")
    if os.path.exists(path):
        with open(path) as f:
            return json.load(f)
    return {"done": [], "failed": {}}


def write_state(out_dir, state):
    with open(os.path.join(out_dir, "state.json"), "w") as f:
        json.dump(state, f, indent=1)


def build_sheet(out_dir, audio_dir, piece_count, previous_marks):
    """Write sheet.html from out_dir/notes.sqlite and the done notes in state.json. No model needed."""
    state = read_state(out_dir)
    chosen = state["done"]
    if not chosen:
        sys.exit("no finished notes in %s; run export first" % out_dir)
    final = os.path.join(out_dir, "notes.sqlite")
    digest = sha256_file(final)
    con = open_readonly(final)
    marks = ",".join("?" * len(chosen))
    memories = shown_memories(con, set(chosen))
    notes_info = {r["id"]: dict(r) for r in con.execute(
        "SELECT id, title, lang, created_at, audio_path FROM notes WHERE id IN (%s)" % marks, chosen)}
    all_pieces = [dict(r) for r in con.execute(
        "SELECT rowid, note_id, idx, start_sec, text, conf FROM transcript_pieces WHERE note_id IN (%s) "
        "AND trim(text) != ''" % marks, chosen)]
    con.close()
    pieces = sample_pieces(all_pieces, piece_count)

    # Audio: .m4a (or whatever the phone stored), found by file name in audio_dir, copied beside the sheet.
    os.makedirs(os.path.join(out_dir, "audio"), exist_ok=True)
    audio_name = {}
    for nid, info in notes_info.items():
        base = os.path.basename(info["audio_path"] or "")
        src = os.path.join(audio_dir, base) if base else ""
        if base and os.path.isfile(src):
            shutil.copyfile(src, os.path.join(out_dir, "audio", base))
            audio_name[nid] = base
        else:
            print("warning: no audio file for note %d (%s)" % (nid, base or "no audio_path"))

    prev_mem, prev_pcs = load_previous(previous_marks)
    carried = 0
    for m in memories:
        m["carry"] = prev_mem.get((m["text"], m["quote"]))
        carried += m["carry"] is not None
    for p in pieces:
        p["audio_name"] = audio_name.get(p["note_id"])
        p["carry"] = prev_pcs.get((p["text"], p["audio_name"], p["start_sec"]))

    data = {
        "db_sha256": digest,
        "reasons": REASONS,
        "failed": sorted(int(k) for k in state["failed"]),
        "notes": [{"id": nid, "title": notes_info[nid]["title"], "audio": audio_src(audio_name.get(nid)),
                   "memories": [m for m in memories if m["note_id"] == nid]}
                  for nid in chosen if nid in notes_info],
        "pieces": [dict(p, audio=audio_src(p["audio_name"])) for p in pieces],
    }
    sheet = os.path.join(out_dir, "sheet.html")
    with open(sheet, "w") as f:
        f.write(render_sheet(data))
    print("notes: %d  shown memories: %d  (carried-over marks: %d)  pieces: %d" % (len(chosen), len(memories), carried, len(pieces)))
    print("db copy: %s\nsha256: %s\nsheet: %s" % (final, digest, sheet))
    return len(memories)


def audio_src(name):
    """Relative, URL-encoded path of a recording copied under audio/ (None if there is none)."""
    return "audio/" + urllib.parse.quote(name) if name else None


def cmd_export(args):
    os.makedirs(args.out, exist_ok=True)
    work_dir = os.path.join(args.out, "work")
    work = os.path.join(work_dir, "notes.sqlite")
    state = read_state(args.out)
    if args.resume and os.path.exists(work):
        print("resuming: %d notes already done" % len(state["done"]))
    else:
        work = copy_database(args.db, work_dir)
        state = {"done": [], "failed": {}}
        write_state(args.out, state)
    con = sqlite3.connect(work)
    notes = [r[0] for r in con.execute(
        "SELECT n.id FROM notes n WHERE EXISTS (SELECT 1 FROM transcript_pieces p WHERE p.note_id = n.id) "
        "ORDER BY n.created_at DESC, n.id DESC")]
    con.close()
    if args.notes:                       # exactly these notes, newest first; --target does not pick
        wanted = [int(x) for x in args.notes.split(",") if x.strip()]
        missing = [n for n in wanted if n not in notes]
        if missing:
            sys.exit("notes without transcript pieces or not in the db: %s" % missing)
        notes = [n for n in notes if n in wanted]
    for pos, nid in enumerate(notes, 1):
        if str(nid) in state["failed"] and not args.resume:
            continue
        if nid not in state["done"]:
            if args.skip_run:
                ok, tail = True, []
            else:
                ok, tail = run_smoke(work, nid, pos, len(notes))
            settle_database(work)
            if not ok:
                state["failed"][str(nid)] = "\n".join(tail[-15:])
                write_state(args.out, state)
                print("!!! note %d FAILED; last output:\n%s" % (nid, "\n".join(tail[-15:])), flush=True)
                continue
            state["failed"].pop(str(nid), None)
            state["done"].append(nid)
            write_state(args.out, state)
        con = open_readonly(work)
        have = shown_count_for_notes(con, state["done"])
        con.close()
        print("note %d done: %d shown memories so far (%d notes)" % (nid, have, len(state["done"])), flush=True)
        if not args.notes and have >= args.target:
            break
    settle_database(work)
    shutil.copyfile(work, os.path.join(args.out, "notes.sqlite"))   # the work copy stays, for --resume
    if not state["done"]:
        sys.exit("no note finished; failed: %s" % sorted(state["failed"]))
    shown = build_sheet(args.out, args.audio, args.pieces, args.previous_marks)
    if shown < MIN_MARKED_MEMORIES:
        print("shown memories: %d (below the %d the gate needs; the result will be reported as a smaller set)" %
              (shown, MIN_MARKED_MEMORIES))
    elif not args.notes and shown < args.target:
        print("warning: only %d shown memories, target was %d" % (shown, args.target))
    if state["failed"]:
        print("FAILED NOTES (left out of the sheet): %s. Re-run with --resume to try them again." % sorted(int(k) for k in state["failed"]))
        sys.exit(1)


def cmd_render(args):
    build_sheet(args.out, args.audio, args.pieces, args.previous_marks)


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
<div id="warn" style="display:none;background:#fde8c8;padding:8px 16px">Marks are not saved automatically here. Press Save marks often.</div>
<div id="failed" style="display:none;background:#f8d7d7;padding:8px 16px"></div>
<main id="main"></main>
<script>
const DATA = __DATA__;
const KEY = "svemarks:" + DATA.db_sha256;
let store = { memories: {}, pieces: {}, missed: {} }, canSave = true;
try { store = JSON.parse(localStorage.getItem(KEY) || JSON.stringify(store)); } catch (e) { canSave = false; }
const persist = () => {
  if (canSave) { try { localStorage.setItem(KEY, JSON.stringify(store)); } catch (e) { canSave = false; } }
  document.getElementById("warn").style.display = canSave ? "none" : "block";
  progress();
};
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
  card.key = key;
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

if (DATA.failed.length) { const f = document.getElementById("failed"); f.style.display = "block";
  f.textContent = "Left out: recordings " + DATA.failed.join(", ") + " failed to process, so this sheet does not cover them."; }
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
  DATA.pieces.forEach(p => { const v = store.pieces[String(p.rowid)];
    if (v) out.pieces[p.rowid] = { verdict: v, text: p.text, audio_name: p.audio_name, start_sec: p.start_sec }; });
  const unmarked = cards.filter(c => !c.pieceMode && !(store.memories[c.key] || {}).v).length + cards.filter(c => c.pieceMode && !c.isDone()).length;
  const noReason = cards.filter(c => !c.pieceMode && (store.memories[c.key] || {}).v === "wrong" && !store.memories[c.key].r).length;
  if ((unmarked || noReason) && !confirm(unmarked + " cards are unmarked and " + noReason + " Wrong cards have no reason. Save anyway?")) return;
  const a = document.createElement("a");
  a.href = URL.createObjectURL(new Blob([JSON.stringify(out, null, 1)], { type: "application/json" }));
  a.download = "marks.json"; document.body.appendChild(a); a.click(); a.remove();
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
    # Only memories that are in the db and still count as shown (an id not in this db is skipped).
    types = {r["id"]: r["type"] for r in con.execute(
        "SELECT id, type FROM memories WHERE origin != 'medicine' AND review != 'wrong'")}
    right = wrong = 0
    by_type, by_reason = {}, {}
    for mid, m in mem_marks.items():
        if m.get("verdict") not in ("right", "wrong") or int(mid) not in types:
            continue
        t = by_type.setdefault(types[int(mid)], [0, 0])
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
        # 100 marked memories, 96 right: gate passes; 95/100 also passes (>= 95%), 94/100 fails.
        big = os.path.join(tmp, "big.sqlite")
        bc = sqlite3.connect(big)
        bc.executescript("""
            CREATE TABLE memories(id INTEGER PRIMARY KEY, type TEXT, origin TEXT DEFAULT 'recording', review TEXT DEFAULT 'new');
            CREATE TABLE transcript_pieces(note_id INTEGER, idx INTEGER, conf REAL);""")
        bc.executemany("INSERT INTO memories(id,type) VALUES (?, 'fact')", [(i,) for i in range(1, 101)])
        # 80 pieces: 10 heard wrong with conf 0.1..0.55, 70 heard right with conf 0.3..1.0 (low conf separates well).
        wrong_conf = [0.10 + 0.05 * i for i in range(10)]
        right_conf = [0.30 + 0.01 * i for i in range(70)]
        bc.executemany("INSERT INTO transcript_pieces VALUES (1, ?, ?)", list(enumerate(wrong_conf + right_conf)))
        bc.execute("UPDATE memories SET origin='medicine' WHERE id=100")   # shown filter: id 100 does not count
        bc.commit()
        bc.close()
        bdb = open_readonly(big)
        rowids = [r[0] for r in bdb.execute("SELECT rowid FROM transcript_pieces ORDER BY idx")]
        def big_marks(n_wrong):
            return {"memories": {str(i): {"verdict": "wrong" if i <= n_wrong else "right", "reason": "meaning"} for i in range(1, 100)},
                    "pieces": {str(r): {"verdict": "wrong" if k < 10 else "right"} for k, r in enumerate(rowids)}, "missed": {}}
        lines = []
        ok, conf_ok = score(bdb, dict(big_marks(4), memories=dict(big_marks(4)["memories"], **{"100": {"verdict": "wrong"}, "999": {"verdict": "wrong"}})),
                            out=lines.append)
        assert lines[0] == "SHOWN-CORRECT: 95/99 = 96.0%  GATE: FAIL (>= 95%)", lines[0]  # 99 marked < 100: medicine and unknown ids skipped
        assert not ok
        # 100 shown: add one more shown memory.
        bdb.close()
        bc = sqlite3.connect(big)
        bc.execute("UPDATE memories SET origin='recording' WHERE id=100")
        bc.commit()
        bc.close()
        bdb = open_readonly(big)
        m = big_marks(4)
        m["memories"]["100"] = {"verdict": "right"}
        lines = []
        ok, conf_ok = score(bdb, m, out=lines.append)
        assert lines[0] == "SHOWN-CORRECT: 96/100 = 96.0%  GATE: PASS (>= 95%)", lines[0]
        assert ok and conf_ok and lines[-1] == "CONF-GATE: PASS", "\n".join(lines)
        print("\n".join(lines[:2] + lines[-4:]))
        m = big_marks(6)
        m["memories"]["100"] = {"verdict": "right"}
        lines = []
        assert score(bdb, m, out=lines.append)[0] is False and lines[0].startswith("SHOWN-CORRECT: 94/100 = 94.0%  GATE: FAIL")
        bdb.close()

        # score refuses marks made for another database.
        mfile = os.path.join(tmp, "marks.json")
        with open(mfile, "w") as f:
            json.dump({"db_sha256": "0" * 64, "memories": {}}, f)
        try:
            cmd_score(argparse.Namespace(db=path, marks=mfile))
            raise AssertionError("sha mismatch was accepted")
        except SystemExit as e:
            assert "different database" in str(e)

        # carry-over: same text+quote keeps its mark; a piece needs the same text, audio file and start.
        with open(mfile, "w") as f:
            json.dump({"memories": {"7": {"verdict": "wrong", "reason": "person", "text": "A", "quote": "qa"}},
                       "pieces": {"3": {"verdict": "right", "text": "p0", "audio_name": "x.m4a", "start_sec": 0}}}, f)
        prev_mem, prev_pcs = load_previous(mfile)
        assert prev_mem[("A", "qa")] == {"v": "wrong", "r": "person"} and ("A", "other quote") not in prev_mem
        assert prev_pcs[("p0", "x.m4a", 0)] == "right" and ("p0", "y.m4a", 0) not in prev_pcs

        # the sheet: text is escaped (cannot close the script), audio src is URL-encoded, #t= is added by the page.
        data = {"db_sha256": "ab", "reasons": REASONS, "failed": [], "pieces": [],
                "notes": [{"id": 1, "title": "T", "audio": audio_src("my rec #1.m4a"),
                           "memories": [{"id": 1, "type": "fact", "text": "x</script><b>y", "who": None, "due_date": None,
                                         "due_text": None, "queue": 0, "note_id": 1, "start_sec": 3, "quote": "q", "carry": None}]}]}
        page = render_sheet(data)
        assert "x</script><b>y" not in page and "x<\\/script><b>y" in page, "text must not close the script tag"
        assert "audio/my%20rec%20%231.m4a" in page and "my rec #1" not in page
        assert page.count("</script>") == 1
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
    e.add_argument("--notes", help="comma-separated note ids to use (newest first); --target then does not pick notes")
    e.add_argument("--resume", action="store_true",
                   help="reuse out/work/notes.sqlite and skip finished notes (a note stopped part way redoes only its undone sections)")
    e.add_argument("--skip-run", action="store_true", help="do not run the smoke; use the memories already in the db")
    e.set_defaults(fn=cmd_export)
    r = sub.add_parser("render", help="write sheet.html again from a finished export dir, without the model")
    r.add_argument("--out", required=True)
    r.add_argument("--audio", required=True)
    r.add_argument("--pieces", type=int, default=60)
    r.add_argument("--previous-marks")
    r.set_defaults(fn=cmd_render)
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
