/*
 * greeting.js — the "hello" moment on the homepage hero.
 *
 * A single word cross-fades through a set of greetings, each in a different
 * accent colour, the way Apple cycles "hello" across languages at setup. It is
 * a flourish: with scripting off or under reduced motion the word rests on
 * "Hello", and the headline beneath ("Meet Summaverick") carries the meaning
 * for assistive tech — the cycling span is aria-hidden.
 *
 * Scripts beyond Latin (Telugu, Devanagari, CJK, Arabic) render in the
 * viewer's system fonts; where a script is missing the sweep simply moves on.
 */
import { $ } from "./lib/dom.js";

// text + the spectrum variant class suffix ("" = the default aurora).
const GREETINGS = [
  { t: "Hello", c: "" },
  { t: "నమస్తే", c: "warm" },       // Telugu — home ground
  { t: "नमस्ते", c: "emerald" },     // Hindi
  { t: "Hola", c: "cyan" },
  { t: "Bonjour", c: "" },
  { t: "こんにちは", c: "warm" },     // Japanese
  { t: "안녕하세요", c: "emerald" },  // Korean
  { t: "مرحبا", c: "cyan" },         // Arabic
  { t: "Olá", c: "" },
  { t: "Ciao", c: "warm" },
];

const HOLD_MS = 1900; // time each greeting rests
const FADE_MS = 420; // must match the CSS transition

export function initGreeting() {
  const el = $("[data-hello]");
  if (!el) return;

  if (window.matchMedia("(prefers-reduced-motion: reduce)").matches) {
    paint(el, GREETINGS[0]);
    return;
  }

  let i = 0;
  paint(el, GREETINGS[0]);
  setInterval(() => {
    // Fade the current word out, swap, fade the next in.
    el.style.opacity = "0";
    el.style.transform = "translateY(0.35em)";
    el.style.filter = "blur(6px)";
    setTimeout(() => {
      i = (i + 1) % GREETINGS.length;
      paint(el, GREETINGS[i]);
    }, FADE_MS);
  }, HOLD_MS);
}

function paint(el, g) {
  el.textContent = g.t;
  el.className = "hello__word text-spectrum" + (g.c ? " text-spectrum--" + g.c : "");
  el.setAttribute("lang", langOf(g.t));
  // Next frame, release to the resting state so the transition plays.
  requestAnimationFrame(() => {
    el.style.opacity = "1";
    el.style.transform = "none";
    el.style.filter = "none";
  });
}

function langOf(t) {
  if (/[ఀ-౿]/.test(t)) return "te";
  if (/[ऀ-ॿ]/.test(t)) return "hi";
  if (/[぀-ヿ一-鿿]/.test(t)) return "ja";
  if (/[가-힯]/.test(t)) return "ko";
  if (/[؀-ۿ]/.test(t)) return "ar";
  return "";
}
