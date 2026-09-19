/**
 * Rasterize an SVG (or arbitrary HTML) into PNG/WebP bytes using the
 * pre-installed Chromium via the DevTools Protocol. No native image tooling
 * (ImageMagick, sharp, cwebp) is available in this environment, but a headless
 * browser can draw an SVG onto a <canvas> and hand back both PNG and WebP,
 * which is all the brand-asset build needs.
 *
 * This file only runs at build time (scripts/build-brand-assets.mjs); nothing
 * here ships to the site.
 */
import { spawn } from "node:child_process";

const CHROME =
  process.env.CHROME_BIN || "/opt/pw-browsers/chromium-1194/chrome-linux/chrome";

export async function launchChrome({ port = 9333 } = {}) {
  const args = [
    `--remote-debugging-port=${port}`,
    "--headless=new",
    "--hide-scrollbars",
    "--no-first-run",
    "--no-default-browser-check",
    "--disable-gpu",
    "--no-sandbox",
    "--force-color-profile=srgb",
    "--disable-features=Translate,OutdatedBuildDetector",
    "--user-data-dir=/tmp/brand-cdp-" + port,
    "about:blank",
  ];
  const proc = spawn(CHROME, args, { stdio: "ignore", detached: false });
  const target = await waitForTarget(port);
  const client = await connect(target.webSocketDebuggerUrl);
  await client.send("Page.enable");
  await client.send("Runtime.enable");
  return { proc, client };
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
    await new Promise((r) => setTimeout(r, 200));
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
  ws.addEventListener("message", (event) => {
    const msg = JSON.parse(event.data);
    if (msg.id && pending.has(msg.id)) {
      const { resolve, reject } = pending.get(msg.id);
      pending.delete(msg.id);
      if (msg.error) reject(new Error(msg.error.message));
      else resolve(msg.result);
    }
  });
  const send = (method, params = {}) =>
    new Promise((resolve, reject) => {
      const id = nextId++;
      pending.set(id, { resolve, reject });
      ws.send(JSON.stringify({ id, method, params }));
    });
  return { send, close: () => ws.close() };
}

async function evaluate(client, expression) {
  const { result, exceptionDetails } = await client.send("Runtime.evaluate", {
    expression: `(async () => { ${expression} })()`,
    returnByValue: true,
    awaitPromise: true,
  });
  if (exceptionDetails) {
    throw new Error(exceptionDetails.exception?.description ?? "evaluate failed");
  }
  return result.value;
}

/**
 * Draw an SVG string onto a canvas of the given pixel size and return the bytes.
 * `format` is "png" or "webp". The SVG is scaled to fill the canvas.
 */
export async function renderSvg(client, { svg, width, height, format = "png", quality = 0.95 }) {
  const svgB64 = Buffer.from(svg, "utf8").toString("base64");
  const mime =
    format === "webp" ? "image/webp" : format === "jpeg" ? "image/jpeg" : "image/png";
  const dataUrl = await evaluate(
    client,
    `
    const img = new Image();
    const src = "data:image/svg+xml;base64,${svgB64}";
    await new Promise((res, rej) => { img.onload = res; img.onerror = rej; img.src = src; });
    const c = document.createElement("canvas");
    c.width = ${width}; c.height = ${height};
    const ctx = c.getContext("2d");
    ctx.imageSmoothingEnabled = true;
    ctx.imageSmoothingQuality = "high";
    ctx.clearRect(0, 0, ${width}, ${height});
    ctx.drawImage(img, 0, 0, ${width}, ${height});
    return c.toDataURL("${mime}", ${quality});
    `
  );
  const base64 = dataUrl.split(",")[1];
  return Buffer.from(base64, "base64");
}
