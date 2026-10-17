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
 * viewer's system fonts; the stylesheet names the face each platform ships
 * for the script (keyed on the lang attribute set here) and gives the word
 * room for marks that sit above and below a Latin line. Where a script is
 * missing altogether the sweep simply moves on.
 */
import { $ } from "./lib/dom.js";

// text, language tag, and the spectrum variant class suffix ("" = the default aurora).
const GREETINGS = [
  { t: "Hello", lang: "en", c: "" },
  { t: "నమస్తే", lang: "te", c: "warm" }, // Telugu — home ground
  { t: "नमस्ते", lang: "hi", c: "emerald" }, // Hindi
  { t: "Hola", lang: "es", c: "cyan" },
  { t: "Bonjour", lang: "fr", c: "" },
  { t: "こんにちは", lang: "ja", c: "warm" }, // Japanese
  { t: "안녕하세요", lang: "ko", c: "emerald" }, // Korean
  { t: "مرحبا", lang: "ar", c: "cyan" }, // Arabic
  { t: "Olá", lang: "pt", c: "" },
  { t: "Ciao", lang: "it", c: "warm" },
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
  el.setAttribute("lang", g.lang);
  // Arabic reads right to left; the others left to right.
  el.setAttribute("dir", g.lang === "ar" ? "rtl" : "ltr");
  // Next frame, release to the resting state so the transition plays.
  requestAnimationFrame(() => {
    el.style.opacity = "1";
    el.style.transform = "none";
    el.style.filter = "none";
  });
}
