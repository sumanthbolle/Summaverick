/* boot.js — homepage. Shared chrome, the workflow illustration, and the contact
 * form. Everything here is an enhancement: the page reads completely, and the
 * illustration's three steps stay visible, if this file never runs. */

import { initChrome } from "./chrome.js";
import { initLeadForm } from "./lead-form.js";
import { initWorkflowDemo } from "./workflow-demo.js";

function boot() {
  initChrome();
  initWorkflowDemo();
  initLeadForm();
}

if (document.readyState === "loading") {
  document.addEventListener("DOMContentLoaded", boot);
} else {
  boot();
}
