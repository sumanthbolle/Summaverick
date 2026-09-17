/* boot.js — homepage. Shared chrome and the contact form; the hero entrance
 * is a CSS animation so it never depends on this file loading. */

import { initChrome } from "./chrome.js";
import { initLeadForm } from "./lead-form.js";

function boot() {
  initChrome();
  initLeadForm();
}

if (document.readyState === "loading") {
  document.addEventListener("DOMContentLoaded", boot);
} else {
  boot();
}
