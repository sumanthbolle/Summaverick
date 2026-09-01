/*
 * motion.js — the second motion layer on top of scroll-experience.js.
 *
 * Adds the "motionsites"-genre flourishes: magnetic buttons, cursor-tilt
 * cards, count-up stats, a marquee practice strip, and a pinned (sticky)
 * capabilities rail that advances as you scroll. Everything here is an
 * enhancement and self-guards: it no-ops under prefers-reduced-motion, when
 * GSAP/ScrollTrigger are missing, or (for pointer effects) on touch/coarse
 * pointers. boot.js only calls it once the scroll experience has engaged.
 */

import { $, $$, prefersReducedMotion } from "./dom.js";

export function initMotion() {
  if (prefersReducedMotion() || !window.gsap || !window.ScrollTrigger) return;
  const gsap = window.gsap;
  const ScrollTrigger = window.ScrollTrigger;

  const finePointer = window.matchMedia("(hover: hover) and (pointer: fine)").matches;
  if (finePointer) {
    magneticButtons(gsap);
    cardTilt(gsap);
  }
  countUp(gsap, ScrollTrigger);
  marquee(gsap);
  capabilitiesRail(gsap, ScrollTrigger);
}

/* ---- Magnetic buttons: the CTA leans toward the cursor ---------------- */
function magneticButtons(gsap) {
  $$(".btn-primary").forEach((btn) => {
    const strength = 0.35;
    const label = btn.firstChild ? btn : null;
    const move = (e) => {
      const r = btn.getBoundingClientRect();
      const mx = e.clientX - (r.left + r.width / 2);
      const my = e.clientY - (r.top + r.height / 2);
      gsap.to(btn, { x: mx * strength, y: my * strength, duration: 0.4, ease: "power3.out" });
    };
    const reset = () =>
      gsap.to(btn, { x: 0, y: 0, duration: 0.6, ease: "elastic.out(1, 0.4)" });
    btn.addEventListener("pointermove", move);
    btn.addEventListener("pointerleave", reset);
    void label;
  });
}

/* ---- Card tilt: work / engagement cards follow the cursor in 3D ------- */
function cardTilt(gsap) {
  $$(".work-card, .engage-card").forEach((card) => {
    card.style.transformStyle = "preserve-3d";
    const max = 6; // degrees
    const move = (e) => {
      const r = card.getBoundingClientRect();
      const px = (e.clientX - r.left) / r.width - 0.5;
      const py = (e.clientY - r.top) / r.height - 0.5;
      gsap.to(card, {
        rotateY: px * max,
        rotateX: -py * max,
        y: -4,
        duration: 0.4,
        ease: "power2.out",
        transformPerspective: 700,
        transformOrigin: "center",
      });
    };
    const reset = () =>
      gsap.to(card, { rotateX: 0, rotateY: 0, y: 0, duration: 0.6, ease: "power3.out" });
    card.addEventListener("pointermove", move);
    card.addEventListener("pointerleave", reset);
  });
}

/* ---- Count-up: stat numbers tick to their value in view --------------- */
function countUp(gsap, ScrollTrigger) {
  $$("[data-countup]").forEach((el) => {
    const end = parseFloat(el.dataset.countup) || 0;
    const obj = { v: 0 };
    el.textContent = "0";
    ScrollTrigger.create({
      trigger: el,
      start: "top 90%",
      once: true,
      onEnter: () =>
        gsap.to(obj, {
          v: end,
          duration: 1.2,
          ease: "power2.out",
          onUpdate: () => {
            el.textContent = String(Math.round(obj.v));
          },
        }),
    });
  });
}

/* ---- Marquee: the practice strip scrolls continuously ----------------- */
function marquee(gsap) {
  const row = $(".trust-row");
  if (!row || row.dataset.marquee) return;
  const items = Array.from(row.children);
  if (items.length < 2) return;
  row.dataset.marquee = "1";

  const track = document.createElement("div");
  track.className = "marquee-track";
  items.forEach((it) => track.append(it));
  const clone = track.cloneNode(true);
  clone.setAttribute("aria-hidden", "true");

  const lane = document.createElement("div");
  lane.className = "marquee";
  lane.append(track, clone);
  row.classList.add("is-marquee");
  row.append(lane);

  const tl = gsap.to(lane, { xPercent: -50, repeat: -1, duration: 26, ease: "none" });
  // Ease off while the pointer is over the strip.
  row.addEventListener("pointerenter", () => gsap.to(tl, { timeScale: 0, duration: 0.4 }));
  row.addEventListener("pointerleave", () => gsap.to(tl, { timeScale: 1, duration: 0.4 }));
}

/* ---- Capabilities rail: sticky index advances through the five steps --- */
function capabilitiesRail(gsap, ScrollTrigger) {
  const steps = $$(".cap-steps li");
  const fill = $(".cap-progress span");
  if (!steps.length) return;
  const articles = steps.map((li) => document.getElementById(li.dataset.cap));

  const setActive = (i) => {
    steps.forEach((s, j) => s.classList.toggle("is-active", j === i));
    if (fill) fill.style.transform = `scaleY(${(i + 1) / steps.length})`;
  };
  setActive(0);

  articles.forEach((art, i) => {
    if (!art) return;
    ScrollTrigger.create({
      trigger: art,
      start: "top 55%",
      end: "bottom 55%",
      onToggle: (self) => self.isActive && setActive(i),
    });
  });
}
