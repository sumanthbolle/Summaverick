/*
 * fx.js — two small homepage touches.
 *
 * Night nav: while the floating nav sits over a [data-night] section (the
 * hero, About), it takes the night glass, so it never shows a light pill on
 * the dark ground. Set as data-over="night" on #nav; the CSS does the rest.
 *
 * Spotlight: cards marked [data-spot] get --mx/--my, the pointer position
 * inside the card, which the CSS turns into a soft light on the card's edge.
 * Pointer devices only, and nothing is written when motion is reduced.
 */

import { $, $$ } from "./lib/dom.js";

function initNightNav() {
  const nav = $("#nav");
  const nights = $$("[data-night]");
  if (!nav || !nights.length) return;
  let raf = 0;
  const sync = () => {
    raf = 0;
    const line = nav.getBoundingClientRect().top + nav.offsetHeight / 2;
    const over = nights.some((el) => {
      const r = el.getBoundingClientRect();
      // A section can end its night early: the hero's dawn turns to day
      // halfway down, so the nav switches to the light glass there.
      const end = el.querySelector("[data-night-end]")?.getBoundingClientRect();
      const bottom = end ? end.top + end.height / 2 : r.bottom;
      return r.top <= line && bottom >= line;
    });
    if (over) nav.setAttribute("data-over", "night");
    else nav.removeAttribute("data-over");
  };
  const queue = () => {
    if (!raf) raf = requestAnimationFrame(sync);
  };
  sync();
  window.addEventListener("scroll", queue, { passive: true });
  window.addEventListener("resize", queue);
}

function initSpotlight() {
  if (!window.matchMedia("(hover: hover) and (pointer: fine)").matches) return;
  if (window.matchMedia("(prefers-reduced-motion: reduce)").matches) return;
  for (const card of $$("[data-spot]")) {
    card.addEventListener("pointermove", (e) => {
      const r = card.getBoundingClientRect();
      card.style.setProperty("--mx", `${e.clientX - r.left}px`);
      card.style.setProperty("--my", `${e.clientY - r.top}px`);
    });
  }
}

export function initFx() {
  initNightNav();
  initSpotlight();
}
