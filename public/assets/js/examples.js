/*
 * examples.js — plays the looping "how the agent works" demos in the
 * "Agents we build" cards.
 *
 * Every demo's resting state (scripting off, initialization failure, or
 * reduced motion) is the fully resolved illustration in CSS, so nothing here
 * is needed to read the cards. When motion is allowed, each demo loops only
 * while its card is in view — a class toggle drives CSS animations, and the
 * loop pauses when the card scrolls away so nothing animates off-screen.
 */
import { $$ } from "./lib/dom.js";

export function initExamples() {
  const vizzes = $$("[data-ex-anim]");
  if (!vizzes.length) return;

  const reduce = window.matchMedia("(prefers-reduced-motion: reduce)");
  if (reduce.matches) return;

  if (!("IntersectionObserver" in window)) {
    // No observer: just let them all loop.
    for (const v of vizzes) v.classList.add("is-live");
    return;
  }

  const io = new IntersectionObserver(
    (entries) => {
      for (const e of entries) {
        e.target.classList.toggle("is-live", e.isIntersecting);
      }
    },
    { threshold: 0.35 }
  );
  for (const v of vizzes) io.observe(v);
}
