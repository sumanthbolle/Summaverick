/*
 * Scene 4 — The firewall. A retrieved (untrusted) document carries a hidden
 * instruction. A scan sweeps it, the payload is flagged and stripped, and the
 * clean evidence continues. Driven by the FIREWALL fixture.
 */

import { el } from "../lib/dom.js";
import { FIREWALL } from "../data/fixtures.js";

let st = null;

export function init(section, ctx) {
  const { gsap, reduced } = ctx;
  const wrap = section.querySelector("[data-firewall]");

  const tokenEls = FIREWALL.tokens.map((t) =>
    el("span", { class: t.kind === "payload" ? "payload" : "clean", text: t.t })
  );
  const doc = el("div", { class: "doc" }, [
    el("div", { class: "small text-mute", style: "margin-bottom:var(--space-2)", text: FIREWALL.source + " · " + FIREWALL.caseId }),
    el("div", {}, tokenEls),
  ]);
  const scan = el("span", { class: "scanline", "aria-hidden": "true" });
  const verdict = el("div", { class: "verdict", html: "&#9888; " + FIREWALL.verdict });

  wrap.replaceChildren(doc, scan, verdict);

  const payloads = tokenEls.filter((_, i) => FIREWALL.tokens[i].kind === "payload");

  if (reduced || !gsap) {
    payloads.forEach((p) => (p.style.opacity = "0.35"));
    return;
  }

  gsap.set(doc, { opacity: 0, x: -40 });
  gsap.set(scan, { opacity: 0, y: 0 });
  gsap.set(verdict, { opacity: 0, y: 10, scale: 0.96, transformOrigin: "left center" });

  const tl = gsap.timeline({
    scrollTrigger: { trigger: section, start: "top top", end: "bottom bottom", scrub: 0.6 },
  });

  tl.to(doc, { opacity: 1, x: 0, duration: 0.5, ease: "power2.out" }, 0.0)
    // Sweep the scan down the document.
    .to(scan, { opacity: 1, duration: 0.15 }, 0.6)
    .to(scan, { y: () => doc.offsetHeight - 6, duration: 1.0, ease: "none" }, 0.6)
    // The payload is caught and stripped as the scan crosses it.
    .to(payloads, { opacity: 0.2, duration: 0.4, ease: "power2.in" }, 1.15)
    .to(scan, { opacity: 0, duration: 0.2 }, 1.7)
    // Verdict lands.
    .to(verdict, { opacity: 1, y: 0, scale: 1, duration: 0.4, ease: "back.out(1.6)" }, 1.85)
    .to({}, { duration: 0.6 });

  st = tl.scrollTrigger;
}

export function destroy() {
  st?.kill();
  st = null;
}
