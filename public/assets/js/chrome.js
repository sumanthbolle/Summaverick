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

/* One quiet rise as marked elements enter the viewport, played once. The
 * CSS keeps everything visible when motion is reduced or this never runs. */
function initReveals() {
  if (
    window.matchMedia("(prefers-reduced-motion: reduce)").matches ||
    !("IntersectionObserver" in window)
  ) {
    return;
  }
  const targets = $$("[data-reveal]");
  if (!targets.length) return;
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
