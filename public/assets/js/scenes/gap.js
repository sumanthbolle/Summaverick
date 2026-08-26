/*
 * Scene 2 — The gap. Same question, two answers. The generic one's claims fail
 * verification and strike through; the agent's claims gain citations and hold.
 * Driven by the GAP fixture. Fully scrubbed, so scroll-up reverses it.
 */

import { el } from "../lib/dom.js";
import { GAP } from "../data/fixtures.js";

let st = null;

function buildClaim(c) {
  const status = el("span", { class: "status", "aria-hidden": "true", text: c.state === "ok" ? "✓" : "✕" });
  const txt = el("div", { class: "txt" }, [
    el("span", { text: c.txt }),
    c.cite ? el("span", { class: "cite", html: "&#8250; " + c.cite }) : null,
  ]);
  return el("div", { class: "claim", dataset: { targetState: c.state } }, [status, txt]);
}

function buildAnswer(a, fate) {
  const head = el("div", { class: "answer-head" }, [
    el("span", { class: "who" }, [a.who]),
    el("span", { class: "small text-mute", text: fate === "dissolve" ? "unverified" : "verified" }),
  ]);
  const body = el("div", { class: "answer-body" }, a.claims.map(buildClaim));
  return el("div", { class: "answer", dataset: { fate } }, [head, body]);
}

export function init(section, ctx) {
  const { gsap, reduced } = ctx;
  const wrap = section.querySelector("[data-gap-answers]");
  const qEl = section.querySelector("[data-gap-q]");
  qEl.textContent = "“" + GAP.question + "”";

  const generic = buildAnswer(GAP.generic, "dissolve");
  const agent = buildAnswer(GAP.agent, "keep");
  wrap.replaceChildren(generic, agent);

  const claims = [...wrap.querySelectorAll(".claim")];
  const cites = [...wrap.querySelectorAll(".cite")];

  if (reduced || !gsap) {
    // Final state, no motion: apply the verified/failed styling and show cites.
    claims.forEach((c) => (c.dataset.state = c.dataset.targetState));
    return;
  }

  // Start neutral.
  gsap.set([generic, agent], { opacity: 0, y: 30 });
  gsap.set(cites, { opacity: 0, scale: 0.9, transformOrigin: "left center" });

  const tl = gsap.timeline({
    scrollTrigger: { trigger: section, start: "top top", end: "bottom bottom", scrub: 0.6 },
  });

  tl.to(generic, { opacity: 1, y: 0, duration: 0.5, ease: "power2.out" }, 0.0)
    .to(agent, { opacity: 1, y: 0, duration: 0.5, ease: "power2.out" }, 0.08);

  // Generic claims fail one by one; the card goes dashed + dims.
  generic.querySelectorAll(".claim").forEach((c, i) => {
    tl.set(c, { attr: { "data-state": "bad" } }, 0.9 + i * 0.35);
  });
  tl.to(generic, { opacity: 0.5, duration: 0.5, ease: "none" }, 1.9);

  // Agent claims verify; citations attach.
  agent.querySelectorAll(".claim").forEach((c, i) => {
    tl.set(c, { attr: { "data-state": "ok" } }, 1.2 + i * 0.4);
    const cite = c.querySelector(".cite");
    if (cite) tl.to(cite, { opacity: 1, scale: 1, duration: 0.4, ease: "back.out(1.7)" }, 1.25 + i * 0.4);
  });

  // Hold the resolved comparison to the end of the scene.
  tl.to({}, { duration: 0.8 });

  st = tl.scrollTrigger;
}

export function destroy() {
  st?.kill();
  st = null;
}
