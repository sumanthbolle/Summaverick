// summaverick quiz client. No question data or answers are embedded here — the
// server ships stems + options only, grades every response, and reveals the
// correct answer per-question after it is answered. localStorage holds ONLY the
// in-progress attempt id (a pointer); all real state lives in D1.
const $ = (id) => document.getElementById(id);
const ATTEMPT_KEY = "sv_attempt";

const state = { attemptId: null, questions: [], idx: 0, mode: null, done: new Set() };

async function api(path, opts = {}) {
  const res = await fetch(path, {
    ...opts,
    headers: { "content-type": "application/json", ...(opts.headers || {}) },
    credentials: "same-origin",
  });
  const data = await res.json().catch(() => ({}));
  if (!res.ok || data.ok === false) {
    throw new Error(data.message || data.error || `HTTP ${res.status}`);
  }
  return data;
}

function show(view) {
  for (const s of document.querySelectorAll("main > section")) s.hidden = true;
  const el = $("view-" + view);
  if (el) el.hidden = false;
  $("loading").hidden = true;
}
function fail(e) {
  $("loading").hidden = true;
  const el = $("error");
  el.hidden = false;
  el.textContent = "Error: " + (e && e.message ? e.message : e);
}

// ---- categories ----
async function loadCategories() {
  show("categories");
  const { categories } = await api("/api/quiz/categories");
  const grid = $("categoryGrid");
  grid.innerHTML = "";
  for (const c of categories) {
    const b = document.createElement("button");
    b.className = "cat";
    b.innerHTML =
      `<div class="icon">${c.icon || "📘"}</div>` +
      `<div class="name">${esc(c.name)}</div>` +
      `<div class="meta">${c.count} questions</div>`;
    b.onclick = () => loadModes(c);
    grid.appendChild(b);
  }
}

function loadModes(cat) {
  show("modes");
  $("modeCatName").textContent = cat.name;
  const list = $("modeList");
  list.innerHTML = "";
  for (const m of cat.modes || []) {
    const b = document.createElement("button");
    b.className = "mode";
    b.innerHTML =
      `<span><span class="name">${esc(m.name || m.id)}</span>` +
      `<div class="meta">${esc(m.desc || "")}</div></span>` +
      `<span class="badge">${m.count}${m.timer ? " · " + m.timer + "s" : ""}</span>`;
    b.onclick = () => startAttempt(cat.id, m.id);
    list.appendChild(b);
  }
}

// ---- attempt lifecycle ----
async function startAttempt(categoryId, mode) {
  try {
    show("categories");
    $("loading").hidden = false;
    const data = await api("/api/quiz/attempt", {
      method: "POST",
      body: JSON.stringify({ categoryId, mode }),
    });
    state.attemptId = data.attemptId;
    state.questions = data.questions;
    state.mode = data.mode;
    state.idx = 0;
    state.done = new Set();
    localStorage.setItem(ATTEMPT_KEY, data.attemptId);
    renderQuestion();
  } catch (e) {
    fail(e);
  }
}

async function resume(attemptId) {
  const data = await api("/api/quiz/attempt/" + attemptId);
  if (data.finished) {
    localStorage.removeItem(ATTEMPT_KEY);
    return loadCategories();
  }
  state.attemptId = attemptId;
  state.questions = data.questions;
  state.mode = { id: data.mode };
  state.done = new Set((data.responses || []).map((r) => r.questionId));
  state.idx = data.questions.findIndex((q) => !q.answered);
  if (state.idx < 0) state.idx = data.questions.length - 1;
  renderQuestion();
}

function renderQuestion() {
  show("quiz");
  const q = state.questions[state.idx];
  $("progress").textContent = `Question ${state.idx + 1} / ${state.questions.length}`;
  $("feedback").hidden = true;
  $("nextBtn").hidden = true;
  $("finishBtn").hidden = true;
  const submit = $("submitBtn");
  submit.hidden = false;
  submit.disabled = true;

  const sel = new Set();
  const card = $("questionCard");
  card.innerHTML = `<p class="stem">${esc(q.stem)}</p>`;
  q.options.forEach((opt, i) => {
    const b = document.createElement("button");
    b.className = "opt";
    b.textContent = opt;
    b.onclick = () => {
      if (q.isMulti) {
        b.classList.toggle("sel");
        sel.has(i) ? sel.delete(i) : sel.add(i);
      } else {
        card.querySelectorAll(".opt").forEach((o) => o.classList.remove("sel"));
        b.classList.add("sel");
        sel.clear();
        sel.add(i);
      }
      submit.disabled = sel.size === 0;
    };
    card.appendChild(b);
  });

  const started = Date.now();
  submit.onclick = async () => {
    submit.disabled = true;
    try {
      const r = await api(`/api/quiz/attempt/${state.attemptId}/response`, {
        method: "POST",
        body: JSON.stringify({
          questionId: q.id,
          chosen: [...sel],
          msTaken: Date.now() - started,
        }),
      });
      state.done.add(q.id);
      revealAnswer(card, r);
      showFeedback(r);
      submit.hidden = true;
      const last = state.idx >= state.questions.length - 1;
      $(last ? "finishBtn" : "nextBtn").hidden = false;
    } catch (e) {
      fail(e);
    }
  };
}

function revealAnswer(card, r) {
  const opts = card.querySelectorAll(".opt");
  opts.forEach((o, i) => {
    o.disabled = true;
    if (r.correctOptions.includes(i)) o.classList.add("correct");
    if (o.classList.contains("sel") && !r.correctOptions.includes(i))
      o.classList.add("wrong");
  });
}
function showFeedback(r) {
  const fb = $("feedback");
  fb.hidden = false;
  fb.className = "feedback " + (r.correct ? "ok" : "bad");
  fb.innerHTML =
    `<strong>${r.correct ? "Correct" : "Incorrect"}</strong>` +
    (r.explanation ? `<div class="exp">${esc(r.explanation)}</div>` : "");
}

$("nextBtn").onclick = () => {
  state.idx = Math.min(state.idx + 1, state.questions.length - 1);
  renderQuestion();
};
$("finishBtn").onclick = finish;

async function finish() {
  try {
    const data = await api(`/api/quiz/attempt/${state.attemptId}/finish`, {
      method: "POST",
    });
    localStorage.removeItem(ATTEMPT_KEY);
    show("results");
    $("scoreBox").textContent =
      `${data.correctCount} / ${data.total}  (${Math.round(data.score * 100)}%)`;
    const list = $("reviewList");
    list.innerHTML = "";
    for (const item of data.review) {
      const div = document.createElement("div");
      div.className = "item";
      const chosenTxt = item.chosen.map((i) => item.options[i]).join(", ") || "—";
      const correctTxt = item.correctOptions.map((i) => item.options[i]).join(", ");
      div.innerHTML =
        `<div class="q">${item.correct ? "✅" : "❌"} ${esc(item.stem)}</div>` +
        `<div class="exp">Your answer: ${esc(chosenTxt)} · Correct: ${esc(correctTxt)}</div>` +
        (item.explanation ? `<div class="exp">${esc(item.explanation)}</div>` : "");
      list.appendChild(div);
    }
  } catch (e) {
    fail(e);
  }
}

async function loadHistory() {
  try {
    const { attempts } = await api("/api/quiz/history");
    show("history");
    const list = $("historyList");
    list.innerHTML = attempts.length ? "" : "<p class='tag'>No attempts yet.</p>";
    for (const a of attempts) {
      const div = document.createElement("div");
      div.className = "item";
      const when = new Date(a.startedAt).toLocaleString();
      const score = a.score == null ? "in progress" : Math.round(a.score * 100) + "%";
      div.innerHTML = `<div class="q">${esc(a.categoryId)} · ${esc(a.mode)}</div>` +
        `<div class="exp">${when} · ${a.total} q · ${score}</div>`;
      list.appendChild(div);
    }
  } catch (e) {
    fail(e);
  }
}

function esc(s) {
  return String(s ?? "").replace(/[&<>"']/g, (c) =>
    ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c])
  );
}

for (const b of document.querySelectorAll("[data-go]")) {
  b.onclick = () => (b.dataset.go === "categories" ? loadCategories() : null);
}
$("historyLink").onclick = (e) => {
  e.preventDefault();
  loadHistory();
};

// boot: resume in-progress attempt if we have a pointer, else show categories.
(async () => {
  try {
    const saved = localStorage.getItem(ATTEMPT_KEY);
    if (saved) {
      await resume(saved).catch(() => loadCategories());
    } else {
      await loadCategories();
    }
  } catch (e) {
    fail(e);
  }
})();
