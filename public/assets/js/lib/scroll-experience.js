/*
 * scroll-experience.js — the Apple-style scroll layer for the homepage.
 *
 * Lenis drives momentum (smooth) scrolling; GSAP + ScrollTrigger drive the
 * scroll-linked "tide" parallax, the cinematic section reveals, the top
 * progress bar, and the right-edge section pager. Everything here is an
 * enhancement: the page is complete and readable without it, and the whole
 * module no-ops under prefers-reduced-motion or if a vendor lib is missing.
 *
 * initScrollExperience() returns true when it took over motion, so boot.js can
 * skip the plain IntersectionObserver reveal fallback in content.js.
 */

import { $, $$, prefersReducedMotion } from "./dom.js";

const ROOT = document.documentElement;

/* CSS hides these while .sv-anim is set; we animate them back in. Keep this in
 * sync with the reveal selectors in site.css. */
const REVEAL_SELECTOR =
  ".section-head, .capability, .work-card, .engage-card, .agent-shell, .contact-form";

export function initScrollExperience() {
  // No motion wanted, or the libs never loaded — hand back to the plain page.
  if (prefersReducedMotion() || !window.gsap || !window.ScrollTrigger || !window.Lenis) {
    ROOT.classList.remove("sv-anim");
    return false;
  }

  const gsap = window.gsap;
  const ScrollTrigger = window.ScrollTrigger;
  gsap.registerPlugin(ScrollTrigger);

  const lenis = startSmoothScroll(gsap, ScrollTrigger);

  buildProgressBar(lenis);
  playMastheadIntro(gsap);
  tideParallax(gsap, ScrollTrigger);
  revealOnScroll(gsap, ScrollTrigger);
  buildSectionPager(lenis, ScrollTrigger);

  // Everything is wired and the reveal targets are under GSAP's control now.
  ROOT.classList.add("sv-ready");
  // Late layout (fonts, images) can shift trigger positions — recompute once.
  window.addEventListener("load", () => ScrollTrigger.refresh());
  return true;
}

/* ---- Lenis momentum scroll, driven off GSAP's ticker ------------------- */
function startSmoothScroll(gsap, ScrollTrigger) {
  const lenis = new window.Lenis({
    lerp: 0.1,
    wheelMultiplier: 1,
    smoothWheel: true,
    // Trackpads/touch keep native feel; only the wheel gets momentum.
    syncTouch: false,
  });

  lenis.on("scroll", ScrollTrigger.update);
  gsap.ticker.add((time) => lenis.raf(time * 1000));
  gsap.ticker.lagSmoothing(0);

  // Route in-page anchor links through Lenis so jumps are eased, not snapped.
  const navOffset = -76;
  document.addEventListener("click", (e) => {
    const link = e.target.closest('a[href^="#"]');
    if (!link) return;
    const id = link.getAttribute("href");
    if (!id || id === "#") return;
    const target = document.querySelector(id);
    if (!target) return;
    e.preventDefault();
    lenis.scrollTo(target, { offset: navOffset, duration: 1.1 });
    // Close the mobile drawer if it was open.
    if (document.body.hasAttribute("data-nav-open")) {
      $("#nav-toggle")?.click();
    }
  });

  return lenis;
}

/* ---- Top scroll-progress bar ------------------------------------------ */
function buildProgressBar(lenis) {
  const bar = document.createElement("div");
  bar.className = "scroll-progress";
  bar.setAttribute("aria-hidden", "true");
  const fill = document.createElement("span");
  bar.append(fill);
  document.body.append(bar);
  lenis.on("scroll", ({ progress }) => {
    fill.style.transform = `scaleX(${progress || 0})`;
  });
}

/* ---- Masthead intro: headline lines rise on load ---------------------- */
function playMastheadIntro(gsap) {
  const copy = $(".mast-copy");
  const panel = $(".mast-panel");
  if (!copy) return;

  const bits = [
    copy.querySelector(".kicker"),
    copy.querySelector(".display"),
    copy.querySelector(".lead"),
    copy.querySelector(".mast-cta"),
  ].filter(Boolean);

  gsap.set([...bits, panel].filter(Boolean), { opacity: 0, y: 24 });
  const tl = gsap.timeline({ defaults: { ease: "power3.out", duration: 0.9 } });
  tl.to(bits, { opacity: 1, y: 0, stagger: 0.09 }, 0.1);
  if (panel) tl.to(panel, { opacity: 1, y: 0 }, 0.35);
}

/* ---- The "tide": scroll-linked parallax on the masthead atmosphere ----- */
function tideParallax(gsap, ScrollTrigger) {
  const mast = $(".masthead");
  if (!mast) return;

  // Aurora blobs drift up at different rates as the masthead scrolls away —
  // the layered, unhurried motion reads like a slow tide.
  gsap.utils.toArray(".mast-aurora__blob").forEach((blob, i) => {
    gsap.to(blob, {
      yPercent: -18 - i * 14,
      xPercent: i % 2 ? 10 : -8,
      ease: "none",
      scrollTrigger: {
        trigger: mast,
        start: "top top",
        end: "bottom top",
        scrub: true,
      },
    });
  });

  // The grid parallaxes slower than the copy, and the whole masthead fades
  // and settles as it leaves — the Apple "the section hands off" feel.
  const grid = $(".mast-aurora__grid");
  if (grid) {
    gsap.to(grid, {
      yPercent: 24,
      ease: "none",
      scrollTrigger: { trigger: mast, start: "top top", end: "bottom top", scrub: true },
    });
  }

  gsap.to(".mast-copy", {
    yPercent: 12,
    opacity: 0.35,
    ease: "none",
    scrollTrigger: { trigger: mast, start: "top top", end: "bottom top", scrub: true },
  });
}

/* ---- Cinematic reveals as sections enter ------------------------------ */
function revealOnScroll(gsap, ScrollTrigger) {
  $$(REVEAL_SELECTOR).forEach((node) => {
    // CSS pre-hides the container (opacity 0). We fade the container in and,
    // for grouped blocks, rise the children in a stagger — so the visible
    // element and the hidden element are always the same node (no flashes).
    const kids = childrenToStagger(node);
    const riseTargets = kids.length ? kids : [node];

    gsap.set(riseTargets, { y: 26 });
    const tl = gsap.timeline({
      scrollTrigger: { trigger: node, start: "top 82%", once: true },
    });
    tl.to(node, { opacity: 1, duration: 0.6, ease: "power2.out" }, 0);
    tl.to(
      riseTargets,
      {
        y: 0,
        duration: 0.85,
        ease: "power3.out",
        stagger: kids.length ? 0.08 : 0,
        clearProps: "transform",
      },
      0
    );
  });

  // The dark trust strip pills fan in individually.
  const pills = $$(".trust-row span");
  if (pills.length) {
    gsap.from(pills, {
      opacity: 0,
      y: 14,
      duration: 0.6,
      ease: "power2.out",
      stagger: 0.05,
      scrollTrigger: { trigger: ".trust-strip", start: "top 88%", once: true },
    });
  }
}

/* Which inner elements should stagger for a given reveal container. */
function childrenToStagger(node) {
  if (node.classList.contains("section-head")) return $$(":scope > *", node);
  if (node.classList.contains("capability")) {
    return [node.querySelector(".kicker"), node.querySelector("h3"), node.querySelector(".capability-body")].filter(Boolean);
  }
  if (node.classList.contains("contact-form")) return $$(":scope > *", node);
  return [];
}

/* ---- Right-edge section pager (scrollspy + click-to-jump) -------------- */
function buildSectionPager(lenis, ScrollTrigger) {
  const sections = [
    ["#top", "Top"],
    ["#capabilities", "Capabilities"],
    ["#work", "Work"],
    ["#platform", "Platform"],
    ["#contact", "Contact"],
  ].filter(([sel]) => document.querySelector(sel));
  if (sections.length < 3) return;

  const nav = document.createElement("nav");
  nav.className = "section-pager";
  nav.setAttribute("aria-label", "Page sections");

  const dots = sections.map(([sel, label]) => {
    const a = document.createElement("a");
    a.href = sel;
    a.className = "section-pager__dot";
    a.innerHTML = `<span class="section-pager__label">${label}</span>`;
    a.setAttribute("aria-label", label);
    nav.append(a);
    return a;
  });
  document.body.append(nav);

  const setActive = (i) => dots.forEach((d, j) => d.classList.toggle("is-active", j === i));
  setActive(0);

  sections.forEach(([sel], i) => {
    ScrollTrigger.create({
      trigger: document.querySelector(sel),
      start: "top 45%",
      end: "bottom 45%",
      onToggle: (self) => self.isActive && setActive(i),
    });
  });
}
