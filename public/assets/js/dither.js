/*
 * dither.js — the ordered-dither light behind the homepage hero and the
 * band where the night hero gives way to the page.
 *
 * Each canvas[data-dither] is painted as a dot matrix: the page is cut into
 * 4px cells, each cell lights a 3x3 dot or stays dark, and an 8x8 Bayer
 * matrix decides which, so a smooth field of light reads as a printed
 * halftone. The dots are one colour (--dither on the canvas) at a few alpha
 * steps, so the texture sits on any ground the CSS gives it.
 *
 *   data-dither="night"  a light falling from the top of the hero, drifting
 *                        slowly while the hero is on screen
 *   data-dither="dawn"   a static band of light across the seam below it
 *
 * Decorative only: the canvases are aria-hidden, the page reads without
 * them, and under reduced motion the night light is painted once and stays.
 */

const CELL = 4;
const DOT = 3;
const LEVELS = 5;
const FPS = 14;

const BAYER = [
  0, 32, 8, 40, 2, 34, 10, 42,
  48, 16, 56, 24, 50, 18, 58, 26,
  12, 44, 4, 36, 14, 46, 6, 38,
  60, 28, 52, 20, 62, 30, 54, 22,
  3, 35, 11, 43, 1, 33, 9, 41,
  51, 19, 59, 27, 49, 17, 57, 25,
  15, 47, 7, 39, 13, 45, 5, 37,
  63, 31, 55, 23, 61, 29, 53, 21,
].map((v) => (v + 0.5) / 64);

const smooth = (a, b, x) => {
  const t = Math.min(1, Math.max(0, (x - a) / (b - a)));
  return t * t * (3 - 2 * t);
};

const FIELDS = {
  // Two soft lights: a broad one from the top, a smaller one that drifts
  // across the upper right. Falls to nothing by two thirds of the height.
  night(nx, ny, t, aspect) {
    const sx = 0.42 + Math.sin(t * 0.07) * 0.1;
    const dx = (nx - sx) * aspect;
    const top = Math.exp(-(dx * dx) / 0.5 - (ny * ny) / 0.035);
    const ox = 0.8 + Math.sin(t * 0.11 + 1.3) * 0.06;
    const oy = 0.1 + Math.cos(t * 0.09) * 0.03;
    const ex = (nx - ox) * aspect;
    const ey = ny - oy;
    const orb = Math.exp(-(ex * ex) / 0.08 - (ey * ey) / 0.03) * 0.5;
    return (top * 0.8 + orb) * (1 - smooth(0.15, 0.55, ny));
  },
  // A ridge of light across the lower middle of the band, brightest at the
  // centre of the page and thinning toward both edges.
  dawn(nx, ny) {
    const ridge = Math.exp(-((ny - 0.58) ** 2) / 0.03);
    const across = 0.35 + 0.65 * Math.exp(-((nx - 0.5) ** 2) / 0.12);
    return ridge * across;
  },
};

function parseColor(value) {
  const hex = value.trim().replace("#", "");
  if (hex.length !== 6) return [190, 196, 210];
  return [0, 2, 4].map((i) => parseInt(hex.slice(i, i + 2), 16));
}

function setup(canvas) {
  const field = FIELDS[canvas.dataset.dither];
  if (!field) return;
  const ctx = canvas.getContext("2d");
  if (!ctx) return;

  const reduced = window.matchMedia("(prefers-reduced-motion: reduce)");
  const animated = canvas.dataset.dither === "night";
  let cols = 0;
  let rows = 0;
  let image = null;
  let rgb = [190, 196, 210];
  let maxAlpha = 0.5;
  let visible = false;
  let raf = 0;
  let last = 0;
  const start = performance.now();

  const measure = () => {
    const box = canvas.getBoundingClientRect();
    cols = Math.max(1, Math.ceil(box.width / CELL));
    rows = Math.max(1, Math.ceil(box.height / CELL));
    canvas.width = cols * CELL;
    canvas.height = rows * CELL;
    image = ctx.createImageData(canvas.width, canvas.height);
    const style = getComputedStyle(canvas);
    rgb = parseColor(style.getPropertyValue("--dither") || "");
    maxAlpha = parseFloat(style.getPropertyValue("--dither-alpha")) || 0.5;
  };

  const paint = (t) => {
    if (!image) return;
    const data = image.data;
    const w = canvas.width;
    const aspect = cols / rows;
    data.fill(0);
    for (let y = 0; y < rows; y++) {
      const ny = (y + 0.5) / rows;
      for (let x = 0; x < cols; x++) {
        const v = field((x + 0.5) / cols, ny, t, aspect) * (LEVELS - 1);
        if (v <= 0.01) continue;
        const base = Math.floor(v);
        const level = base + (v - base > BAYER[(y & 7) * 8 + (x & 7)] ? 1 : 0);
        if (!level) continue;
        const a = Math.round((level / (LEVELS - 1)) * maxAlpha * 255);
        for (let py = 0; py < DOT; py++) {
          let i = ((y * CELL + py) * w + x * CELL) * 4;
          for (let px = 0; px < DOT; px++, i += 4) {
            data[i] = rgb[0];
            data[i + 1] = rgb[1];
            data[i + 2] = rgb[2];
            data[i + 3] = a;
          }
        }
      }
    }
    ctx.putImageData(image, 0, 0);
  };

  const tick = (now) => {
    raf = 0;
    if (!visible || document.hidden || reduced.matches) return;
    if (now - last >= 1000 / FPS) {
      last = now;
      paint((now - start) / 1000);
    }
    raf = requestAnimationFrame(tick);
  };

  const run = () => {
    if (animated && !reduced.matches && visible && !raf) raf = requestAnimationFrame(tick);
  };

  const redraw = () => {
    measure();
    paint(animated ? (performance.now() - start) / 1000 : 0);
  };

  redraw();
  canvas.classList.add("is-painted");

  let resizeRaf = 0;
  new ResizeObserver(() => {
    cancelAnimationFrame(resizeRaf);
    resizeRaf = requestAnimationFrame(redraw);
  }).observe(canvas);

  // The dot colour follows the appearance on the dawn band.
  new MutationObserver(redraw).observe(document.documentElement, {
    attributes: true,
    attributeFilter: ["data-theme"],
  });

  if (!animated) return;
  new IntersectionObserver((entries) => {
    visible = entries.some((e) => e.isIntersecting);
    run();
  }).observe(canvas);
  document.addEventListener("visibilitychange", run);
  reduced.addEventListener?.("change", run);
}

export function initDither() {
  if (!("ResizeObserver" in window) || !("IntersectionObserver" in window)) return;
  document.querySelectorAll("canvas[data-dither]").forEach(setup);
}
