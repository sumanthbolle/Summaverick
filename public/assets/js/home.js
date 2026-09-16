export function initStudioHero() {
  if (window.matchMedia("(prefers-reduced-motion: reduce)").matches) return;

  const inner = document.querySelector(".studio-hero__inner");
  if (!inner || !("animate" in inner)) return;

  inner.animate(
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
}
