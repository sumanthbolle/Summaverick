/* Homepage enhancements: shared navigation, contact form, brand motion,
 * and the examples player. Core content also reads with scripting disabled. */

import { initChrome } from "./chrome.js";
import { initLeadForm } from "./lead-form.js";
import { initMotion } from "./motion.js";
import { initGreeting } from "./greeting.js";
import { initGlassmark } from "./glassmark.js";
import { initHeroHeadline } from "./hero-headline.js";
import { initExamples } from "./examples.js";
import { initDither } from "./dither.js";
import { initParticleS } from "./particle-s.js";
import { initFx } from "./fx.js";

function boot() {
  initChrome();
  initLeadForm();
  initMotion();
  initGreeting();
  initHeroHeadline(initGlassmark());
  initExamples();
  initDither();
  initParticleS();
  initFx();
}

if (document.readyState === "loading") {
  document.addEventListener("DOMContentLoaded", boot);
} else {
  boot();
}
