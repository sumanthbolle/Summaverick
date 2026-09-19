/**
 * Propagates brand.config.json into every static HTML page. Idempotent — safe
 * to run after any brand change. Run via `pnpm brand:apply` (or `pnpm brand`,
 * which rebuilds the images first).
 *
 * Across public/**.html it:
 *   1. Renders the nav/footer "S" as an <img> badge of the current mark asset.
 *   2. Points every contact call-to-action at config.contactHref (mailto),
 *      tagging it data-brand="contact" so the next email change is one line.
 *
 * Page prose, layout and copy are left untouched.
 */
import { readFileSync, writeFileSync, readdirSync, statSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, resolve, join } from "node:path";

const here = dirname(fileURLToPath(import.meta.url));
const root = resolve(here, "..");
const cfg = JSON.parse(readFileSync(resolve(root, "brand.config.json"), "utf8"));
const publicDir = resolve(root, "public");

const mailto = cfg.contactHref;
const markSrc = cfg.assets.markPng;
const navSize = cfg.marks.navSize;
const footerSize = cfg.marks.footerSize;

const navImg = `<img class="brand-mark" src="${markSrc}" width="${navSize}" height="${navSize}" alt="" />`;
const footerImg = `<img class="brand-mark" src="${markSrc}" width="${footerSize}" height="${footerSize}" alt="" />`;

// Any existing brand mark (inline SVG from an earlier design, or a previously
// applied <img>) — matched by the 26/28 nav size and the 30/32 footer size.
const NAV_MARK = /<(?:svg|img) class="brand-mark"[^>]*width="2[68]"[^>]*(?:\/>|>[\s\S]*?<\/svg>)/g;
const FOOTER_MARK = /<(?:svg|img) class="brand-mark"[^>]*width="3[02]"[^>]*(?:\/>|>[\s\S]*?<\/svg>)/g;

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

  // 1) Nav + footer mark → <img> badge of the current asset.
  html = html.replace(NAV_MARK, () => (marks++, navImg));
  html = html.replace(FOOTER_MARK, () => (marks++, footerImg));

  // 2) Contact CTAs → mailto, tagged for future one-line changes.
  html = html.replace(/href="\/?#contact"/g, `href="${mailto}" data-brand="contact"`);
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
  `apply-brand: ${filesChanged} files changed · ${marks} marks → <img> · ${contacts} contact links at ${mailto}`
);
