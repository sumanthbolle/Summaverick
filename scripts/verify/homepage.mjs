/**
 * Checks the homepage behaviours that only exist in a real browser: what is
 * actually visible, what reduced motion changes, and whether the first screen
 * explains the offer with scripting disabled.
 *
 *   node scripts/verify/homepage.mjs [origin]
 *
 * Prints one line per check and exits non-zero on the first failure.
 */
import {
  evaluate,
  goto,
  launch,
  pressEnter,
  pressTab,
  setReducedMotion,
  sleep,
} from "./cdp.mjs";

const ORIGIN = process.argv[2] ?? "http://localhost:8788";

const results = [];
const check = (name, pass, detail = "") => {
  results.push({ name, pass, detail });
  console.log(`${pass ? "PASS" : "FAIL"}  ${name}${detail ? `\n        ${detail}` : ""}`);
};

const VISIBLE = `
  const vis = (sel) => {
    const el = document.querySelector(sel);
    if (!el) return null;
    const r = el.getBoundingClientRect();
    const s = getComputedStyle(el);
    return r.width > 0 && r.height > 0 && s.display !== "none" && s.visibility !== "hidden";
  };
`;

const { proc, client } = await launch({ headless: true, width: 1440, height: 1000 });

try {
  // ---- Motion enabled ----------------------------------------------------
  await setReducedMotion(client, false);
  await goto(client, `${ORIGIN}/`);
  await sleep(900);

  const controls = await evaluate(
    client,
    `${VISIBLE} return {
      play: vis("[data-flow-play]"),
      prev: vis("[data-flow-prev]"),
      next: vis("[data-flow-next]"),
      retry: vis("[data-lead-retry]"),
      submit: vis("[data-lead-submit]"),
    };`
  );
  check(
    "motion on: only Play example is offered, and retry is not",
    controls.play === true &&
      controls.prev === false &&
      controls.next === false &&
      controls.retry === false &&
      controls.submit === true,
    JSON.stringify(controls)
  );

  const motionFlag = await evaluate(
    client,
    `return document.documentElement.getAttribute("data-motion");`
  );
  check("motion on: the enhancement flag is set once scripts run", motionFlag === "on", `data-motion=${motionFlag}`);

  // The hero panel must end up fully opaque and unmoved, not stuck mid-fade.
  const heroPanel = await evaluate(
    client,
    `const el = document.querySelector("[data-hero-panel]");
     const s = getComputedStyle(el);
     return { opacity: Number(s.opacity), transform: s.transform };`
  );
  check(
    "motion on: the hero panel settles fully visible",
    heroPanel.opacity > 0.99 && (heroPanel.transform === "none" || /matrix\(1, 0, 0, 1, 0, 0\)/.test(heroPanel.transform)),
    JSON.stringify(heroPanel)
  );

  // ---- The workflow example runs once and stops --------------------------
  const play = await evaluate(
    client,
    `document.querySelector("[data-flow-play]").click();
     return document.querySelector("[data-flow-play]").textContent.trim();`
  );
  check("workflow: the control becomes Pause while playing", play === "Pause", `label=${play}`);

  await sleep(500);
  const midRun = await evaluate(
    client,
    `return {
       active: [...document.querySelectorAll("[data-flow-step]")].findIndex(s => s.classList.contains("is-active")),
       status: document.querySelector("[data-flow-status]").textContent.trim(),
       stepsReadable: [...document.querySelectorAll("[data-flow-step] h3")].every(h => {
         const r = h.getBoundingClientRect();
         return r.height > 0 && Number(getComputedStyle(h).opacity) > 0.99;
       }),
     };`
  );
  check(
    "workflow: a step is highlighted and all three stay readable while playing",
    midRun.active === 0 && midRun.stepsReadable === true,
    `active=${midRun.active} status="${midRun.status}"`
  );

  await sleep(5200);
  const afterRun = await evaluate(
    client,
    `return {
       label: document.querySelector("[data-flow-play]").textContent.trim(),
       status: document.querySelector("[data-flow-status]").textContent.trim(),
       active: [...document.querySelectorAll("[data-flow-step]")].filter(s => s.classList.contains("is-active")).length,
     };`
  );
  check(
    "workflow: it finishes on its own and offers a replay",
    afterRun.label === "Replay example" && afterRun.active === 0,
    `label="${afterRun.label}" status="${afterRun.status}"`
  );

  // Nothing should still be scheduled: read the label twice, well apart.
  await sleep(2500);
  const stillDone = await evaluate(
    client,
    `return document.querySelector("[data-flow-play]").textContent.trim();`
  );
  check("workflow: it does not loop", stillDone === "Replay example", `label="${stillDone}"`);

  // ---- Pause ------------------------------------------------------------
  await evaluate(client, `document.querySelector("[data-flow-play]").click(); return 1;`);
  await sleep(400);
  const paused = await evaluate(
    client,
    `const b = document.querySelector("[data-flow-play]");
     b.click();
     const first = [...document.querySelectorAll("[data-flow-step]")].findIndex(s => s.classList.contains("is-active"));
     return { label: b.textContent.trim(), first, status: document.querySelector("[data-flow-status]").textContent.trim() };`
  );
  await sleep(2000);
  const heldStill = await evaluate(
    client,
    `return [...document.querySelectorAll("[data-flow-step]")].findIndex(s => s.classList.contains("is-active"));`
  );
  check(
    "workflow: Pause stops it where it is and offers Resume",
    paused.label === "Resume" && heldStill === paused.first,
    `label="${paused.label}" step held at ${heldStill} (status: "${paused.status}")`
  );

  // ---- Keyboard ---------------------------------------------------------
  const focusable = await evaluate(
    client,
    `const sel = 'a[href], button:not([disabled]), input, select, textarea, details > summary, [tabindex]:not([tabindex="-1"])';
     const nodes = [...document.querySelectorAll(sel)].filter(el => {
       if (el.hidden || el.closest("[hidden]")) return false;
       if (el.getAttribute("tabindex") === "-1") return false;
       const r = el.getBoundingClientRect();
       return r.width > 0 && r.height > 0;
     });
     const flow = document.querySelector("[data-flow-play]");
     const summary = document.querySelector(".disclosure > summary");
     return {
       flowReachable: nodes.includes(flow),
       summaryReachable: nodes.includes(summary),
       honeypotSkipped: !nodes.includes(document.querySelector('[name="company_url"]')),
       total: nodes.length,
     };`
  );
  check(
    "keyboard: the replay control and the disclosure are in the tab order, the honeypot is not",
    focusable.flowReachable && focusable.summaryReachable && focusable.honeypotSkipped,
    JSON.stringify(focusable)
  );

  // Tab there for real, so :focus-visible applies the way it would for a
  // keyboard user. A programmatic .focus() would not. Start from a fresh load
  // so the control reads "Play example" rather than a leftover state.
  await goto(client, `${ORIGIN}/`);
  await sleep(600);
  await evaluate(client, `window.scrollTo(0, 0); return 1;`);
  let tabs = 0;
  let reachedByTab = null;
  while (tabs < 60) {
    await pressTab(client);
    tabs += 1;
    reachedByTab = await evaluate(
      client,
      `const a = document.activeElement;
       if (!a || a === document.body) return null;
       const s = getComputedStyle(a);
       return {
         isFlow: a.hasAttribute("data-flow-play"),
         label: (a.textContent || a.getAttribute("aria-label") || a.name || "").trim().slice(0, 40),
         focusVisible: a.matches(":focus-visible"),
         shadow: s.boxShadow,
       };`
    );
    if (reachedByTab?.isFlow) break;
  }
  check(
    "keyboard: tabbing to the play control gives it a visible focus ring",
    reachedByTab?.isFlow === true &&
      reachedByTab.focusVisible === true &&
      reachedByTab.shadow !== "none",
    `reached after ${tabs} tab presses — ${JSON.stringify(reachedByTab)}`
  );

  const activatedByKeyboard = await (async () => {
    await pressEnter(client);
    await sleep(250);
    return evaluate(client, `return document.querySelector("[data-flow-play]").textContent.trim();`);
  })();
  check(
    "keyboard: Enter starts the example without a mouse",
    activatedByKeyboard === "Pause",
    `label after Enter: "${activatedByKeyboard}"`
  );
  // Leave it stopped before the next checks.
  await evaluate(client, `document.querySelector("[data-flow-play]").click(); return 1;`);

  // The disclosure must open without moving the page under the reader.
  const disclosure = await evaluate(
    client,
    `const d = document.querySelector(".disclosure");
     d.scrollIntoView({ block: "center", behavior: "instant" });
     const before = window.scrollY;
     d.querySelector("summary").click();
     return { open: d.open, scrollBefore: before, scrollAfter: window.scrollY,
              label: d.querySelector("summary").textContent.trim() };`
  );
  check(
    "disclosure: it opens in place without scrolling the page",
    disclosure.open === true && disclosure.scrollBefore === disclosure.scrollAfter,
    JSON.stringify(disclosure)
  );

  // ---- Reduced motion ---------------------------------------------------
  await setReducedMotion(client, true);
  await goto(client, `${ORIGIN}/`);
  await sleep(700);

  const reducedState = await evaluate(
    client,
    `${VISIBLE}
     const revealed = [...document.querySelectorAll("[data-reveal]")].every(el =>
       Number(getComputedStyle(el).opacity) > 0.99);
     return {
       motionFlag: document.documentElement.getAttribute("data-motion"),
       revealed,
       play: vis("[data-flow-play]"),
       prev: vis("[data-flow-prev]"),
       next: vis("[data-flow-next]"),
       heroOpacity: Number(getComputedStyle(document.querySelector("[data-hero-panel]")).opacity),
       activeStep: [...document.querySelectorAll("[data-flow-step]")].findIndex(s => s.classList.contains("is-active")),
     };`
  );
  check(
    "reduced motion: no entrance animation is armed and everything is already visible",
    reducedState.motionFlag === null &&
      reducedState.revealed === true &&
      reducedState.heroOpacity > 0.99,
    JSON.stringify(reducedState)
  );
  check(
    "reduced motion: Play is replaced by manual step controls, starting on step 1",
    reducedState.play === false &&
      reducedState.prev === true &&
      reducedState.next === true &&
      reducedState.activeStep === 0,
    JSON.stringify(reducedState)
  );

  const stepped = await evaluate(
    client,
    `const next = document.querySelector("[data-flow-next]");
     next.click(); next.click();
     const after = [...document.querySelectorAll("[data-flow-step]")].findIndex(s => s.classList.contains("is-active"));
     const status = document.querySelector("[data-flow-status]").textContent.trim();
     document.querySelector("[data-flow-prev]").click();
     const back = [...document.querySelectorAll("[data-flow-step]")].findIndex(s => s.classList.contains("is-active"));
     return { after, back, status };`
  );
  check(
    "reduced motion: the manual controls swap state immediately",
    stepped.after === 2 && stepped.back === 1,
    JSON.stringify(stepped)
  );

  // Switching to reduced motion mid-play must stop the sequence.
  await setReducedMotion(client, false);
  await goto(client, `${ORIGIN}/`);
  await sleep(600);
  await evaluate(client, `document.querySelector("[data-flow-play]").click(); return 1;`);
  await sleep(600);
  await setReducedMotion(client, true);
  await sleep(1800);
  const interrupted = await evaluate(
    client,
    `${VISIBLE} return { play: vis("[data-flow-play]"), next: vis("[data-flow-next]"),
              active: [...document.querySelectorAll("[data-flow-step]")].findIndex(s => s.classList.contains("is-active")) };`
  );
  check(
    "reduced motion: turning it on mid-play stops the sequence and hands over manual controls",
    interrupted.play === false && interrupted.next === true,
    JSON.stringify(interrupted)
  );

  // ---- Scripting disabled ----------------------------------------------
  await setReducedMotion(client, false);
  await client.send("Emulation.setScriptExecutionDisabled", { value: true });
  await goto(client, `${ORIGIN}/`);
  await sleep(500);
  const noScript = await evaluate(client, `return 1;`).catch(() => null);
  const noJs = await client.send("Runtime.evaluate", {
    expression: `(() => {
      const t = (sel) => document.querySelector(sel)?.textContent?.trim() ?? "";
      const op = (sel) => Number(getComputedStyle(document.querySelector(sel)).opacity);
      return JSON.stringify({
        headline: t("#hero-title"),
        eyebrow: t(".hero .studio-kicker"),
        lead: t(".studio-lead").slice(0, 40),
        offers: [...document.querySelectorAll(".offer h3")].map(h => h.textContent.trim()),
        steps: [...document.querySelectorAll("[data-flow-step] h3")].map(h => h.textContent.trim()),
        revealsVisible: [...document.querySelectorAll("[data-reveal]")].every(el => Number(getComputedStyle(el).opacity) > 0.99),
        heroPanelOpacity: op("[data-hero-panel]"),
      });
    })()`,
    returnByValue: true,
  });
  const noJsState = JSON.parse(noJs.result.value);
  check(
    "no scripting: the offer, the three offers and all three workflow steps still read",
    noJsState.headline === "Make everyday work easier for your team." &&
      noJsState.offers.length === 3 &&
      noJsState.steps.length === 3 &&
      noJsState.revealsVisible === true &&
      noJsState.heroPanelOpacity > 0.99,
    JSON.stringify(noJsState)
  );
  await client.send("Emulation.setScriptExecutionDisabled", { value: false });

  void noScript;
} finally {
  client.close();
  proc.kill("SIGTERM");
}

const failed = results.filter((r) => !r.pass);
console.log(`\n${results.length - failed.length}/${results.length} checks passed`);
process.exit(failed.length ? 1 : 0);
