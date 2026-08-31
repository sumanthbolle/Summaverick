/*
 * Ask Summaverick — full-page composer over the live research agent. Submits to
 * /api/research/stream and renders the trace + answer as it arrives. Same
 * backend as the home preview; here it's the whole page.
 */

import { el, $ } from "./lib/dom.js";
import { streamResearch } from "./lib/agent-stream.js";
import { initChrome } from "./chrome.js";

const CHIPS = [
  "How do I use GlideRecord to query the incident table?",
  "How do I define a Business Rule with the Fluent SDK?",
  "What is the CMDB and how do CI relationships work?",
  "How do ACLs evaluate on a table?",
];

function initAsk() {
  const form = $("[data-ask-form]");
  const input = $("[data-ask-input]");
  const chipsWrap = $("[data-ask-chips]");
  const results = $("[data-ask-results]");
  const trace = $("[data-ask-trace]");
  if (!form || !input) return;

  const tick = (k) => (k === "block" ? "✕" : k === "active" ? "○" : k === "muted" ? "·" : "✓");
  let stepIndex = 0;

  const addStep = (label, detail, kind) => {
    const row = el("div", { class: "trace-step", dataset: { kind: kind || "ok" } }, [
      el("span", { class: "tick", text: tick(kind) }),
      el("div", {}, [el("span", { class: "k", text: label }), " ", el("span", { class: "v", text: detail || "" })]),
    ]);
    trace.append(row);
    const i = stepIndex++;
    requestAnimationFrame(() => setTimeout(() => row.classList.add("show"), 20 + Math.min(i, 6) * 40));
  };

  const addAnswer = (a) => {
    const verOk = a.verification ? a.verification.ok : null;
    const head = el("div", { class: "a-head" }, [
      el("span", { class: "a-badge", text: a.llmUsed ? `model answer${a.llmModel ? " · " + a.llmModel : ""}` : "evidence-backed draft" }),
      a.verification ? el("span", { class: "a-badge", "data-ok": String(verOk), text: `${a.verification.citationCount} citation(s) · ${verOk ? "verified" : "unverified"}` }) : null,
    ]);
    const note = !a.llmUsed
      ? el("div", { class: "small text-mute", style: "margin-top:var(--space-2)",
          text: a.llmError ? `model unavailable (${a.llmError}) — retrieval + verification ran; showing the evidence-backed draft` : "no model key configured — showing the evidence-backed draft" })
      : null;
    const cites = (a.citations || []).length
      ? el("div", { class: "a-cites" }, a.citations.map((c) =>
          el("span", { class: "cite", html: "&#8250; " + (c.sourceType ? c.sourceType + " · " : "") + c.title })))
      : null;
    const block = el("div", { class: "trace-answer" }, [head, el("div", { class: "a-text", text: a.text }), note, cites]);
    trace.append(block);
    requestAnimationFrame(() => setTimeout(() => block.classList.add("show"), 30));
  };

  const run = $("[data-ask-run]");
  const setBusy = (b) => { if (run) { run.disabled = b; run.textContent = b ? "Asking…" : "Ask"; } };

  async function ask(query) {
    const q = (query || "").trim();
    if (!q) return;
    results.hidden = false;
    stepIndex = 0;
    trace.replaceChildren();
    setBusy(true);
    try {
      await streamResearch(q, {
        stage: (d) => addStep(d.label, d.detail, d.kind),
        blocked: (d) => addStep("blocked", d.reason, "block"),
        answer: (d) => addAnswer(d),
        error: (d) => addStep("error", d.message || "the run failed", "block"),
        rateLimited: (msg) => addStep("rate limited", msg, "block"),
      });
    } catch (err) {
      addStep("unavailable", "the agent endpoint isn't reachable from here — try again shortly", "block");
    } finally {
      setBusy(false);
    }
  }

  chipsWrap?.replaceChildren(
    ...CHIPS.map((c) => el("button", { class: "chip", type: "button", text: c, onclick: () => { input.value = c; ask(c); } }))
  );

  form.addEventListener("submit", (e) => { e.preventDefault(); ask(input.value); });
  // Enter submits; Shift+Enter for a newline.
  input.addEventListener("keydown", (e) => {
    if (e.key === "Enter" && !e.shiftKey) { e.preventDefault(); ask(input.value); }
  });
}

initChrome();
initAsk();
