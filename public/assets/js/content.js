/*
 * content.js — agent preview and lead form. Capability / work / engagement
 * copy lives in the HTML so the page is complete without JS.
 */

import { el, $, $$, prefersReducedMotion } from "./lib/dom.js";
import { streamResearch } from "./lib/agent-stream.js";
import { AGENT_TRACE, INJECTION_MARKERS } from "./data/consulting.js";

function initReveal() {
  if (prefersReducedMotion()) return;
  const targets = $$(".capability, .work-card, .engage-card, .section-head, .mast-panel");
  if (!("IntersectionObserver" in window) || !targets.length) return;
  targets.forEach((t) => {
    t.style.opacity = "0";
    t.style.transform = "translateY(12px)";
    t.style.transition = "opacity .5s var(--ease-out), transform .5s var(--ease-out)";
  });
  const io = new IntersectionObserver(
    (entries) => {
      for (const en of entries) {
        if (en.isIntersecting) {
          en.target.style.opacity = "1";
          en.target.style.transform = "none";
          io.unobserve(en.target);
        }
      }
    },
    { rootMargin: "0px 0px -8% 0px" }
  );
  targets.forEach((t) => io.observe(t));
}

/* Agent preview. Streams the real trace from POST /api/research/stream.
 * Falls back to a captured trace when the Worker isn't present. */
function initAgentPreview() {
  const form = $("[data-agent-form]");
  const input = $("[data-agent-input]");
  const traceEl = $("[data-agent-trace]");
  const runBtn = $("[data-agent-run]");
  if (!form || !traceEl) return;

  let stepIndex = 0;
  const tick = (kind) =>
    kind === "block" ? "✕" : kind === "active" ? "○" : kind === "muted" ? "·" : "✓";

  const clear = () => {
    stepIndex = 0;
    traceEl.replaceChildren();
  };
  const addStep = (label, detail, kind) => {
    const row = el("div", { class: "trace-step", dataset: { kind: kind || "ok" } }, [
      el("span", { class: "tick", text: tick(kind) }),
      el("div", {}, [
        el("span", { class: "k", text: label }),
        " ",
        el("span", { class: "v", text: detail || "" }),
      ]),
    ]);
    traceEl.append(row);
    const i = stepIndex++;
    requestAnimationFrame(() =>
      setTimeout(() => row.classList.add("show"), 20 + Math.min(i, 6) * 40)
    );
    return row;
  };
  const addAnswer = (a) => {
    const verOk = a.verification ? a.verification.ok : null;
    const head = el("div", { class: "a-head" }, [
      el("span", {
        class: "a-badge",
        text: a.llmUsed
          ? `model answer${a.llmModel ? " · " + a.llmModel : ""}`
          : "evidence-backed draft",
      }),
      a.verification
        ? el("span", {
            class: "a-badge",
            "data-ok": String(verOk),
            text: `${a.verification.citationCount} citation(s) · ${verOk ? "verified" : "unverified"}`,
          })
        : null,
    ]);
    const note = !a.llmUsed
      ? el("div", {
          class: "small text-mute",
          style: "margin-top:var(--space-2)",
          text: a.llmError
            ? `model unavailable (${a.llmError}) — retrieval + verification ran; showing the evidence-backed draft`
            : "no model key configured — showing the evidence-backed draft",
        })
      : null;
    const cites = (a.citations || []).length
      ? el("div", { class: "a-cites" }, a.citations.map((c) =>
          el("span", {
            class: "cite",
            html: "&#8250; " + (c.sourceType ? c.sourceType + " · " : "") + c.title,
          })
        ))
      : null;
    const block = el("div", { class: "trace-answer" }, [
      head,
      el("div", { class: "a-text", text: a.text }),
      note,
      cites,
    ]);
    traceEl.append(block);
    requestAnimationFrame(() => setTimeout(() => block.classList.add("show"), 30));
  };

  const setBusy = (busy) => {
    if (runBtn) {
      runBtn.disabled = busy;
      runBtn.textContent = busy ? "Running…" : "Run";
    }
  };

  const streamLive = (query) =>
    streamResearch(query, {
      stage: (d) => addStep(d.label, d.detail, d.kind),
      blocked: (d) => addStep("blocked", d.reason, "block"),
      answer: (d) => addAnswer(d),
      error: (d) => addStep("error", d.message || "the run failed", "block"),
      rateLimited: (msg) => addStep("rate limited", msg, "block"),
    });

  function playCanned(query) {
    const s = (query || "").toLowerCase();
    const blocked = INJECTION_MARKERS.some((m) => s.includes(m));
    const steps = blocked ? AGENT_TRACE.blocked : AGENT_TRACE.ok;
    steps.forEach((step, i) =>
      setTimeout(() => addStep(step.k, step.v, step.kind), 90 + i * 240)
    );
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
    if (data.get("company_url")) {
      status.textContent = "Thank you. We will reply shortly.";
      form.reset();
      return;
    }
    if (!data.get("email")) {
      status.textContent = "An email address is required so we can reply.";
      return;
    }
    status.textContent = "Thank you. We will reply shortly.";
    form.reset();
  });
}

export function initContent(opts = {}) {
  // The scroll experience runs its own richer reveals; only fall back to the
  // plain IntersectionObserver reveal when it didn't engage.
  if (opts.reveal !== false) initReveal();
  initAgentPreview();
  initLeadForm();
}
