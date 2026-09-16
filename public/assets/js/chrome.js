/*
 * Shared chrome for the company surface (home, ask). Theme, scrolled nav,
 * mobile drawer, and the copyright year. Product tools keep /assets/app.js.
 */

import { $ } from "./lib/dom.js";

export function initChrome() {
  const nav = $("#nav");
  const onScroll = () => {
    const y = window.scrollY;
    nav?.setAttribute("data-scrolled", String(y > 8));
  };
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
    try {
      localStorage.setItem("sv-theme", next);
    } catch (e) {}
  });

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
