/*
 * boot.js — company homepage. Core chrome and a progressive hero reveal.
 */

import { initChrome } from "./chrome.js";
import { initContent } from "./content.js";
import { initStudioHero } from "./home.js";

function boot() {
  initChrome();
  initStudioHero();
  initContent({ reveal: false });
}

if (document.readyState === "loading") {
  document.addEventListener("DOMContentLoaded", boot);
} else {
  boot();
}
