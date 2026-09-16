export function initStudioHero() {
  if (window.matchMedia("(prefers-reduced-motion: reduce)").matches) return;

  const copy = document.querySelector(".studio-hero__copy");
  const portrait = document.querySelector(".studio-hero__portrait");
  if (!copy || !portrait || !("animate" in copy) || !("animate" in portrait)) return;

  copy.animate(
    [
      { opacity: 0, transform: "translateY(18px)" },
      { opacity: 1, transform: "none" },
    ],
    {
      duration: 760,
      easing: "cubic-bezier(.16, 1, .3, 1)",
      fill: "both",
    }
  );

  portrait.animate(
    [
      { opacity: 0, clipPath: "inset(0 34% 0 0)" },
      { opacity: 1, clipPath: "inset(0 0 0 0)" },
    ],
    {
      duration: 960,
      delay: 110,
      easing: "cubic-bezier(.16, 1, .3, 1)",
      fill: "both",
    }
  );
}
