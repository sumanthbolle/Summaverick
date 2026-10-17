/*
 * glassmark.js — the crystal assembly for the hero "Summaverick".
 *
 * The resolved glass wordmark is the default (CSS) state, so with scripting
 * off, initialization failure, or reduced motion the complete word shows
 * immediately and nothing here runs. When motion is allowed, the reveal plays
 * once the font is ready and the hero has entered view; it never replays on
 * scroll-back or a theme switch by itself. hero-headline.js replays it each
 * time the headline returns to the brand, through the controller returned
 * here. A replay control is injected only for local review (?preview).
 */
import { $ } from "./lib/dom.js";

const PLAY_MS = 3600;

/**
 * @returns {{ play(): void, firstPlay: Promise<void>, reduce: MediaQueryList } | null}
 *   play() restarts the assembly; firstPlay resolves after the reveal has
 *   started on its own (never, under reduced motion).
 */
export function initGlassmark() {
  const mark = $("[data-glassmark]");
  if (!mark) return null;

  const reduce = window.matchMedia("(prefers-reduced-motion: reduce)");

  let cleanupTimer = 0;
  const play = () => {
    if (reduce.matches) return;
    window.clearTimeout(cleanupTimer);
    mark.classList.remove("is-playing");
    // Force a reflow so re-adding the class restarts the animations (replay).
    void mark.offsetWidth;
    mark.classList.add("is-playing");
    cleanupTimer = window.setTimeout(() => {
      // Rest state equals the CSS default, so simply drop the class.
      mark.classList.remove("is-playing");
    }, PLAY_MS);
  };

  // Preview-only replay: gated on ?preview, plus an always-safe hook.
  window.__glassmarkReplay = play;
  if (location.search.includes("preview")) {
    const btn = document.createElement("button");
    btn.type = "button";
    btn.className = "glassmark-replay";
    btn.textContent = "Replay reveal";
    btn.addEventListener("click", play);
    mark.appendChild(btn);
  }

  let resolveFirst = () => {};
  const firstPlay = new Promise((resolve) => {
    resolveFirst = resolve;
  });

  if (reduce.matches) return { play, firstPlay, reduce };

  let played = false;
  const start = () => {
    if (played) return;
    played = true;
    play();
    resolveFirst();
  };

  const whenVisible = () => {
    if (!("IntersectionObserver" in window)) {
      start();
      return;
    }
    const io = new IntersectionObserver(
      (entries) => {
        for (const e of entries) {
          if (e.isIntersecting) {
            start();
            io.disconnect();
          }
        }
      },
      { threshold: 0.4 }
    );
    io.observe(mark);
  };

  // Run after the font is ready so glyph metrics are final before the reveal.
  if (document.fonts && document.fonts.ready) {
    document.fonts.ready.then(whenVisible);
  } else {
    whenVisible();
  }

  return { play, firstPlay, reduce };
}
