/*
 * examples.js — the "Watch an assistant do the work" demo player.
 *
 * Without this file every story reads in full, stacked. Here we turn the
 * stories into tabs and play each one in beats by setting data-at on its
 * panel: 1 the request, 2 the assistant at work, 3 the result, 4 the
 * before/after comparison. CSS does all of the animating.
 *
 * Until someone picks a tab the stories play in turn, like a short film, and
 * only while the player is on screen. Once they choose a tab (or press
 * Replay) the player stops advancing on its own and stays on their choice.
 * With reduced motion nothing plays: every panel shows its finished state.
 */
import { $, $$ } from "./lib/dom.js";

// How long each beat stays on screen before the next (at = 0 … 4), in ms.
const BEATS = [450, 1900, 2700, 2500, 4300];
const TOTAL = BEATS.reduce((sum, ms) => sum + ms, 0);

export function initExamples() {
  const root = $("[data-demo]");
  if (!root) return;
  const tabs = $$("[data-demo-tab]", root);
  const panels = $$("[data-demo-panel]", root);
  if (!tabs.length || tabs.length !== panels.length) return;

  const reduce = window.matchMedia("(prefers-reduced-motion: reduce)");
  let current = 0;
  let timer = 0;
  let autoplay = true;
  let inView = false;

  root.classList.add("is-enhanced");

  const setAt = (panel, at) => {
    panel.dataset.at = String(at);
    for (const el of $$("[data-step]", panel)) {
      el.classList.toggle("is-shown", Number(el.dataset.step) <= at);
    }
    for (const el of $$("[data-say]", panel)) {
      const n = Number(el.dataset.say);
      el.classList.toggle("is-shown", n <= at);
      el.classList.toggle("is-current", n === at);
    }
  };

  // Jump a panel back to its empty first frame without playing it in reverse.
  const rewind = (panel) => {
    panel.classList.add("is-instant");
    setAt(panel, 0);
    void panel.offsetWidth;
    panel.classList.remove("is-instant");
  };

  const advance = (panel, at) => {
    setAt(panel, at);
    timer = window.setTimeout(() => {
      if (at < 4) {
        advance(panel, at + 1);
        return;
      }
      tabs[current].classList.remove("is-running");
      tabs[current].classList.add("is-done");
      if (autoplay && inView) show((current + 1) % tabs.length);
    }, BEATS[at]);
  };

  function show(index, { play = true } = {}) {
    window.clearTimeout(timer);
    current = index;
    tabs.forEach((tab, i) => {
      const on = i === index;
      tab.setAttribute("aria-selected", String(on));
      tab.tabIndex = on ? 0 : -1;
      tab.classList.remove("is-running", "is-done");
    });
    panels.forEach((panel, i) => {
      panel.hidden = i !== index;
    });

    const panel = panels[index];
    const tab = tabs[index];
    if (!play || reduce.matches) {
      setAt(panel, 4);
      tab.classList.add("is-done");
      return;
    }
    rewind(panel);
    tab.style.setProperty("--dur", `${TOTAL}ms`);
    void tab.offsetWidth; // restart the progress bar
    tab.classList.add("is-running");
    advance(panel, 0);
  }

  const choose = (index) => {
    autoplay = false;
    show(index);
  };

  tabs.forEach((tab, i) => {
    tab.addEventListener("click", () => choose(i));
    tab.addEventListener("keydown", (event) => {
      const last = tabs.length - 1;
      const to = {
        ArrowRight: i === last ? 0 : i + 1,
        ArrowLeft: i === 0 ? last : i - 1,
        Home: 0,
        End: last,
      }[event.key];
      if (to === undefined) return;
      event.preventDefault();
      tabs[to].focus();
      choose(to);
    });
  });
  for (const button of $$("[data-demo-replay]", root)) {
    button.addEventListener("click", () => choose(current));
  }

  // Start in the finished state; the first story plays when it comes into view.
  show(0, { play: false });
  if (reduce.matches) return;

  if (!("IntersectionObserver" in window)) {
    inView = true;
    show(current);
    return;
  }
  new IntersectionObserver(
    (entries) => {
      for (const entry of entries) {
        const wasInView = inView;
        inView = entry.isIntersecting;
        if (inView && !wasInView) {
          show(current);
        } else if (!inView && wasInView) {
          // Off screen: stop the clock and leave the story finished.
          show(current, { play: false });
        }
      }
    },
    { threshold: 0.35 }
  ).observe(root);
}
