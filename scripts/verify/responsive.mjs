/**
 * Responsive, keyboard and contrast checks at the widths named in the brief.
 *
 *   node scripts/verify/responsive.mjs [origin]
 *
 * Every number here is read out of a real layout rather than asserted from the
 * stylesheet, so a passing run means the page behaved, not that a rule exists.
 */
import { evaluate, goto, launch, pressTab, setReducedMotion, sleep } from "./cdp.mjs";

const ORIGIN = process.argv[2] ?? "http://localhost:8788";
const WIDTHS = [360, 390, 768, 1024, 1440];
const PAGES = ["/", "/ask", "/learn"];

const results = [];
const check = (name, pass, detail = "") => {
  results.push({ name, pass, detail });
  console.log(`${pass ? "PASS" : "FAIL"}  ${name}${detail ? `\n        ${detail}` : ""}`);
};

const { proc, client } = await launch({ headless: true, width: 1440, height: 900 });

const setViewport = (width, height = 900) =>
  client.send("Emulation.setDeviceMetricsOverride", {
    width,
    height,
    deviceScaleFactor: 1,
    mobile: width < 768,
  });

try {
  await setReducedMotion(client, false);

  // ---- Horizontal overflow ----------------------------------------------
  for (const path of PAGES) {
    const offenders = [];
    for (const width of WIDTHS) {
      await setViewport(width);
      await goto(client, `${ORIGIN}${path}`);
      await sleep(450);
      const overflow = await evaluate(
        client,
        `const doc = document.documentElement;
         // The honeypot is parked off-screen on purpose and never scrolls.
         const wide = [...document.querySelectorAll("body *")].filter(el => {
           if (el.closest(".hp, .visually-hidden, .skip")) return false;
           const r = el.getBoundingClientRect();
           return r.width > 0 && r.right > doc.clientWidth + 1;
         }).slice(0, 5).map(el => el.tagName.toLowerCase() + (el.className ? "." + String(el.className).split(" ")[0] : ""));
         return { scrollW: doc.scrollWidth, clientW: doc.clientWidth, wide };`
      );
      if (overflow.scrollW > overflow.clientW + 1 || overflow.wide.length) {
        offenders.push(`${width}px: ${JSON.stringify(overflow)}`);
      }
    }
    check(`${path}: no horizontal overflow at ${WIDTHS.join("/")}px`, offenders.length === 0, offenders.join("\n        "));
  }

  // ---- Mobile reading order and stacking --------------------------------
  await setViewport(390, 844);
  await goto(client, `${ORIGIN}/`);
  await sleep(500);
  const mobileOrder = await evaluate(
    client,
    `const copy = document.querySelector(".hero__copy").getBoundingClientRect();
     const panel = document.querySelector("[data-hero-panel]").getBoundingClientRect();
     // Only the shown walk-through: a hidden view reports zero-sized rects,
     // which would read as two steps sharing a line.
     const steps = [...document.querySelectorAll('[data-stage-view]:not([hidden]) [data-stage-step]')].map(s => s.getBoundingClientRect());
     const actions = [...document.querySelectorAll(".hero .btn")].map(b => {
       const r = b.getBoundingClientRect();
       return { label: b.textContent.trim(), top: Math.round(r.top), h: Math.round(r.height) };
     });
     return {
       copyBottom: Math.round(copy.bottom),
       panelTop: Math.round(panel.top),
       stepsStacked: steps.every((r, i) => i === 0 || r.top > steps[i - 1].bottom - 2),
       eyebrowVisible: document.querySelector(".hero .studio-kicker").getBoundingClientRect().height > 0,
       leadVisible: document.querySelector(".hero .studio-lead").getBoundingClientRect().height > 0,
       actions,
     };`
  );
  check(
    "390px: the hero copy reads before the illustration, and the specialty line and both buttons stay with it",
    mobileOrder.panelTop >= mobileOrder.copyBottom - 2 &&
      mobileOrder.eyebrowVisible &&
      mobileOrder.leadVisible &&
      mobileOrder.actions.length === 2,
    JSON.stringify(mobileOrder)
  );
  check(
    "390px: the walk-through steps stack vertically",
    mobileOrder.stepsStacked === true,
    `stacked=${mobileOrder.stepsStacked}`
  );

  // ---- Touch targets ----------------------------------------------------
  const smallTargets = await evaluate(
    client,
    `const controls = [...document.querySelectorAll("a.btn, button, .nav-toggle, .theme-toggle, input, select, textarea")]
       .filter(el => !el.hidden && !el.closest("[hidden]") && el.getAttribute("tabindex") !== "-1")
       .map(el => {
         const r = el.getBoundingClientRect();
         return { el: el.tagName.toLowerCase() + (el.className ? "." + String(el.className).split(" ")[0] : ""),
                  w: Math.round(r.width), h: Math.round(r.height) };
       })
       .filter(t => t.h > 0 && t.h < 44);
     return controls;`
  );
  check(
    "390px: every visible control clears a 44px touch target",
    smallTargets.length === 0,
    smallTargets.length ? JSON.stringify(smallTargets) : "all controls >= 44px tall"
  );

  // ---- Mobile navigation -------------------------------------------------
  const navPanel = await evaluate(
    client,
    `const toggle = document.querySelector("#nav-toggle");
     const panel = document.querySelector("#nav-panel");
     const before = { hidden: panel.hidden, expanded: toggle.getAttribute("aria-expanded") };
     toggle.click();
     const r = panel.getBoundingClientRect();
     const open = { hidden: panel.hidden, expanded: toggle.getAttribute("aria-expanded"),
                    links: [...panel.querySelectorAll("a")].map(a => a.textContent.trim()),
                    width: Math.round(r.width), overflows: r.right > document.documentElement.clientWidth + 1 };
     toggle.click();
     return { before, open, closedAgain: panel.hidden };`
  );
  check(
    "390px: the menu opens, lists the same destinations, and closes again",
    navPanel.before.hidden === true &&
      navPanel.open.hidden === false &&
      navPanel.open.expanded === "true" &&
      navPanel.open.overflows === false &&
      navPanel.open.links.length >= 6 &&
      navPanel.closedAgain === true,
    JSON.stringify(navPanel)
  );

  // ---- Sticky header must not swallow anchor destinations ----------------
  await setViewport(1440, 900);
  await goto(client, `${ORIGIN}/`);
  await sleep(450);
  const anchors = await evaluate(
    client,
    `const header = document.querySelector(".nav").getBoundingClientRect();
     const out = [];
     for (const id of ["build", "examples", "how-we-work", "resources", "contact", "about"]) {
       location.hash = "#" + id;
       const target = document.getElementById(id);
       const heading = target.querySelector("h2");
       const r = (heading ?? target).getBoundingClientRect();
       out.push({ id, headingTop: Math.round(r.top), headerBottom: Math.round(header.bottom),
                  clear: r.top >= header.bottom - 1 });
     }
     return { headerHeight: Math.round(header.height), out };`
  );
  check(
    "anchors: the sticky header never covers the heading it jumps to",
    anchors.out.every((a) => a.clear),
    JSON.stringify(anchors)
  );

  // ---- Heading order and landmarks --------------------------------------
  const structure = await evaluate(
    client,
    `const heads = [...document.querySelectorAll("h1,h2,h3,h4,h5,h6")]
       .filter(h => h.getBoundingClientRect().height > 0)
       .map(h => Number(h.tagName[1]));
     let jumps = [];
     for (let i = 1; i < heads.length; i++) if (heads[i] - heads[i - 1] > 1) jumps.push([heads[i-1], heads[i]]);
     const skip = document.querySelector("a.skip");
     return {
       h1Count: heads.filter(l => l === 1).length,
       jumps,
       skipTarget: skip ? skip.getAttribute("href") : null,
       skipTargetExists: skip ? !!document.querySelector(skip.getAttribute("href")) : false,
       landmarks: {
         header: !!document.querySelector("header"),
         main: !!document.querySelector("main"),
         footer: !!document.querySelector("footer"),
         navLabelled: [...document.querySelectorAll("nav")].every(n => n.hasAttribute("aria-label")),
       },
       sectionsLabelled: [...document.querySelectorAll("main > section")].every(s => s.hasAttribute("aria-labelledby")),
     };`
  );
  check(
    "structure: one h1, no skipped heading levels, labelled landmarks and a working skip link",
    structure.h1Count === 1 &&
      structure.jumps.length === 0 &&
      structure.skipTargetExists &&
      structure.landmarks.navLabelled &&
      structure.sectionsLabelled,
    JSON.stringify(structure)
  );

  // The skip link must be reachable with the very first Tab. Reload first: the
  // anchor checks above moved the sequential-focus starting point.
  const firstTab = await (async () => {
    await goto(client, `${ORIGIN}/`);
    await sleep(500);
    await pressTab(client);
    return evaluate(
      client,
      `const a = document.activeElement;
       const r = a.getBoundingClientRect();
       return { text: a.textContent.trim(), onScreen: r.top >= 0 && r.left >= 0 && r.height > 0 };`
    );
  })();
  check(
    "keyboard: the first Tab reaches a visible Skip to content link",
    /skip to content/i.test(firstTab.text) && firstTab.onScreen === true,
    JSON.stringify(firstTab)
  );

  // ---- Text contrast -----------------------------------------------------
  for (const [path, theme] of [
    ["/", "light"],
    ["/", "dark"],
    ["/ask", "light"],
    ["/ask", "dark"],
  ]) {
    await goto(client, `${ORIGIN}${path}`);
    // Several surfaces transition their background. getComputedStyle reports the
    // in-flight colour, so let the switch settle before measuring.
    await evaluate(client, `document.documentElement.setAttribute("data-theme", "${theme}"); return 1;`);
    await sleep(600);
    const contrast = await evaluate(
      client,
      `const lum = ([r, g, b]) => {
         const f = (c) => { c /= 255; return c <= 0.03928 ? c / 12.92 : Math.pow((c + 0.055) / 1.055, 2.4); };
         return 0.2126 * f(r) + 0.7152 * f(g) + 0.0722 * f(b);
       };
       const parse = (c) => (c.match(/[\\d.]+/g) || []).slice(0, 4).map(Number);
       const over = (fg, bg) => {
         const a = fg[3] === undefined ? 1 : fg[3];
         return [0, 1, 2].map(i => fg[i] * a + bg[i] * (1 - a));
       };
       const bgOf = (el) => {
         let node = el;
         while (node && node !== document.documentElement) {
           const c = parse(getComputedStyle(node).backgroundColor);
           if (c.length >= 3 && (c[3] === undefined || c[3] > 0.99)) return c.slice(0, 3);
           node = node.parentElement;
         }
         const root = parse(getComputedStyle(document.documentElement).backgroundColor);
         return root.length >= 3 ? root.slice(0, 3) : [255, 255, 255];
       };
       const ratio = (a, b) => {
         const [hi, lo] = lum(a) > lum(b) ? [lum(a), lum(b)] : [lum(b), lum(a)];
         return (hi + 0.05) / (lo + 0.05);
       };
       const targets = [...document.querySelectorAll("p, h1, h2, h3, a, label, button, span, li, dt, dd, summary, figcaption")]
         .filter(el => {
           const r = el.getBoundingClientRect();
           if (r.height === 0 || r.width === 0) return false;
           const t = [...el.childNodes].some(n => n.nodeType === 3 && n.textContent.trim());
           return t;
         });
       const bad = [];
       for (const el of targets) {
         const s = getComputedStyle(el);
         const fg = over(parse(s.color), bgOf(el));
         const r = ratio(fg, bgOf(el));
         const size = parseFloat(s.fontSize);
         const bold = Number(s.fontWeight) >= 700;
         const large = size >= 24 || (size >= 18.66 && bold);
         const need = large ? 3 : 4.5;
         if (r < need) {
           bad.push({ tag: el.tagName.toLowerCase(), cls: String(el.className).split(" ")[0],
                      text: el.textContent.trim().slice(0, 30), ratio: Number(r.toFixed(2)), need });
         }
       }
       return { checked: targets.length, bad: bad.slice(0, 8), badCount: bad.length };`
    );
    check(
      `contrast ${path} (${theme}): body and heading text meets WCAG AA`,
      contrast.badCount === 0,
      `${contrast.checked} text nodes checked${contrast.badCount ? ` — ${JSON.stringify(contrast.bad)}` : ""}`
    );
  }
  await evaluate(client, `document.documentElement.removeAttribute("data-theme"); return 1;`);

  // ---- Layout stability --------------------------------------------------
  await client.send("Emulation.clearDeviceMetricsOverride");
  await setViewport(1440, 900);
  const shift = await (async () => {
    await client.send("Performance.enable").catch(() => {});
    await goto(client, `${ORIGIN}/`);
    await sleep(2500);
    return evaluate(
      client,
      `return new Promise((resolve) => {
         let total = 0;
         try {
           new PerformanceObserver((list) => {
             for (const e of list.getEntries()) if (!e.hadRecentInput) total += e.value;
           }).observe({ type: "layout-shift", buffered: true });
         } catch (e) { return resolve({ error: String(e) }); }
         setTimeout(() => resolve({ cls: Number(total.toFixed(4)) }), 600);
       });`
    );
  })();
  check(
    "layout: entrance motion does not shift the page (CLS from buffered entries)",
    typeof shift.cls === "number" && shift.cls <= 0.1,
    JSON.stringify(shift)
  );
} finally {
  client.close();
  proc.kill("SIGTERM");
}

const failed = results.filter((r) => !r.pass);
console.log(`\n${results.length - failed.length}/${results.length} checks passed`);
process.exit(failed.length ? 1 : 0);
