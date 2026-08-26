/*
 * Scene 5 — The receipts. Claims materialise with their sources attached; the
 * one claim no source could verify fails in view. Alongside, the eval
 * scoreboard — including the misses. Driven by RECEIPTS and SCORES fixtures.
 */

import { el } from "../lib/dom.js";
import { RECEIPTS, SCORES } from "../data/fixtures.js";

let st = null;

function buildClaim(c) {
  return el("div", { class: "claim", dataset: { targetState: c.state } }, [
    el("span", { class: "status", "aria-hidden": "true", text: c.state === "ok" ? "✓" : "✕" }),
    el("div", { class: "txt" }, [
      el("span", { text: c.txt }),
      el("span", { class: "cite", html: "&#8250; " + c.cite }),
    ]),
  ]);
}

function buildScoreboard() {
  const rows = SCORES.suites.map((s) =>
    el("div", { class: "score-row" }, [
      el("div", {}, [
        el("div", { class: "small text-mute", text: s.name }),
        s.fails.length
          ? el("div", { class: "fail-list" }, ["failing: ", ...s.fails.flatMap((f, i) => [
              i ? ", " : "", el("span", { class: "fail-id", text: f }),
            ])])
          : el("div", { class: "small text-ok", text: "all cases pass" }),
      ]),
      el("div", { class: "score-num", "data-num": String(s.passed) }, [
        el("span", { text: String(s.passed) }),
        el("span", { class: "den", text: " / " + s.total }),
      ]),
    ])
  );
  return el("div", {}, [
    el("div", { class: "small text-mute", style: "margin-bottom:var(--space-3)", text: "eval suite · latest CI run" }),
    ...rows,
    el("p", { class: "small text-mute", style: "margin-top:var(--space-4)", text: "Wired to live eval_results in T9. Misses are named on purpose." }),
  ]);
}

export function init(section, ctx) {
  const { gsap, reduced } = ctx;
  const claimsCol = section.querySelector("[data-receipts-claims]");
  const board = section.querySelector("[data-scoreboard]");

  const claims = RECEIPTS.claims.map(buildClaim);
  claimsCol.replaceChildren(el("div", { class: "card", style: "padding:var(--space-4)" }, claims));
  board.replaceChildren(buildScoreboard());

  if (reduced || !gsap) {
    claims.forEach((c) => (c.dataset.state = c.dataset.targetState));
    return;
  }

  gsap.set(claims, { opacity: 0, y: 20 });
  gsap.set(board, { opacity: 0, y: 24 });
  claims.forEach((c) => gsap.set(c.querySelector(".cite"), { opacity: 0, scale: 0.9, transformOrigin: "left center" }));

  const tl = gsap.timeline({
    scrollTrigger: { trigger: section, start: "top top", end: "bottom bottom", scrub: 0.6 },
  });

  claims.forEach((c, i) => {
    const at = 0.2 + i * 0.5;
    tl.to(c, { opacity: 1, y: 0, duration: 0.4, ease: "power2.out" }, at)
      .set(c, { attr: { "data-state": c.dataset.targetState } }, at + 0.25)
      .to(c.querySelector(".cite"), { opacity: 1, scale: 1, duration: 0.35, ease: "back.out(1.6)" }, at + 0.28);
  });

  tl.to(board, { opacity: 1, y: 0, duration: 0.5, ease: "power2.out" }, 0.6);
  tl.to({}, { duration: 0.6 });

  st = tl.scrollTrigger;
}

export function destroy() {
  st?.kill();
  st = null;
}
