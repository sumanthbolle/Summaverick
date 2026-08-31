/*
 * boot.js — company homepage. Chrome, the Apple-style scroll experience,
 * below-fold interactions, and the agent preview.
 */

import { initChrome } from "./chrome.js";
import { initContent } from "./content.js";
import { initScrollExperience } from "./lib/scroll-experience.js";

function boot() {
  initChrome();
  // The scroll experience owns reveals when it engages; otherwise fall back to
  // the plain IntersectionObserver reveal in content.js.
  const rich = initScrollExperience();
  initContent({ reveal: !rich });
}

if (document.readyState === "loading") {
  document.addEventListener("DOMContentLoaded", boot);
} else {
  boot();
}
