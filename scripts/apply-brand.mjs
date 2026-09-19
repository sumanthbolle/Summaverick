/**
 * Propagates brand.config.json into every static HTML page. Idempotent — safe
 * to run after any brand change. Run via `pnpm brand:apply` (or `pnpm brand`,
 * which also rebuilds the image assets first).
 *
 * It does three things across public/**.html:
 *   1. Swaps the inline nav/footer "S" mark to the current vector art.
 *   2. Points every contact call-to-action at config.contactHref (mailto),
 *      tagging it data-brand="contact" so the next email change is one line.
 *   3. Reports what changed.
 *
 * Page prose, layout and copy are left untouched.
 */
import { readFileSync, writeFileSync, readdirSync, statSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, resolve, join } from "node:path";
import { S_PATH, S_STROKE } from "./lib/brand-art.mjs";

const here = dirname(fileURLToPath(import.meta.url));
const root = resolve(here, "..");
const cfg = JSON.parse(readFileSync(resolve(root, "brand.config.json"), "utf8"));
const publicDir = resolve(root, "public");

// The exact markup the old brand used, and the new stroked replacement.
const OLD_VIEWBOX = 'viewBox="12 12 78 84"';
const NEW_VIEWBOX = 'viewBox="0 0 100 100"';
const OLD_PATH =
  '<path fill="currentColor" d="M90 12H38a26 26 0 0 0 0 52h24a6 6 0 0 1 0 12H22l-8 20h48a26 26 0 0 0 0-52H38a6 6 0 0 1 0-12h44Z"/>';
const NEW_PATH = `<path d="${S_PATH}" fill="none" stroke="currentColor" stroke-width="${S_STROKE}" stroke-linecap="butt" stroke-linejoin="round"/>`;

const mailto = cfg.contactHref;

function walk(dir) {
  const out = [];
  for (const name of readdirSync(dir)) {
    const p = join(dir, name);
    if (statSync(p).isDirectory()) out.push(...walk(p));
    else if (name.endsWith(".html")) out.push(p);
  }
  return out;
}

let filesChanged = 0;
let marks = 0;
let contacts = 0;

for (const file of walk(publicDir)) {
  let html = readFileSync(file, "utf8");
  const before = html;

  // 1) Inline mark → current vector art.
  marks += (html.match(new RegExp(escape(OLD_PATH), "g")) || []).length;
  html = html.split(OLD_VIEWBOX).join(NEW_VIEWBOX).split(OLD_PATH).join(NEW_PATH);

  // 2) Contact CTAs → mailto, tagged for future one-line changes.
  //    a. Legacy on-page/home anchors.
  html = html.replace(/href="\/?#contact"/g, `href="${mailto}" data-brand="contact"`);
  //    b. Anything already tagged is re-pointed at the current address.
  html = html.replace(
    /href="mailto:[^"]*" data-brand="contact"/g,
    `href="${mailto}" data-brand="contact"`
  );
  contacts += (html.match(/data-brand="contact"/g) || []).length;

  if (html !== before) {
    writeFileSync(file, html);
    filesChanged++;
  }
}

console.log(
  `apply-brand: ${filesChanged} files changed · ${marks} marks swapped · ${contacts} contact links pointed at ${mailto}`
);

function escape(s) {
  return s.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
}
