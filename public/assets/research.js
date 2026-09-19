// summaverick research client. POSTs to /api/research and renders the answer
// plus the full pipeline trace. All rendering uses textContent / DOM nodes (no
// innerHTML with server strings) so untrusted evidence text can never inject.
import { mountChrome } from "/assets/app.js";
mountChrome({ active: "tools" });

const $ = (id) => document.getElementById(id);

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

// ---- tiny DOM helpers -----------------------------------------------------
function el(tag, opts = {}, ...kids) {
  const n = document.createElement(tag);
  if (opts.class) n.className = opts.class;
  if (opts.text != null) n.textContent = String(opts.text);
  if (opts.href) n.href = opts.href;
  if (opts.attrs) for (const [k, v] of Object.entries(opts.attrs)) n.setAttribute(k, v);
  for (const k of kids) if (k) n.append(k);
  return n;
}
function badge(on, labelOn = "fired", labelOff = "clear", warn = false) {
  const cls = on ? (warn ? "badge warn" : "badge on") : "badge off";
  return el("span", { class: cls, text: on ? labelOn : labelOff });
}
function pct(n) {
  return `${Math.round((n || 0) * 100)}%`;
}

function section(title, ...body) {
  const s = el("section", { class: "section" });
  s.append(el("h2", { text: title }));
  const panel = el("div", { class: "card" });
  for (const b of body) if (b) panel.append(b);
  s.append(panel);
  return s;
}

// ---- renderers ------------------------------------------------------------
function renderClassification(c) {
  const dl = el("dl", { class: "kv" });
  const add = (k, v) => { dl.append(el("dt", { text: k }), el("dd", { text: v })); };
  add("Domain", c.domain);
  add("Intent", c.intent);
  add("Confidence", c.confidence);
  add("Rationale", c.rationale);
  const flags = el("div", { class: "chips" });
  const f = (label, on) => on && flags.append(el("span", { class: "chip", text: label }));
  f("SDK docs", c.requiresSdkDocs);
  f("Product docs", c.requiresProductDocs);
  f("Repository", c.requiresRepositoryContext);
  f("Live instance", c.requiresLiveInstance);
  if (c.requestedReleaseFamily) f(`release: ${c.requestedReleaseFamily}`, true);
  const mods = el("div", { class: "chips" });
  for (const m of c.modules || []) mods.append(el("span", { class: "chip", text: m }));
  const wrap = el("div");
  wrap.append(dl, el("dt", { class: "muted", text: "modules" }), mods,
    el("dt", { class: "muted", text: "signals" }), flags);
  return wrap;
}

function renderLayers(layers, answeredBy, candidateCount) {
  const list = el("ul", { class: "layer-list" });
  layers.forEach((L, i) => {
    const row = el("li", { class: "layer" + (L.hit ? " hit" : L.planned ? "" : " dim") });
    row.append(el("div", { class: "num", text: String(i + 1) }));
    const body = el("div", { class: "body" });
    body.append(el("strong", { text: L.layer }));
    const bits = [];
    bits.push(L.planned ? "planned" : "not planned");
    bits.push(`${L.candidateCount} candidate doc(s)`);
    if (L.answered) bits.push("answered here");
    body.append(el("small", { text: bits.join(" · ") }));
    row.append(body);
    row.append(badge(L.hit, "hit", "miss"));
    list.append(row);
  });
  const summary = el("p", { class: "muted",
    text: `${candidateCount} total candidate document(s) after ranking · top layer: ${answeredBy || "none"}` });
  const wrap = el("div");
  wrap.append(list, summary);
  return wrap;
}

function renderEvidence(evidence) {
  if (!evidence.length) return el("p", { class: "muted", text: "No evidence retrieved." });
  const tbl = el("table", { class: "tbl" });
  const head = el("tr");
  ["#", "Source", "Title", "Relevance", "Authority"].forEach((h) => head.append(el("th", { text: h })));
  tbl.append(head);
  evidence.forEach((e, i) => {
    const tr = el("tr");
    tr.append(el("td", { text: String(i + 1) }));
    tr.append(el("td", { text: e.sourceType }));
    const titleCell = el("td");
    if (e.url) titleCell.append(el("a", { text: e.title, href: e.url, attrs: { target: "_blank", rel: "noopener noreferrer" } }));
    else titleCell.textContent = e.title;
    tr.append(titleCell);
    tr.append(el("td", { text: e.relevanceScore }));
    tr.append(el("td", { text: e.authorityScore }));
    tbl.append(tr);
  });
  return el("div", { class: "scroll-x" }, tbl);
}

function renderClaims(claims) {
  if (!claims.length) return el("p", { class: "muted", text: "No claims were extracted." });
  const wrap = el("div");
  for (const c of claims) {
    const row = el("div", { class: "claim" });
    const head = el("p", { class: "c-text" });
    head.append(badge(c.verdict === "supported", "supported", "unsupported", true), document.createTextNode(" "));
    head.append(document.createTextNode(c.text));
    row.append(head);
    if (c.evidence) {
      const ev = el("div", { class: "c-ev" });
      ev.append(document.createTextNode(`↳ ${c.evidence.sourceType}: `));
      if (c.evidence.url) ev.append(el("a", { text: c.evidence.title, href: c.evidence.url, attrs: { target: "_blank", rel: "noopener noreferrer" } }));
      else ev.append(document.createTextNode(c.evidence.title));
      row.append(ev);
    } else {
      row.append(el("div", { class: "c-ev", text: "↳ no supporting evidence" }));
    }
    wrap.append(row);
  }
  return wrap;
}

function renderGates(gates) {
  const wrap = el("div");
  for (const g of gates) {
    const row = el("div", { class: "gate" });
    const name = el("div", { class: "g-name" });
    name.append(document.createTextNode(g.gate + " "), badge(g.fired));
    row.append(name);
    row.append(el("div", { class: "g-detail", text: g.detail }));
    wrap.append(row);
  }
  return wrap;
}

function renderEval(ev) {
  const meters = el("div", { class: "meters" });
  const meter = (val, lbl) => {
    const m = el("div", { class: "meter" });
    m.append(el("div", { class: "m-val", text: val }));
    m.append(el("div", { class: "m-lbl", text: lbl }));
    return m;
  };
  meters.append(meter(pct(ev.passRate), `overall pass (${ev.passed}/${ev.total})`));
  meters.append(meter(pct(ev.intentAccuracy), `intent accuracy (${ev.intentEvaluated} cases)`));
  meters.append(meter(pct(ev.adversarialBlockRate), `adversarial blocked (${ev.adversarialTotal})`));
  const cats = el("div", { class: "chips" });
  for (const [cat, b] of Object.entries(ev.byCategory || {})) {
    cats.append(el("span", { class: "chip", text: `${cat}: ${b.passed}/${b.total}` }));
  }
  const wrap = el("div");
  wrap.append(meters, el("p", { class: "muted", text: "By category:" }), cats);
  return wrap;
}

function renderVerification(v, sec, llm) {
  const dl = el("dl", { class: "kv" });
  const add = (k, node) => { dl.append(el("dt", { text: k }), node instanceof Node ? el("dd", {}, node) : el("dd", { text: node })); };
  if (v) {
    add("Verifier ok", badge(v.ok, "yes", "no"));
    add("Confidence", v.confidence);
    add("Citations", String(v.citationCount));
    add("Unsupported claims", String(v.unsupportedClaimCount));
    if (v.issues && v.issues.length) add("Issues", v.issues.join("; "));
  }
  add("Read-only", badge(sec.readOnly, "enforced", "off"));
  add("Prompt-injection", badge(sec.promptInjectionDetected, "detected", "none", true));
  add("Live instance", badge(sec.liveInstanceEnabled, "enabled", "disabled"));
  const llmNode = el("span");
  llmNode.append(badge(llm.used, "used", "not used"));
  llmNode.append(document.createTextNode(` ${llm.provider}${llm.model ? " · " + llm.model : ""}`));
  if (llm.error) llmNode.append(document.createTextNode(` (${llm.error})`));
  add("Answer synthesis", llmNode);
  return dl;
}

function renderEvents(events) {
  if (!events || !events.length) return null;
  const d = el("details", { class: "events" });
  d.append(el("summary", { text: `Raw trace events (${events.length})` }));
  const ol = el("ol");
  for (const e of events) ol.append(el("li", { text: `${e.name}` }));
  d.append(ol);
  return d;
}

function renderTrace(t) {
  const root = $("trace");
  root.textContent = "";

  root.append(section("1 · Query classification", renderClassification(t.classification)));

  if (t.plan) {
    const dl = el("dl", { class: "kv" });
    const add = (k, v) => { dl.append(el("dt", { text: k }), el("dd", { text: v })); };
    add("Planned sources", (t.plan.sources || []).join(", ") || "none");
    add("Release family", t.plan.releaseFamily || "—");
    add("SDK version", t.plan.sdkVersion || "not detected");
    add("Max tool calls", String(t.plan.maximumToolCalls));
    if (t.expansions && t.expansions.length) add("Query expansions", t.expansions.join(" · "));
    root.append(section("2 · Research plan", dl));
  }

  root.append(section("3 · Retrieval layers", renderLayers(t.layers, t.answeredBy, t.candidateDocumentCount)));
  root.append(section("4 · Ranked evidence", renderEvidence(t.evidence)));
  root.append(section("5 · Per-claim evidence verification", renderClaims(t.claims)));
  root.append(section("6 · Evidence gates", renderGates(t.evidenceGates)));
  root.append(section("7 · Verification, security & synthesis",
    renderVerification(t.verification, t.security, t.llm)));
  root.append(section("8 · Adversarial + regression eval scores", renderEval(t.evalScores)));

  const events = renderEvents(t.events);
  if (events) root.append(section("Trace log", events));
}

// ---- flow -----------------------------------------------------------------
async function run(query) {
  $("error").hidden = true;
  $("result").hidden = true;
  $("loading").hidden = false;
  $("askBtn").disabled = true;
  try {
    const data = await api("/api/research", {
      method: "POST",
      body: JSON.stringify({ query }),
    });
    // `mode` says what this text is: prose from an answer model, a list of
    // documentation matches, or no close match. Never label it "answer" blindly.
    const answer = $("answer");
    answer.textContent = "";
    if (data.notice) answer.append(el("p", { class: "muted", text: data.notice }));
    answer.append(el("div", { text: data.answer || "(nothing returned)" }));
    for (const src of data.sources || []) {
      const row = el("p", { class: "muted" });
      row.append(
        src.url
          ? el("a", { href: src.url, text: src.title })
          : el("span", { text: src.title })
      );
      if (src.snippet) row.append(el("span", { text: ` — ${src.snippet}` }));
      answer.append(row);
    }
    renderTrace(data.trace);
    $("result").hidden = false;
  } catch (e) {
    const el2 = $("error");
    el2.hidden = false;
    el2.textContent = "Error: " + (e && e.message ? e.message : e);
  } finally {
    $("loading").hidden = true;
    $("askBtn").disabled = false;
  }
}

$("askForm").addEventListener("submit", (e) => {
  e.preventDefault();
  const q = $("query").value.trim();
  if (q) run(q);
});
for (const b of document.querySelectorAll(".examples button")) {
  b.addEventListener("click", () => {
    $("query").value = b.dataset.ex;
    run(b.dataset.ex);
  });
}
