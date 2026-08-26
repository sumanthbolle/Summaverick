/*
 * content.js — below-the-fold: work cards, engagement models, the agent
 * preview (client-side trace playback), and the lead form. No scroll timelines
 * here; a light IntersectionObserver reveal keeps it alive without GSAP.
 */

import { el, $, $$, prefersReducedMotion } from "./lib/dom.js";
import { WORK, ENGAGEMENTS, AGENT_TRACE, INJECTION_MARKERS } from "./data/consulting.js";

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

function renderStatic() {
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

/* Agent preview: plays a captured trace step-by-step. A query that trips an
 * injection marker plays the blocked trace instead — the firewall, live. */
function initAgentPreview() {
  const form = $("[data-agent-form]");
  const input = $("[data-agent-input]");
  const traceEl = $("[data-agent-trace]");
  if (!form || !traceEl) return;

  let timers = [];
  const clear = () => { timers.forEach(clearTimeout); timers = []; traceEl.replaceChildren(); };

  const looksLikeInjection = (q) => {
    const s = q.toLowerCase();
    return INJECTION_MARKERS.some((m) => s.includes(m));
  };

  const play = (steps) => {
    clear();
    steps.forEach((step, i) => {
      const row = el("div", { class: "trace-step", dataset: { kind: step.kind } }, [
        el("span", { class: "tick", text: step.kind === "block" ? "✕" : "✓" }),
        el("div", {}, [el("span", { class: "k", text: step.k }), " ", el("span", { class: "v", text: step.v })]),
      ]);
      traceEl.append(row);
      timers.push(setTimeout(() => row.classList.add("show"), 90 + i * 260));
    });
  };

  form.addEventListener("submit", (e) => {
    e.preventDefault();
    const q = (input.value || "").trim();
    play(looksLikeInjection(q) ? AGENT_TRACE.blocked : AGENT_TRACE.ok);
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
