/* boot.js — homepage. Shared chrome, the paired Personal/Enterprise
 * walk-through, and the contact form. Everything here is an enhancement: the
 * page reads completely, and every step of both walk-throughs stays visible,
 * if this file never runs. */

import { initChrome } from "./chrome.js";
import { initLeadForm } from "./lead-form.js";
import { initAgentStage } from "./agent-stage.js";
import { initMotion } from "./motion.js";
import { initGreeting } from "./greeting.js";

function boot() {
  initChrome();
  initAgentStage();
  initLeadForm();
  initMotion();
  initGreeting();
}

if (document.readyState === "loading") {
  document.addEventListener("DOMContentLoaded", boot);
} else {
  boot();
}
