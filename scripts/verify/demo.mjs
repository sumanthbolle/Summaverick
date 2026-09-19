/**
 * Drives the homepage on the VM's display so a screen recording shows the real
 * implementation. Not part of the test suite — it only produces the walkthrough.
 *
 *   node scripts/verify/demo.mjs desktop [origin]
 *   node scripts/verify/demo.mjs mobile  [origin]
 *   node scripts/verify/demo.mjs reduced [origin]
 */
import { evaluate, goto, launch, setReducedMotion, sleep } from "./cdp.mjs";

const MODE = process.argv[2] ?? "desktop";
const ORIGIN = process.argv[3] ?? "http://localhost:8788";

const SIZES = {
  desktop: { width: 1920, height: 1200, port: 9300 },
  reduced: { width: 1920, height: 1200, port: 9301 },
  // A phone-width window, centred so the recording is not a sliver in a corner.
  mobile: { width: 430, height: 1150, port: 9302, left: 745, top: 20 },
};
const size = SIZES[MODE];

/** Smooth, readable scrolling — a recording of a jump is hard to follow. */
const glideTo = async (client, selector, { block = "start", pause = 900 } = {}) => {
  await evaluate(
    client,
    `const el = document.querySelector(${JSON.stringify(selector)});
     el.scrollIntoView({ block: ${JSON.stringify(block)}, behavior: "smooth" });
     return 1;`
  );
  await sleep(pause);
};

const moveMouse = async (client, selector) => {
  const box = await evaluate(
    client,
    `const r = document.querySelector(${JSON.stringify(selector)}).getBoundingClientRect();
     return { x: Math.round(r.left + r.width / 2), y: Math.round(r.top + r.height / 2) };`
  );
  await client.send("Input.dispatchMouseEvent", { type: "mouseMoved", x: box.x, y: box.y });
  return box;
};

const clickOn = async (client, selector) => {
  const { x, y } = await moveMouse(client, selector);
  for (const type of ["mousePressed", "mouseReleased"]) {
    await client.send("Input.dispatchMouseEvent", {
      type,
      x,
      y,
      button: "left",
      clickCount: 1,
    });
  }
};

const typeInto = async (client, selector, value) => {
  await clickOn(client, selector);
  for (const char of value) {
    await client.send("Input.insertText", { text: char });
    await sleep(18);
  }
};

const { proc, client } = await launch({
  headless: false,
  width: size.width,
  height: size.height,
  port: size.port,
  left: size.left ?? 0,
  top: size.top ?? 0,
});

try {
  if (MODE === "mobile") {
    // Chrome will not open a window narrower than about 500px, so the window
    // itself is the viewport rather than a scaled emulation — what the recording
    // shows is a real narrow rendering. The exact 360/390px widths are covered
    // by scripts/verify/responsive.mjs.
    await client.send("Emulation.setDeviceMetricsOverride", {
      width: 0,
      height: 0,
      deviceScaleFactor: 0,
      mobile: true,
    });
    await client.send("Emulation.setTouchEmulationEnabled", { enabled: true });
  }
  if (MODE !== "mobile") {
    const { windowId } = await client.send("Browser.getWindowForTarget");
    await client.send("Browser.setWindowBounds", {
      windowId,
      bounds: { windowState: "maximized" },
    });
    await sleep(500);
  }
  await client.send("Network.enable");
  // Own rate-limit bucket, so recording the walkthrough does not spend the
  // allowance a visitor would use.
  await client.send("Network.setExtraHTTPHeaders", {
    headers: { "cf-connecting-ip": `198.51.100.${(Date.now() % 200) + 20}` },
  });
  await setReducedMotion(client, MODE === "reduced");
  // Give the recorder a moment on a blank page before the reload reveals motion.
  await goto(client, `${ORIGIN}/`);
  await sleep(1600);

  if (MODE === "reduced") {
    // Show the hero already settled, then the manual step controls.
    await sleep(1200);
    await glideTo(client, ".flow__panel", { block: "center", pause: 1600 });
    for (const _ of [0, 1]) {
      await clickOn(client, "[data-flow-next]");
      await sleep(1300);
    }
    await clickOn(client, "[data-flow-prev]");
    await sleep(1600);
    await glideTo(client, ".offers", { block: "start", pause: 1500 });
    await glideTo(client, ".examples", { block: "start", pause: 1500 });
  } else {
    if (MODE === "mobile") {
      // Linger on the first screen: the specialty line and both buttons have to
      // stay with the headline at this width.
      await sleep(2000);
      await glideTo(client, "[data-hero-panel]", { block: "center", pause: 1800 });
      await glideTo(client, "#hero-title", { block: "start", pause: 1400 });
      // The menu carries the same destinations as the desktop header.
      await clickOn(client, "#nav-toggle");
      await sleep(2400);
      await clickOn(client, "#nav-toggle");
      await sleep(1200);
    }

    // Hero, then the offers rising in as they enter.
    await glideTo(client, ".offers", { block: "start", pause: 1800 });

    if (MODE === "desktop") {
      await moveMouse(client, ".offers__grid .offer:nth-child(1)");
      await sleep(1100);
      await moveMouse(client, ".offers__grid .offer:nth-child(3)");
      await sleep(1100);
    } else {
      await glideTo(client, ".offers__grid .offer:nth-child(3)", { block: "center", pause: 1400 });
    }

    // The workflow example: play it, pause it, resume, let it finish.
    await glideTo(client, ".flow__panel", { block: "center", pause: 1400 });
    await clickOn(client, "[data-flow-play]");
    await sleep(1500);
    await clickOn(client, "[data-flow-play]"); // pause
    await sleep(1400);
    await clickOn(client, "[data-flow-play]"); // resume
    await sleep(4200);

    // The example card and its disclosure.
    await glideTo(client, ".example", { block: "center", pause: 1300 });
    await clickOn(client, ".disclosure > summary");
    await sleep(2200);
    await clickOn(client, ".disclosure > summary");
    await sleep(900);

    // Contact: type a real message and send it.
    await glideTo(client, ".contact-form", { block: "center", pause: 1200 });
    await typeInto(client, "#lead-name", "Priya Raman");
    await typeInto(client, "#lead-email", "priya@northwind.example");
    await typeInto(client, "#lead-org", "Northwind Ops");
    await typeInto(
      client,
      "#lead-message",
      "Our request intake is spread across email and spreadsheets."
    );
    // Keep the button and the status line that answers it both on screen.
    await glideTo(client, ".contact-form__submit", { block: "center", pause: 1200 });
    await clickOn(client, "[data-lead-submit]");
    await sleep(3500);

    const outcome = await evaluate(
      client,
      `return { status: document.querySelector("[data-lead-status]").textContent.trim(),
               onScreen: (() => {
                 const r = document.querySelector("[data-lead-status]").getBoundingClientRect();
                 return r.top >= 0 && r.bottom <= innerHeight;
               })() };`
    );
    console.log(`contact status shown: "${outcome.status}" (on screen: ${outcome.onScreen})`);
    if (outcome.status !== "Thanks—your message has been received.") {
      throw new Error(`walkthrough did not reach a confirmed send: "${outcome.status}"`);
    }
    await sleep(1500);
  }
} finally {
  client.close();
  proc.kill("SIGTERM");
}
