/*
 * The paired Personal / Enterprise walk-through on the homepage.
 *
 * Nothing here is required to understand the page. With this file absent, or
 * with scripting off, both views render in full and every step reads as plain
 * text — the `[data-js="on"]` attribute set in the page head is what collapses
 * them into a tabbed, stepped presentation.
 *
 * Rules the illustration keeps:
 *   - it never cycles on its own, and never switches view by itself;
 *   - playback stops at the review step and waits for a decision;
 *   - Previous/Next are always available, including mid-playback;
 *   - it pauses when scrolled out of sight;
 *   - reduced motion drops the timer and keeps the manual steps.
 *
 * Timings: 850ms on a step, 150ms between them, so a transition lands inside
 * the 700–1000ms window without ever outrunning someone reading it.
 */

import { $, $$ } from "./lib/dom.js";

const STEP_MS = 850;
const GAP_MS = 150;
const REVIEW_STEP = 3; // the step that hands the decision to a person

export function initAgentStage() {
  const root = $("[data-stage]");
  if (!root) return;

  const tabs = $$("[data-stage-tab]", root);
  const views = $$("[data-stage-view]", root);
  const play = $("[data-stage-play]", root);
  const prev = $("[data-stage-prev]", root);
  const next = $("[data-stage-next]", root);
  const status = $("[data-stage-status]", root);
  if (!tabs.length || !views.length || !play) return;

  const reduced = window.matchMedia("(prefers-reduced-motion: reduce)");
  let current = tabs[0].dataset.stageTab;
  let timer = null;
  let index = -1;
  let state = "idle"; // idle | playing | paused | waiting | finished

  const view = () => views.find((v) => v.dataset.stageView === current);
  const steps = () => $$("[data-stage-step]", view());
  const name = () => (current === "enterprise" ? "Enterprise" : "Personal");
  const stepLabel = (step) => step?.querySelector(".stage__who")?.textContent?.trim() ?? "";

  const say = (text) => {
    if (status) status.textContent = text;
  };

  const clearTimer = () => {
    if (timer !== null) {
      clearTimeout(timer);
      timer = null;
    }
  };

  /* The highlight is emphasis only: an unhighlighted step is still readable. */
  const show = (i, { announce = true } = {}) => {
    const list = steps();
    index = Math.max(-1, Math.min(list.length - 1, i));
    list.forEach((step, n) => step.classList.toggle("is-active", n === index));
    if (announce && list[index]) {
      say(`${name()}, step ${index + 1} of ${list.length}: ${stepLabel(list[index])}`);
    }
  };

  const setPlayLabel = () => {
    play.textContent =
      state === "playing" ? "Pause"
        : state === "paused" ? "Resume"
          : state === "waiting" ? "Skip the decision"
            : state === "finished" ? "Replay" : "Play";
  };

  const finish = () => {
    state = "finished";
    clearTimer();
    steps().forEach((step) => step.classList.remove("is-active"));
    index = -1;
    say(`${name()} example finished. Every step above stays on the page.`);
    setPlayLabel();
  };

  /* Playback halts here so the decision is a person's, not the timer's. */
  const waitForDecision = () => {
    state = "waiting";
    clearTimer();
    setPlayLabel();
    say(
      `${name()}, step ${REVIEW_STEP + 1} of ${steps().length}: waiting for you. ` +
        "Choose one of the two buttons in the illustration, or use Next step."
    );
  };

  const advance = () => {
    const list = steps();
    const target = index + 1;
    if (target >= list.length) {
      timer = setTimeout(finish, STEP_MS);
      return;
    }
    show(target);
    if (target === REVIEW_STEP) {
      timer = setTimeout(waitForDecision, STEP_MS);
      return;
    }
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
    const list = steps();
    say(
      list[index]
        ? `Paused on step ${index + 1} of ${list.length}: ${stepLabel(list[index])}`
        : "Paused."
    );
  };

  const resume = () => {
    state = "playing";
    setPlayLabel();
    timer = setTimeout(advance, GAP_MS);
  };

  const reset = ({ announce = false } = {}) => {
    clearTimer();
    state = "idle";
    views.forEach((v) => $$("[data-stage-step]", v).forEach((s) => s.classList.remove("is-active")));
    index = -1;
    setPlayLabel();
    if (announce) say(`${name()} example ready. Press Play, or step through it.`);
    else say("");
    if (reduced.matches) show(0, { announce: false });
  };

  const select = (id, { focus = false } = {}) => {
    if (!views.some((v) => v.dataset.stageView === id)) return;
    current = id;
    tabs.forEach((tab) => {
      const on = tab.dataset.stageTab === id;
      tab.setAttribute("aria-selected", String(on));
      tab.tabIndex = on ? 0 : -1;
      if (on && focus) tab.focus();
    });
    views.forEach((v) => {
      v.hidden = v.dataset.stageView !== id;
    });
    reset({ announce: true });
  };

  /* ---- controls --------------------------------------------------------- */
  play.addEventListener("click", () => {
    if (state === "playing") pause();
    else if (state === "paused") resume();
    else if (state === "waiting") {
      show(REVIEW_STEP + 1);
      state = "playing";
      setPlayLabel();
      timer = setTimeout(finish, STEP_MS);
    } else start();
  });

  next.addEventListener("click", () => {
    clearTimer();
    state = "idle";
    setPlayLabel();
    show(index + 1);
  });

  prev.addEventListener("click", () => {
    clearTimer();
    state = "idle";
    setPlayLabel();
    show(Math.max(0, index - 1));
  });

  /* The approve/edit buttons inside an illustration advance the example. They
     are worded, and answered, so they cannot be read as a real-world action. */
  for (const button of $$("[data-stage-advance]", root)) {
    button.addEventListener("click", () => {
      clearTimer();
      state = "idle";
      setPlayLabel();
      show(REVIEW_STEP + 1, { announce: false });
      const list = steps();
      say(
        `${button.dataset.stageSaid} Now showing step ${REVIEW_STEP + 2} of ${list.length}: ` +
          stepLabel(list[REVIEW_STEP + 1])
      );
    });
  }

  /* ---- tabs ------------------------------------------------------------- */
  tabs.forEach((tab) => {
    tab.addEventListener("click", () => select(tab.dataset.stageTab));
    tab.addEventListener("keydown", (e) => {
      const i = tabs.indexOf(tab);
      if (e.key === "ArrowRight" || e.key === "ArrowLeft") {
        e.preventDefault();
        const step = e.key === "ArrowRight" ? 1 : -1;
        const target = tabs[(i + step + tabs.length) % tabs.length];
        select(target.dataset.stageTab, { focus: true });
      }
    });
  });

  for (const link of $$('[data-stage-jump]')) {
    link.addEventListener("click", () => select(link.dataset.stageJump));
  }

  /* ---- reduced motion and visibility ------------------------------------ */
  const applyMotionMode = () => {
    play.hidden = reduced.matches;
    if (reduced.matches) {
      clearTimer();
      state = "idle";
      if (index < 0) show(0, { announce: false });
    }
    setPlayLabel();
  };
  reduced.addEventListener?.("change", applyMotionMode);
  applyMotionMode();

  if ("IntersectionObserver" in window) {
    const io = new IntersectionObserver(
      (entries) => {
        for (const entry of entries) {
          if (!entry.isIntersecting && state === "playing") pause();
        }
      },
      { threshold: 0.15 }
    );
    io.observe(root);
  }

  document.addEventListener("visibilitychange", () => {
    if (document.hidden && state === "playing") pause();
  });
}
