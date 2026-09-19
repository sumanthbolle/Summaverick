/**
 * Regenerates every brand image from vector art + brand.config.json:
 *
 *   favicon.svg                          metallic S on a rounded ink tile
 *   favicon.ico                          16/32/48 PNG-in-ICO
 *   apple-touch-icon.png                 180
 *   android-chrome-192 / -512 / maskable app / PWA icons
 *   img/summaverick-mark.svg/.png/.webp  the bare mark (mono + metallic)
 *   img/summaverick-logo.png/.webp       OG / social lock-up (1200x630)
 *   img/summaverick-group-logo.jpg/.webp square lock-up (About section)
 *
 * No native image tooling exists here, so a headless Chromium draws each SVG
 * onto a canvas and returns PNG/JPEG/WebP bytes. Run via `pnpm brand:assets`.
 */
import { readFileSync, writeFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, resolve } from "node:path";
import { launchChrome, renderSvg } from "./lib/rasterize.mjs";
import {
  iconSvg,
  maskableSvg,
  ogSvg,
  squareSvg,
  markSvg,
} from "./lib/brand-art.mjs";

const here = dirname(fileURLToPath(import.meta.url));
const root = resolve(here, "..");
const cfg = JSON.parse(readFileSync(resolve(root, "brand.config.json"), "utf8"));
const stops = cfg.colors.metalStops;
const bg = cfg.colors.ink;
const onInk = cfg.colors.onInk;
const P = (p) => resolve(root, "public", p.replace(/^\//, ""));

/** Minimal ICO container holding PNG images (widely supported). */
function encodeIco(pngs) {
  const header = Buffer.alloc(6);
  header.writeUInt16LE(0, 0);
  header.writeUInt16LE(1, 2); // type: icon
  header.writeUInt16LE(pngs.length, 4);
  const dir = Buffer.alloc(16 * pngs.length);
  let offset = 6 + dir.length;
  const bodies = [];
  pngs.forEach(({ size, buf }, i) => {
    const b = i * 16;
    dir.writeUInt8(size >= 256 ? 0 : size, b + 0);
    dir.writeUInt8(size >= 256 ? 0 : size, b + 1);
    dir.writeUInt8(0, b + 2); // palette
    dir.writeUInt8(0, b + 3); // reserved
    dir.writeUInt16LE(1, b + 4); // planes
    dir.writeUInt16LE(32, b + 6); // bpp
    dir.writeUInt32LE(buf.length, b + 8);
    dir.writeUInt32LE(offset, b + 12);
    offset += buf.length;
    bodies.push(buf);
  });
  return Buffer.concat([header, dir, ...bodies]);
}

const { proc, client } = await launchChrome();
const wrote = [];
const write = (path, buf) => {
  writeFileSync(P(path), buf);
  wrote.push(`${path} (${(buf.length / 1024).toFixed(1)} KB)`);
};

try {
  const icon = (size) => iconSvg({ size: 128, bg, stops });
  const png = (svg, w, h) => renderSvg(client, { svg, width: w, height: h, format: "png" });
  const webp = (svg, w, h) => renderSvg(client, { svg, width: w, height: h, format: "webp", quality: 0.92 });
  const jpeg = (svg, w, h) => renderSvg(client, { svg, width: w, height: h, format: "jpeg", quality: 0.9 });

  // --- Favicon: SVG (modern tab icon) + ICO fallback (16/32/48) -------------
  const faviconSvg = iconSvg({ size: 128, bg, stops });
  writeFileSync(P(cfg.assets.faviconSvg), faviconSvg);
  wrote.push(`${cfg.assets.faviconSvg} (svg)`);
  const icoPngs = [];
  for (const s of [16, 32, 48]) icoPngs.push({ size: s, buf: await png(faviconSvg, s, s) });
  write(cfg.assets.faviconIco, encodeIco(icoPngs));

  // --- App / PWA icons ------------------------------------------------------
  write(cfg.assets.appleTouch, await png(iconSvg({ size: 128, bg, stops }), 180, 180));
  write(cfg.assets.androidChrome192, await png(iconSvg({ size: 128, bg, stops }), 192, 192));
  write(cfg.assets.androidChrome512, await png(iconSvg({ size: 128, bg, stops }), 512, 512));
  write(cfg.assets.androidChromeMaskable, await png(maskableSvg({ size: 512, bg, stops }), 512, 512));

  // --- Bare mark (mono SVG for reuse, plus metallic raster) -----------------
  writeFileSync(P(cfg.assets.markSvg), markSvg());
  wrote.push(`${cfg.assets.markSvg} (svg)`);
  const markMetal = iconSvg({ size: 128, bg: "none", radius: 0, pad: 8, stops });
  write("/assets/img/summaverick-mark.png", await png(markMetal, 256, 256));
  write("/assets/img/summaverick-mark.webp", await webp(markMetal, 256, 256));

  // --- OG / social lock-up (1200x630) ---------------------------------------
  const og = ogSvg({ bg, onInk, stops, fullName: cfg.fullName, tagline: cfg.ogTagline });
  write(cfg.assets.wordmarkPng, await png(og, 1200, 630));
  write("/assets/img/summaverick-logo.webp", await webp(og, 1200, 630));

  // --- Square lock-up (About section) ---------------------------------------
  const sq = squareSvg({ size: 1024, bg, stops, fullName: cfg.fullName });
  write(cfg.assets.squareLogoJpg, await jpeg(sq, 1024, 1024));
  write("/assets/img/summaverick-group-logo.webp", await webp(sq, 1024, 1024));

  console.log("Brand assets written:\n  " + wrote.join("\n  "));
} finally {
  client.close();
  proc.kill();
}
