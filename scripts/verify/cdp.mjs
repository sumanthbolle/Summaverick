/**
 * Minimal Chrome DevTools Protocol client used by the verification scripts in
 * this directory. Node's global WebSocket is enough; nothing here ships to the
 * site, so it stays dependency-free.
 */
import { spawn } from "node:child_process";

export async function launch({
  headless = true,
  width = 1440,
  height = 900,
  port = 9222,
  left = 0,
  top = 0,
} = {}) {
  const args = [
    `--remote-debugging-port=${port}`,
    "--no-first-run",
    "--no-default-browser-check",
    // OutdatedBuildDetector puts a "Can't update Chrome" bubble over the page,
    // which would sit in the middle of any recording made here.
    "--disable-features=Translate,OutdatedBuildDetector",
    "--disable-component-update",
    "--no-service-autorun",
    "--disable-search-engine-choice-screen",
    "--user-data-dir=/tmp/cdp-profile-" + port,
    `--window-size=${width},${height}`,
    `--window-position=${left},${top}`,
  ];
  if (headless) args.push("--headless=new", "--hide-scrollbars");
  const proc = spawn("google-chrome", [...args, "about:blank"], {
    stdio: "ignore",
    detached: false,
  });

  const target = await waitForTarget(port);
  const client = await connect(target.webSocketDebuggerUrl);
  return { proc, client, port };
}

async function waitForTarget(port, timeoutMs = 20000) {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    try {
      const res = await fetch(`http://127.0.0.1:${port}/json/list`);
      const targets = await res.json();
      const page = targets.find((t) => t.type === "page" && t.webSocketDebuggerUrl);
      if (page) return page;
    } catch {
      /* not up yet */
    }
    await sleep(200);
  }
  throw new Error("Chrome did not expose a debuggable page in time");
}

async function connect(url) {
  const ws = new WebSocket(url);
  await new Promise((resolve, reject) => {
    ws.addEventListener("open", resolve, { once: true });
    ws.addEventListener("error", reject, { once: true });
  });

  let nextId = 1;
  const pending = new Map();
  const listeners = new Map();

  ws.addEventListener("message", (event) => {
    const msg = JSON.parse(event.data);
    if (msg.id && pending.has(msg.id)) {
      const { resolve, reject } = pending.get(msg.id);
      pending.delete(msg.id);
      if (msg.error) reject(new Error(`${msg.error.message} (${JSON.stringify(msg.error)})`));
      else resolve(msg.result);
      return;
    }
    const handlers = listeners.get(msg.method);
    if (handlers) for (const h of handlers) h(msg.params);
  });

  const send = (method, params = {}) =>
    new Promise((resolve, reject) => {
      const id = nextId++;
      pending.set(id, { resolve, reject });
      ws.send(JSON.stringify({ id, method, params }));
    });

  const on = (method, handler) => {
    if (!listeners.has(method)) listeners.set(method, []);
    listeners.get(method).push(handler);
  };

  return { send, on, close: () => ws.close() };
}

export const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

/** Evaluate an expression in the page and return its JSON value. */
export async function evaluate(client, expression) {
  const { result, exceptionDetails } = await client.send("Runtime.evaluate", {
    expression: `(() => { ${expression} })()`,
    returnByValue: true,
    awaitPromise: true,
  });
  if (exceptionDetails) {
    throw new Error(exceptionDetails.exception?.description ?? "evaluate failed");
  }
  return result.value;
}

export async function goto(client, url) {
  await client.send("Page.enable");
  const loaded = new Promise((resolve) => client.on("Page.loadEventFired", resolve));
  await client.send("Page.navigate", { url });
  await loaded;
}

/** A real key press through the browser input pipeline, so :focus-visible applies. */
export async function pressKey(client, { key, code, keyCode, shift = false }) {
  const modifiers = shift ? 8 : 0;
  await client.send("Input.dispatchKeyEvent", {
    type: "rawKeyDown",
    key,
    code,
    windowsVirtualKeyCode: keyCode,
    nativeVirtualKeyCode: keyCode,
    modifiers,
  });
  await client.send("Input.dispatchKeyEvent", {
    type: "keyUp",
    key,
    code,
    windowsVirtualKeyCode: keyCode,
    nativeVirtualKeyCode: keyCode,
    modifiers,
  });
}

export const pressTab = (client, shift = false) =>
  pressKey(client, { key: "Tab", code: "Tab", keyCode: 9, shift });

/** Enter needs a text-carrying keyDown for the browser to activate a button. */
export async function pressEnter(client) {
  for (const type of ["keyDown", "keyUp"]) {
    await client.send("Input.dispatchKeyEvent", {
      type,
      key: "Enter",
      code: "Enter",
      windowsVirtualKeyCode: 13,
      nativeVirtualKeyCode: 13,
      text: type === "keyDown" ? "\r" : undefined,
      unmodifiedText: type === "keyDown" ? "\r" : undefined,
    });
  }
}

export async function setReducedMotion(client, reduce) {
  await client.send("Emulation.setEmulatedMedia", {
    features: [{ name: "prefers-reduced-motion", value: reduce ? "reduce" : "no-preference" }],
  });
}
