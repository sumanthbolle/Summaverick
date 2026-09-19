/*
 * motion.js — the cinematic 3D touch on the homepage.
 *
 * Every element marked [data-tilt] tilts toward the pointer and takes a small
 * baseline rotation from its position in the scroll, so it also moves on a
 * touch device with no pointer. Pure transforms on one wrapper — the image
 * itself is untouched.
 *
 * It is an enhancement only: with scripting off, or under a reduced-motion
 * preference, the showcase rests flat and the page is complete. The CSS reads
 * --rx/--ry (pointer) and --sy (scroll baseline); this file never writes them
 * when motion is reduced.
 */
import { $$ } from "./lib/dom.js";

const MAX_TILT = 9; // degrees toward the pointer
const SCROLL_SWING = 7; // degrees across a full pass through the viewport

export function initMotion() {
  if (window.matchMedia("(prefers-reduced-motion: reduce)").matches) return;
  const tilts = $$("[data-tilt]");
  if (!tilts.length) return;
  tilts.forEach(setupTilt);
}

function setupTilt(el) {
  const stage = el.querySelector("[data-tilt-stage]") || el;
  let raf = 0;
  let targetX = 0;
  let targetY = 0;
  let curX = 0;
  let curY = 0;

  const queue = () => {
    if (!raf) raf = requestAnimationFrame(apply);
  };

  const apply = () => {
    raf = 0;
    curX += (targetX - curX) * 0.14;
    curY += (targetY - curY) * 0.14;
    stage.style.setProperty("--rx", curX.toFixed(2) + "deg");
    stage.style.setProperty("--ry", curY.toFixed(2) + "deg");
    if (Math.abs(targetX - curX) > 0.04 || Math.abs(targetY - curY) > 0.04) queue();
  };

  el.addEventListener("pointermove", (e) => {
    const r = el.getBoundingClientRect();
    const px = (e.clientX - r.left) / r.width - 0.5;
    const py = (e.clientY - r.top) / r.height - 0.5;
    targetX = -py * MAX_TILT;
    targetY = px * MAX_TILT;
    queue();
  });

  el.addEventListener("pointerleave", () => {
    targetX = 0;
    targetY = 0;
    queue();
  });

  // Scroll baseline: a gentle swing as the element crosses the viewport, so it
  // has life on touch where there is no pointer. Throttled to one rAF.
  let sraf = 0;
  const onScroll = () => {
    if (sraf) return;
    sraf = requestAnimationFrame(() => {
      sraf = 0;
      const r = el.getBoundingClientRect();
      const vh = window.innerHeight || 1;
      const centre = r.top + r.height / 2;
      const progress = Math.max(-1, Math.min(1, (centre - vh / 2) / (vh / 2)));
      stage.style.setProperty("--sy", (progress * SCROLL_SWING).toFixed(2) + "deg");
    });
  };
  window.addEventListener("scroll", onScroll, { passive: true });
  onScroll();
}
