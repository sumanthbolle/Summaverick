/*
 * Shared chrome for the company surface (home, ask, library). Scrolled nav,
 * appearance toggle, scroll reveals, mobile drawer, and the copyright year.
 * Product tools keep /assets/app.js.
 *
 * Appearance defaults to the operating system and can be overridden with the
 * nav toggle; the choice persists in localStorage. A tiny inline script in
 * each page <head> applies a stored choice before first paint.
 */

import { $, $$ } from "./lib/dom.js";

const THEME_KEY = "sv-theme";

function systemDark() {
  return window.matchMedia("(prefers-color-scheme: dark)").matches;
}

function resolvedTheme() {
  try {
    const stored = localStorage.getItem(THEME_KEY);
    if (stored === "light" || stored === "dark") return stored;
  } catch (e) {}
  return systemDark() ? "dark" : "light";
}

function initTheme() {
  const btn = $("#theme-toggle");
  if (!btn) return;

  const paint = () => {
    const theme = resolvedTheme();
    btn.setAttribute(
      "aria-label",
      theme === "dark" ? "Switch to light appearance" : "Switch to dark appearance"
    );
  };

  btn.addEventListener("click", () => {
    const next = resolvedTheme() === "dark" ? "light" : "dark";
    document.documentElement.setAttribute("data-theme", next);
    try {
      localStorage.setItem(THEME_KEY, next);
    } catch (e) {}
    paint();
  });

  paint();
}

/* One quiet rise as marked elements enter the viewport, played once.
 *
 * The pre-animation state lives behind [data-motion="on"], set here, so the
 * page is complete when this file never runs, when motion is reduced, and when
 * the visitor turns reduced motion on mid-visit — that last case also clears
 * the attribute, which stops anything still in flight. */
function initReveals() {
  const reduced = window.matchMedia("(prefers-reduced-motion: reduce)");
  reduced.addEventListener?.("change", () => {
    if (reduced.matches) document.documentElement.removeAttribute("data-motion");
  });

  if (reduced.matches || !("IntersectionObserver" in window)) return;

  const targets = $$("[data-reveal]");
  if (!targets.length) return;
  document.documentElement.setAttribute("data-motion", "on");
  const show = (t) => {
    t.classList.add("is-in");
    io.unobserve(t);
  };
  const io = new IntersectionObserver(
    (entries) => {
      for (const en of entries) {
        if (en.isIntersecting) show(en.target);
      }
    },
    { rootMargin: "0px 0px -8% 0px" }
  );
  targets.forEach((t) => io.observe(t));
  // Safety net: anything already in the viewport must never stay hidden,
  // even where intersection callbacks are delayed. Below-fold elements keep
  // the scroll effect.
  setTimeout(() => {
    const vh = window.innerHeight || 0;
    targets.forEach((t) => {
      if (t.classList.contains("is-in")) return;
      const r = t.getBoundingClientRect();
      if (r.top < vh && r.bottom > 0) show(t);
    });
  }, 1200);
}

/* The hero illustration settles into place once the page is ready. The copy,
 * the specialties and the buttons never wait on it. */
function initHeroPanel() {
  const panel = $("[data-hero-panel]");
  if (!panel) return;
  if (window.matchMedia("(prefers-reduced-motion: reduce)").matches) {
    panel.classList.add("is-ready");
    return;
  }
  document.documentElement.setAttribute("data-motion", "on");
  requestAnimationFrame(() => panel.classList.add("is-ready"));
}

export function initChrome() {
  const nav = $("#nav");
  const onScroll = () => {
    const y = window.scrollY;
    nav?.setAttribute("data-scrolled", String(y > 8));
  };
  onScroll();
  window.addEventListener("scroll", onScroll, { passive: true });

  initTheme();
  initReveals();
  initHeroPanel();

  const yr = $("[data-year]");
  if (yr) yr.textContent = String(new Date().getFullYear());

  initMobileNav();
}

function initMobileNav() {
  const btn = $("#nav-toggle");
  const panel = $("#nav-panel");
  if (!btn || !panel) return;

  const setOpen = (open) => {
    document.body.toggleAttribute("data-nav-open", open);
    btn.setAttribute("aria-expanded", String(open));
    panel.hidden = !open;
    if (open) panel.querySelector("a")?.focus();
  };

  btn.addEventListener("click", () => {
    setOpen(!document.body.hasAttribute("data-nav-open"));
  });

  panel.addEventListener("click", (e) => {
    if (e.target.closest("a")) setOpen(false);
  });

  document.addEventListener("keydown", (e) => {
    if (e.key === "Escape" && document.body.hasAttribute("data-nav-open")) {
      setOpen(false);
      btn.focus();
    }
  });
}
