/**
 * Derives every brand image from the founder-supplied render
 * (public/assets/img/summaverick-group-logo.jpg) — the logo itself is never
 * redrawn, only cropped and resized for each slot:
 *
 *   summaverick-mark.png / .webp         the metallic "S" badge (header/footer)
 *   favicon.svg + favicon.ico            the "S" badge, tab icon
 *   apple-touch / android-chrome / mask  app + PWA icons (the "S" badge)
 *   summaverick-logo.png / .webp         social / OG card (full lock-up on ink)
 *   summaverick-group-logo.webp          About-section lock-up (webp sibling)
 *
 * markCrop in brand.config.json is the square (in master pixels) that isolates
 * the "S" from the full lock-up. No native image tooling exists here, so the
 * bundled Chromium does the cropping/scaling. Run via `pnpm brand:assets`.
 */
import { readFileSync, writeFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, resolve } from "node:path";
import { launchChrome, cropImage, composeCentered } from "./lib/rasterize.mjs";

const here = dirname(fileURLToPath(import.meta.url));
const root = resolve(here, "..");
const cfg = JSON.parse(readFileSync(resolve(root, "brand.config.json"), "utf8"));
const ink = cfg.colors.ink;
const P = (p) => resolve(root, "public", p.replace(/^\//, ""));

const masterPath = resolve(root, "public", cfg.source.masterImage);
const master = readFileSync(masterPath).toString("base64");
const { x: cx, y: cy, size: cs } = cfg.source.markCrop;
const crop = { imageBase64: master, mime: "image/jpeg", sx: cx, sy: cy, sw: cs, sh: cs };
const full = { imageBase64: master, mime: "image/jpeg" };

/** Minimal ICO container holding PNG images (widely supported). */
function encodeIco(pngs) {
  const header = Buffer.alloc(6);
  header.writeUInt16LE(1, 2);
  header.writeUInt16LE(pngs.length, 4);
  const dir = Buffer.alloc(16 * pngs.length);
  let offset = 6 + dir.length;
  const bodies = [];
  pngs.forEach(({ size, buf }, i) => {
    const b = i * 16;
    dir.writeUInt8(size >= 256 ? 0 : size, b);
    dir.writeUInt8(size >= 256 ? 0 : size, b + 1);
    dir.writeUInt16LE(1, b + 4);
    dir.writeUInt16LE(32, b + 6);
    dir.writeUInt32LE(buf.length, b + 8);
    dir.writeUInt32LE(offset, b + 12);
    offset += buf.length;
    bodies.push(buf);
  });
  return Buffer.concat([header, dir, ...bodies]);
}

const { proc, client } = await launchChrome({ port: 9333 });
const wrote = [];
const save = (path, buf) => {
  writeFileSync(P(path), buf);
  wrote.push(`${path} (${(buf.length / 1024).toFixed(1)} KB)`);
};

try {
  // Header/footer badge — 128px covers 3x retina at ~30px while staying light.
  save(cfg.assets.markPng, await cropImage(client, { ...crop, out: 128, format: "png" }));
  save("/assets/img/summaverick-mark.webp", await cropImage(client, { ...crop, out: 128, format: "webp", quality: 0.92 }));

  // Favicon: SVG embeds the 128px badge (rounded); ICO carries 16/32/48.
  const badge128 = await cropImage(client, { ...crop, out: 128, format: "png" });
  const faviconSvg = `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 128 128" role="img" aria-label="Summaverick"><defs><clipPath id="r"><rect width="128" height="128" rx="28"/></clipPath></defs><image href="data:image/png;base64,${badge128.toString("base64")}" width="128" height="128" clip-path="url(#r)"/></svg>`;
  writeFileSync(P(cfg.assets.faviconSvg), faviconSvg);
  wrote.push(`${cfg.assets.faviconSvg} (svg)`);
  const icoPngs = [];
  for (const s of [16, 32, 48]) icoPngs.push({ size: s, buf: await cropImage(client, { ...crop, out: s, format: "png" }) });
  save(cfg.assets.faviconIco, encodeIco(icoPngs));

  // App / PWA icons.
  save(cfg.assets.appleTouch, await cropImage(client, { ...crop, out: 180, format: "png" }));
  save(cfg.assets.androidChrome192, await cropImage(client, { ...crop, out: 192, format: "png" }));
  save(cfg.assets.androidChrome512, await cropImage(client, { ...crop, out: 512, format: "png" }));
  // Maskable: the same continuous crop (its own dark ground is the safe zone,
  // so there is no seam against a flat tile).
  save(cfg.assets.androidChromeMaskable, await cropImage(client, { ...crop, out: 512, format: "png" }));

  // Social / OG card: the full lock-up, centred on an ink field (1200x630).
  save(cfg.assets.socialPng, await composeCentered(client, { ...full, width: 1200, height: 630, bg: ink, scale: 0.82, format: "png" }));
  save("/assets/img/summaverick-logo.webp", await composeCentered(client, { ...full, width: 1200, height: 630, bg: ink, scale: 0.82, format: "webp", quality: 0.9 }));

  // About-section lock-up: a webp sibling of the master jpg (master jpg is the
  // founder's file, left untouched).
  save("/assets/img/summaverick-group-logo.webp", await composeCentered(client, { ...full, width: 1024, height: 1024, bg: ink, scale: 1, format: "webp", quality: 0.9 }));

  console.log("Brand assets written:\n  " + wrote.join("\n  "));
} finally {
  client.close();
  proc.kill();
}
