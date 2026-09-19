/*
 * The workflow illustration on the homepage.
 *
 * It plays only when asked, runs once, and stops. Nothing here hides content:
 * the three steps read the same before the first play, after the last one, with
 * motion reduced, and when this file never loads. With reduced motion the
 * sequence is replaced by Previous/Next controls that switch state immediately.
 *
 * Timings: 900ms on a step, 500ms between steps — under 4 seconds in total.
 */

import { $, $$ } from "./lib/dom.js";

const STEP_MS = 900;
const GAP_MS = 500;

export function initWorkflowDemo() {
  const root = $("[data-flow]");
  if (!root) return;

  const steps = $$("[data-flow-step]", root);
  const play = $("[data-flow-play]", root);
  const prev = $("[data-flow-prev]", root);
  const next = $("[data-flow-next]", root);
  const status = $("[data-flow-status]", root);
  if (!steps.length || !play) return;

  const reduced = window.matchMedia("(prefers-reduced-motion: reduce)");
  let timer = null;
  let index = -1;
  let state = "idle"; // idle | playing | paused | finished

  const label = (step) => step.querySelector("h3")?.textContent?.trim() ?? "";

  const announce = (text) => {
    if (status) status.textContent = text;
  };

  const show = (i, { announceStep = true } = {}) => {
    index = i;
    steps.forEach((step, n) => step.classList.toggle("is-active", n === i));
    if (announceStep && steps[i]) {
      announce(`Step ${i + 1} of ${steps.length}: ${label(steps[i])}`);
    }
  };

  const clearTimer = () => {
    if (timer !== null) {
      clearTimeout(timer);
      timer = null;
    }
  };

  const setPlayLabel = () => {
    play.textContent =
      state === "playing"
        ? "Pause"
        : state === "paused"
          ? "Resume"
          : state === "finished"
            ? "Replay example"
            : "Play example";
  };

  const finish = () => {
    state = "finished";
    clearTimer();
    steps.forEach((step) => step.classList.remove("is-active"));
    index = -1;
    announce("Example finished. All three steps are shown above.");
    setPlayLabel();
  };

  const advance = () => {
    const nextIndex = index + 1;
    if (nextIndex >= steps.length) {
      timer = setTimeout(finish, STEP_MS);
      return;
    }
    show(nextIndex);
    timer = setTimeout(advance, STEP_MS + GAP_MS);
  };

  const start = () => {
    state = "playing";
    setPlayLabel();
    index = -1;
    advance();
  };

  const pause = () => {
    state = "paused";
    clearTimer();
    setPlayLabel();
    announce(
      steps[index]
        ? `Paused on step ${index + 1} of ${steps.length}: ${label(steps[index])}`
        : "Paused."
    );
  };

  const resume = () => {
    state = "playing";
    setPlayLabel();
    timer = setTimeout(advance, GAP_MS);
  };

  /* Reduced motion: the same three states, stepped through by hand. */
  const useManualControls = (manual) => {
    if (prev) prev.hidden = !manual;
    if (next) next.hidden = !manual;
    play.hidden = manual;
    if (manual) {
      clearTimer();
      state = "idle";
      if (index < 0) show(0);
    }
  };

  play.addEventListener("click", () => {
    if (state === "playing") pause();
    else if (state === "paused") resume();
    else start();
  });

  next?.addEventListener("click", () => {
    show(Math.min(steps.length - 1, index + 1));
  });
  prev?.addEventListener("click", () => {
    show(Math.max(0, index - 1));
  });

  reduced.addEventListener?.("change", () => useManualControls(reduced.matches));
  useManualControls(reduced.matches);
}
