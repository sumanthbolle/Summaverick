/*
 * Scene 3 — Three layers. A query descends L1 → L2 → L3. Each layer it reaches
 * lights up; layer 3 stays dimmed and locked (instance query is off by default).
 * Driven by the LAYERS fixture. Scrubbed and symmetric.
 */

import { el } from "../lib/dom.js";
import { LAYERS } from "../data/fixtures.js";

let st = null;

function buildLayer(l) {
  return el("div", { class: "layer", dataset: { layer: String(l.n), locked: String(l.locked) } }, [
    el("span", { class: "layer-glow", "aria-hidden": "true" }),
    el("div", { class: "lnum", text: String(l.n) }),
    el("div", {}, [
      el("div", { class: "lname", text: l.name }),
      el("div", { class: "ldesc", text: l.desc }),
    ]),
    el("div", { class: "lstate", text: l.state + (l.locked ? " ·🔒" : "") }),
  ]);
}

export function init(section, ctx) {
  const { gsap, reduced } = ctx;
  const wrap = section.querySelector("[data-layers]");

  const chip = el("div", { class: "query-chip", html: "&#9662; " + LAYERS.query });
  const chipHolder = el("div", { style: "min-height:40px;margin-bottom:var(--space-4);" }, [chip]);
  const rows = LAYERS.layers.map(buildLayer);
  const intent = el("p", { class: "small text-mute", style: "margin-top:var(--space-4);font-family:var(--font-mono);" },
    ["classifier intent → ", el("span", { class: "text-dim", text: LAYERS.intent })]);

  wrap.replaceChildren(chipHolder, ...rows, intent);

  if (reduced || !gsap) {
    rows.forEach((r) => { if (r.dataset.locked !== "true") r.querySelector(".layer-glow").style.opacity = "0.4"; });
    return;
  }

  gsap.set(chip, { opacity: 0, y: -16 });
  gsap.set(rows, { opacity: 0.55, y: 18 });
  gsap.set(intent, { opacity: 0 });

  const tl = gsap.timeline({
    scrollTrigger: { trigger: section, start: "top top", end: "bottom bottom", scrub: 0.6 },
  });

  tl.to(chip, { opacity: 1, y: 0, duration: 0.4, ease: "power2.out" }, 0.0)
    .to(rows, { opacity: 1, y: 0, duration: 0.5, ease: "power2.out", stagger: 0.06 }, 0.1);

  // Query touches each open layer in turn: lift it, hold, settle.
  rows.forEach((r, i) => {
    const locked = r.dataset.locked === "true";
    const glow = r.querySelector(".layer-glow");
    const at = 0.9 + i * 0.8;
    if (locked) {
      // Layer 3 does not activate — a small denied nudge, stays dimmed.
      tl.to(r, { x: 6, duration: 0.12, ease: "power1.inOut", yoyo: true, repeat: 1 }, at)
        .to(r, { opacity: 0.5, duration: 0.2 }, at);
    } else {
      tl.to(chip, { y: 8 + i * 6, duration: 0.3, ease: "power2.inOut" }, at)
        .to(r, { scale: 1.02, duration: 0.3, ease: "power2.out" }, at)
        .to(glow, { opacity: 1, duration: 0.3, ease: "power2.out" }, at)
        .to(r, { scale: 1, duration: 0.3, ease: "power2.inOut" }, at + 0.4)
        .to(glow, { opacity: 0.25, duration: 0.3, ease: "power2.inOut" }, at + 0.4);
    }
  });

  tl.to(intent, { opacity: 1, duration: 0.4 }, "-=0.3");
  tl.to({}, { duration: 0.6 });

  st = tl.scrollTrigger;
}

export function destroy() {
  st?.kill();
  st = null;
}
