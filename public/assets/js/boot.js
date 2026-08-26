/*
 * boot.js — orchestrator.
 *
 * Responsibilities:
 *  - smooth scrolling (Lenis) wired to GSAP ScrollTrigger, motion permitting
 *  - mount + animate each scroll scene
 *  - nav state, theme toggle, below-fold content and interactions
 *
 * Every scene renders its content unconditionally so the page is complete and
 * readable; only the scroll timeline is gated on prefers-reduced-motion.
 */

import { $, $$, prefersReducedMotion } from "./lib/dom.js";
import * as hero from "./scenes/hero.js";
import * as gap from "./scenes/gap.js";
import * as layers from "./scenes/layers.js";
import * as firewall from "./scenes/firewall.js";
import * as receipts from "./scenes/receipts.js";
import { initContent } from "./content.js";

const SCENES = { hero, gap, layers, firewall, receipts };

function initSmoothScroll(gsap, ScrollTrigger) {
  const lenis = new window.Lenis({
    duration: 0.85,
    easing: (t) => Math.min(1, 1.001 - Math.pow(2, -10 * t)),
    smoothWheel: true,
    wheelMultiplier: 1.15,
    touchMultiplier: 1.6,
  });
  lenis.on("scroll", ScrollTrigger.update);
  gsap.ticker.add((time) => lenis.raf(time * 1000));
  gsap.ticker.lagSmoothing(0);
  // Let in-page anchors ride Lenis instead of the native jump.
  document.addEventListener("click", (e) => {
    const a = e.target.closest('a[href^="#"]');
    if (!a) return;
    const id = a.getAttribute("href");
    if (id.length < 2) return;
    const target = document.querySelector(id);
    if (!target) return;
    e.preventDefault();
    lenis.scrollTo(target, { offset: -72 });
  });
  return lenis;
}

function initNav() {
  const nav = $("#nav");
  const onScroll = () => nav.setAttribute("data-scrolled", String(window.scrollY > 8));
  onScroll();
  window.addEventListener("scroll", onScroll, { passive: true });

  const toggle = $("#theme-toggle");
  toggle?.addEventListener("click", () => {
    const root = document.documentElement;
    const current =
      root.getAttribute("data-theme") ||
      (window.matchMedia("(prefers-color-scheme: dark)").matches ? "dark" : "light");
    const next = current === "dark" ? "light" : "dark";
    root.setAttribute("data-theme", next);
    try { localStorage.setItem("sv-theme", next); } catch (e) {}
  });
}

function boot() {
  initNav();
  initContent();

  const reduced = prefersReducedMotion();
  const gsap = window.gsap;
  const ScrollTrigger = window.ScrollTrigger;
  const ready = gsap && ScrollTrigger && !reduced;

  if (ready) {
    gsap.registerPlugin(ScrollTrigger);
    initSmoothScroll(gsap, ScrollTrigger);
  }

  const ctx = { gsap, ScrollTrigger, reduced: !ready };

  for (const section of $$("[data-scene]")) {
    const mod = SCENES[section.dataset.scene];
    if (mod && typeof mod.init === "function") {
      try { mod.init(section, ctx); } catch (e) { console.error("scene failed:", section.dataset.scene, e); }
    }
  }

  if (ready) requestAnimationFrame(() => ScrollTrigger.refresh());
}

if (document.readyState === "loading") {
  document.addEventListener("DOMContentLoaded", boot);
} else {
  boot();
}
