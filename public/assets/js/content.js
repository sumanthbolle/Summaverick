/*
 * content.js — below-the-fold: work cards, engagement models, the agent
 * preview (client-side trace playback), and the lead form. No scroll timelines
 * here; a light IntersectionObserver reveal keeps it alive without GSAP.
 */

import { el, $, $$, prefersReducedMotion } from "./lib/dom.js";
import { WORK, ENGAGEMENTS, PILLARS, AGENT_TRACE, INJECTION_MARKERS } from "./data/consulting.js";

function workCard(w) {
  return el("article", { class: "card stack" }, [
    el("span", { class: "eyebrow", text: w.client }),
    el("h3", { class: "h4", text: w.title }),
    el("p", { class: "text-dim small", text: w.problem }),
    el("p", { class: "small", text: w.approach }),
    el("p", { class: "small text-mute", html: "<b>Constraint I held:</b> " + w.constraint }),
    el("div", { class: "metric-row" }, w.stack.map((s) => el("span", { class: "badge", text: s }))),
    w.illustrative ? el("span", { class: "small text-mute", style: "opacity:.7", text: "metrics pending confirmation" }) : null,
  ]);
}

function engageCard(e) {
  return el("article", { class: "card stack" }, [
    el("div", { style: "display:flex;justify-content:space-between;align-items:baseline;gap:var(--space-3)" }, [
      el("h3", { class: "h4", text: e.name }),
      el("span", { class: "badge", text: e.length }),
    ]),
    el("p", { class: "small", text: e.what }),
    el("p", { class: "small text-mute", html: "<b>You leave with:</b> " + e.end }),
  ]);
}

function pillarCard(p, i) {
  return el("article", { class: "card stack" }, [
    el("span", { class: "eyebrow", text: String(i + 1).padStart(2, "0") + " · " + p.label }),
    el("p", { text: p.text }),
  ]);
}

function renderStatic() {
  const pillars = $("[data-pillars]");
  if (pillars) pillars.replaceChildren(...PILLARS.map(pillarCard));
  const work = $("[data-work]");
  if (work) work.replaceChildren(...WORK.map(workCard));
  const engage = $("[data-engage]");
  if (engage) engage.replaceChildren(...ENGAGEMENTS.map(engageCard));
  const yr = $("[data-year]");
  if (yr) yr.textContent = String(new Date().getFullYear());
}

/* Reveal-on-scroll for the plain sections (no pinning). */
function initReveal() {
  if (prefersReducedMotion()) return; // content stays visible, no reveal
  const targets = $$(".card, .section-head");
  if (!("IntersectionObserver" in window)) return;
  targets.forEach((t) => {
    t.style.opacity = "0";
    t.style.transform = "translateY(18px)";
    t.style.transition = "opacity .6s var(--ease-out), transform .6s var(--ease-out)";
  });
  const io = new IntersectionObserver((entries) => {
    for (const en of entries) {
      if (en.isIntersecting) {
        en.target.style.opacity = "1";
        en.target.style.transform = "none";
        io.unobserve(en.target);
      }
    }
  }, { rootMargin: "0px 0px -10% 0px" });
  targets.forEach((t) => io.observe(t));
}

/* Agent preview. Streams the real trace from POST /api/research/stream, stage
 * by stage. If the endpoint is unreachable (e.g. the page is served statically,
 * without the Worker), it falls back to a captured trace so the demo still
 * runs. A query that trips the injection detector is blocked server-side and the
 * block is shown here — the firewall, live. */
function initAgentPreview() {
  const form = $("[data-agent-form]");
  const input = $("[data-agent-input]");
  const traceEl = $("[data-agent-trace]");
  const runBtn = $("[data-agent-run]");
  if (!form || !traceEl) return;

  let stepIndex = 0;
  const tick = (kind) => (kind === "block" ? "✕" : kind === "active" ? "○" : kind === "muted" ? "·" : "✓");

  const clear = () => { stepIndex = 0; traceEl.replaceChildren(); };
  const addStep = (label, detail, kind) => {
    const row = el("div", { class: "trace-step", dataset: { kind: kind || "ok" } }, [
      el("span", { class: "tick", text: tick(kind) }),
      el("div", {}, [el("span", { class: "k", text: label }), " ", el("span", { class: "v", text: detail || "" })]),
    ]);
    traceEl.append(row);
    const i = stepIndex++;
    requestAnimationFrame(() => setTimeout(() => row.classList.add("show"), 20 + Math.min(i, 6) * 40));
    return row;
  };
  const addAnswer = (a) => {
    const verOk = a.verification ? a.verification.ok : null;
    const head = el("div", { class: "a-head" }, [
      el("span", { class: "a-badge", text: a.llmUsed ? "model answer" : "evidence-backed draft" }),
      a.verification ? el("span", { class: "a-badge", "data-ok": String(verOk), text: `${a.verification.citationCount} citation(s) · ${verOk ? "verified" : "unverified"}` }) : null,
    ]);
    const cites = (a.citations || []).length
      ? el("div", { class: "a-cites" }, a.citations.map((c) =>
          el("span", { class: "cite", html: "&#8250; " + (c.sourceType ? c.sourceType + " · " : "") + c.title })))
      : null;
    const block = el("div", { class: "trace-answer" }, [head, el("div", { class: "a-text", text: a.text }), cites]);
    traceEl.append(block);
    requestAnimationFrame(() => setTimeout(() => block.classList.add("show"), 30));
  };

  const setBusy = (busy) => { if (runBtn) { runBtn.disabled = busy; runBtn.textContent = busy ? "Running…" : "Run"; } };

  function handleEvent(type, data) {
    if (type === "stage") addStep(data.label, data.detail, data.kind);
    else if (type === "blocked") addStep("blocked", data.reason, "block");
    else if (type === "answer") addAnswer(data);
    else if (type === "error") addStep("error", data.message || "the run failed", "block");
  }

  async function streamLive(query) {
    const res = await fetch("/api/research/stream", {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({ query }),
    });
    if (!res.ok || !res.body) {
      let msg = `HTTP ${res.status}`;
      try { const j = await res.json(); msg = j.message || msg; } catch (e) {}
      if (res.status === 429) { addStep("rate limited", msg, "block"); return true; }
      throw new Error(msg); // 4xx/5xx without a stream → try fallback
    }
    const reader = res.body.getReader();
    const dec = new TextDecoder();
    let buf = "";
    for (;;) {
      const { value, done } = await reader.read();
      if (done) break;
      buf += dec.decode(value, { stream: true });
      let idx;
      while ((idx = buf.indexOf("\n\n")) >= 0) {
        const frame = buf.slice(0, idx); buf = buf.slice(idx + 2);
        let type = "message", payload = "";
        for (const line of frame.split("\n")) {
          if (line.startsWith("event:")) type = line.slice(6).trim();
          else if (line.startsWith("data:")) payload += line.slice(5).trim();
        }
        if (!payload) continue;
        try { handleEvent(type, JSON.parse(payload)); } catch (e) {}
      }
    }
    return true;
  }

  /* Canned fallback for static hosting. */
  function playCanned(query) {
    const s = (query || "").toLowerCase();
    const blocked = INJECTION_MARKERS.some((m) => s.includes(m));
    const steps = blocked ? AGENT_TRACE.blocked : AGENT_TRACE.ok;
    steps.forEach((step, i) => setTimeout(() => addStep(step.k, step.v, step.kind), 90 + i * 240));
  }

  form.addEventListener("submit", async (e) => {
    e.preventDefault();
    const q = (input.value || "").trim();
    if (!q) return;
    clear();
    setBusy(true);
    try {
      await streamLive(q);
    } catch (err) {
      addStep("preview", "live endpoint unavailable — playing a captured trace", "muted");
      playCanned(q);
    } finally {
      setBusy(false);
    }
  });
}

function initLeadForm() {
  const form = $("[data-lead-form]");
  const status = $("[data-lead-status]");
  if (!form) return;
  form.addEventListener("submit", (e) => {
    e.preventDefault();
    const data = new FormData(form);
    if (data.get("company_url")) { // honeypot filled → silently drop
      status.textContent = "Thanks — I'll be in touch.";
      form.reset();
      return;
    }
    if (!data.get("email")) { status.textContent = "An email helps me reply."; return; }
    // Endpoint (POST /api/leads) arrives with T11. For now, acknowledge.
    status.textContent = "Thanks — I'll be in touch. (Delivery wires up in T11.)";
    form.reset();
  });
}

export function initContent() {
  renderStatic();
  initReveal();
  initAgentPreview();
  initLeadForm();
}
