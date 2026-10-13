/*
 * Shared chrome for the company surface (home, ask, library). Scrolled nav,
 * appearance toggle, scroll reveals, mobile drawer, the copyright year, and the
 * "open in your mail app" chooser for mailto: links.
 * Product tools keep /assets/app.js.
 *
 * Appearance defaults to the operating system and can be overridden with the
 * nav toggle; the choice persists in localStorage. A tiny inline script in
 * each page <head> applies a stored choice before first paint.
 */

import { $, $$ } from "./lib/dom.js";
import { initMailChooser } from "./mail-chooser.js";

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
  // A mailto: link does nothing on a computer with no mail app set up; this
  // offers Outlook, Gmail and the rest instead (see mail-chooser.js).
  initMailChooser();

  const yr = $("[data-year]");
  if (yr) yr.textContent = String(new Date().getFullYear());

  initMobileNav();
  initSectionSpy();
  initDockCta();
}

/* Marks the primary-nav link for the section under the reading line, so the
   header doubles as a "you are here". Sections without a link clear it. */
function initSectionSpy() {
  const links = $$('#nav-links a[href^="#"]');
  if (!links.length || !("IntersectionObserver" in window)) return;
  const byId = new Map(links.map((a) => [a.getAttribute("href").slice(1), a]));
  const sections = $$("main > section[id]");
  const mark = (id) => {
    for (const [key, a] of byId) {
      if (key === id) a.setAttribute("aria-current", "location");
      else a.removeAttribute("aria-current");
    }
  };
  const io = new IntersectionObserver(
    (entries) => {
      for (const en of entries) if (en.isIntersecting) mark(en.target.id);
    },
    { rootMargin: "-35% 0px -60% 0px" }
  );
  sections.forEach((s) => io.observe(s));
}

/* The phone call-to-action: shown once the hero has left the screen, and
   tucked away again while the contact form or the footer is on screen. */
function initDockCta() {
  const cta = $("[data-dock-cta]");
  const hero = $("#top");
  const contact = $("#contact");
  const footer = $(".footer");
  if (!cta || !hero || !("IntersectionObserver" in window)) return;
  const seen = new Set();
  const sync = () => {
    const away = seen.has(hero) || seen.has(contact) || seen.has(footer);
    cta.toggleAttribute("data-away", away);
    cta.tabIndex = away ? -1 : 0;
  };
  const io = new IntersectionObserver((entries) => {
    for (const en of entries) {
      if (en.isIntersecting) seen.add(en.target);
      else seen.delete(en.target);
    }
    sync();
  });
  [hero, contact, footer].forEach((el) => el && io.observe(el));
  cta.setAttribute("data-away", "");
  cta.hidden = false;
  cta.addEventListener("click", () => {
    // Land in the form, not just beside it.
    setTimeout(() => $("#lead-name")?.focus({ preventScroll: true }), 400);
  });
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
