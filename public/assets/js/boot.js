/*
 * boot.js — company homepage. Chrome, below-fold interactions, agent preview.
 * The old GSAP scroll-scene narrative is gone; this page is ordinary HTML.
 */

import { initChrome } from "./chrome.js";
import { initContent } from "./content.js";

function boot() {
  initChrome();
  initContent();
}

if (document.readyState === "loading") {
  document.addEventListener("DOMContentLoaded", boot);
} else {
  boot();
}
