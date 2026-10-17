/*
 * hero-headline.js — "Meet Super Intelligence." ⇄ "Meet Summaverick."
 *
 * The hero headline keeps two faces in one line: the spectrum phrase and the
 * glass wordmark. Once the wordmark's first crystal reveal has played, the
 * line alternates between them for as long as the hero is on screen: the
 * spectrum phrase arrives with a sweep, a hue turn and a flash; the wordmark
 * comes back by reassembling itself (the same reveal, replayed).
 *
 * Everything here is an enhancement. The brand face is the stylesheet's
 * default, so with scripting off, under reduced motion, or if this never runs,
 * the page reads "Meet Summaverick." and nothing moves. The cycle also stops
 * while the tab is hidden or the hero has scrolled away, and the moment the
 * visitor turns reduced motion on.
 */
import { $ } from "./lib/dom.js";

const BRAND_HOLD_MS = 4600; // the reveal (3.6s) plus a moment at rest
const SUPER_HOLD_MS = 4200; // the spectrum pass (3.2s) plus a moment at rest
const SWAP_MS = 600; // must match the face transition in the CSS
const SUPER_PLAY_MS = 3400; // longest of the super-face animations

/**
 * @param {{ play(): void, firstPlay: Promise<void>, reduce: MediaQueryList } | null} glassmark
 */
export function initHeroHeadline(glassmark) {
  const mark = $("[data-headline-cycle]");
  const superFace = mark && mark.querySelector("[data-face-super]");
  if (!mark || !superFace || !glassmark) return;

  const { reduce } = glassmark;
  let timer = 0;
  let playTimer = 0;
  let running = false;
  let onScreen = true;
  let face = "brand";

  const showBrand = () => {
    face = "brand";
    mark.removeAttribute("data-face");
    // Reassemble the wordmark as it lands (the face takes SWAP_MS to appear).
    playTimer = window.setTimeout(() => glassmark.play(), SWAP_MS * 0.35);
  };

  const showSuper = () => {
    face = "super";
    mark.setAttribute("data-face", "super");
    superFace.classList.remove("is-playing");
    void superFace.offsetWidth; // restart the one-pass animations
    superFace.classList.add("is-playing");
    playTimer = window.setTimeout(() => superFace.classList.remove("is-playing"), SUPER_PLAY_MS);
  };

  const tick = () => {
    if (!running) return;
    if (face === "brand") {
      showSuper();
      timer = window.setTimeout(tick, SUPER_HOLD_MS);
    } else {
      showBrand();
      timer = window.setTimeout(tick, BRAND_HOLD_MS);
    }
  };

  const stop = () => {
    running = false;
    window.clearTimeout(timer);
    window.clearTimeout(playTimer);
  };

  const rest = () => {
    // Settle on the brand without a reveal, so there is nothing to catch up on.
    stop();
    face = "brand";
    mark.removeAttribute("data-face");
    superFace.classList.remove("is-playing");
  };

  const resume = () => {
    if (running || reduce.matches || document.hidden || !onScreen) return;
    running = true;
    // Whatever face is showing, give it a full hold before the next swap.
    timer = window.setTimeout(tick, face === "brand" ? BRAND_HOLD_MS : SUPER_HOLD_MS);
  };

  const pause = () => {
    if (!running) return;
    stop();
  };

  // Stop at once if the visitor asks for less motion, and resume if they relent.
  reduce.addEventListener("change", () => (reduce.matches ? rest() : resume()));

  // No work while the tab is in the background.
  document.addEventListener("visibilitychange", () => (document.hidden ? pause() : resume()));

  // No work while the hero is off screen; the line rests on the brand so a
  // scroll back up finds the page as it loaded.
  if ("IntersectionObserver" in window) {
    const io = new IntersectionObserver(
      (entries) => {
        for (const e of entries) {
          onScreen = e.isIntersecting;
          if (onScreen) resume();
          else rest();
        }
      },
      { threshold: 0.2 }
    );
    io.observe(mark);
  }

  // Start only after the wordmark's own first reveal has played.
  glassmark.firstPlay.then(resume);
}
