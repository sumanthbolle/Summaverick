/* Scene 1 — Hero. Type settles in on load; content and grid part on scroll. */

let st = null;

export function init(section, ctx) {
  const { gsap, reduced } = ctx;
  if (reduced || !gsap) return; // CSS leaves everything in its final, readable state

  const q = (s) => section.querySelector(s);
  const items = ["[data-hero-eyebrow]", "[data-hero-title]", "[data-hero-lead]", "[data-hero-cta]"]
    .map(q)
    .filter(Boolean);

  gsap.set(items, { opacity: 0, y: 26 });
  gsap.to(items, {
    opacity: 1,
    y: 0,
    duration: 0.95,
    ease: "power3.out",
    stagger: 0.11,
    delay: 0.12,
  });

  const inner = q(".hero-inner");
  const grid = q("[data-hero-grid]");
  const cue = q("[data-hero-cue]");

  const tl = gsap.timeline({
    scrollTrigger: {
      trigger: section,
      start: "top top",
      end: "bottom top",
      scrub: true,
    },
  });
  tl.to(inner, { yPercent: -14, opacity: 0, ease: "none" }, 0)
    .to(grid, { yPercent: 16, opacity: 0.12, ease: "none" }, 0)
    .to(cue, { opacity: 0, ease: "none", duration: 0.25 }, 0);

  st = tl.scrollTrigger;
}

export function destroy() {
  st?.kill();
  st = null;
}
