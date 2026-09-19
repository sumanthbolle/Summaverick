/**
 * Single source of truth for the Summaverick "S" ribbon mark, expressed as
 * vector art so it stays razor-sharp from a 16px favicon to a 1200px OG card.
 *
 * The shape is a flat ribbon folded into an S: a horizontal tab at the top
 * right, a bowl that opens down-left, a waist through the centre, a bowl that
 * opens up-right, and a horizontal tab at the bottom left. It is drawn as a
 * single stroked centre-line with butt caps, so the ribbon keeps one constant
 * width and the caps read as the folded end-tabs.
 *
 * Colours and wordmark text come from brand.config.json via the build script;
 * the geometry lives here. Nothing in this file ships to the site — it feeds
 * scripts/build-brand-assets.mjs, which writes the PNG/WebP/SVG/ICO assets.
 */

// The ribbon centre-line, in a 0..100 box. Butt-capped + fat-stroked below.
export const S_PATH =
  "M80 30 H51 C25 30 25 52 51 52 C77 52 77 74 51 74 H20";

export const S_STROKE = 16; // ribbon thickness

const FONT =
  "Geist, ui-sans-serif, system-ui, 'Segoe UI', Roboto, Helvetica, Arial, sans-serif";

const DEFAULT_METAL = ["#fdfdff", "#c9ccd4", "#8b8f99", "#c3c6cf", "#6f727b"];

/** Just the ribbon path element, taking its paint from the caller. */
export function markPath({ stroke = "currentColor" } = {}) {
  return `<path d="${S_PATH}" fill="none" stroke="${stroke}" stroke-width="${S_STROKE}" stroke-linecap="butt" stroke-linejoin="round"/>`;
}

/** Monochrome mark on a transparent ground (nav, footer — inherits currentColor). */
export function markSvg({ stroke = "currentColor" } = {}) {
  return `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100" role="img" aria-label="Summaverick"><path d="${S_PATH}" fill="none" stroke="${stroke}" stroke-width="${S_STROKE}" stroke-linecap="butt" stroke-linejoin="round"/></svg>`;
}

// Brushed-metal ramp used on the dark app icons and the OG card.
function metalDefs(id, stops = DEFAULT_METAL) {
  const offsets = [0, 0.28, 0.52, 0.74, 1];
  const line = stops
    .map((c, i) => `<stop offset="${offsets[i] ?? i / (stops.length - 1)}" stop-color="${c}"/>`)
    .join("");
  return `<defs><linearGradient id="${id}" x1="0" y1="0" x2="1" y2="1">${line}</linearGradient></defs>`;
}

/** Rounded dark tile with the metallic S — the favicon / app-icon artwork. */
export function iconSvg({ size = 128, radius = 0.22, bg = "#0d0d11", pad = 20, stops } = {}) {
  const r = Math.round(size * radius);
  const inner = size - pad * 2;
  const scale = inner / 100;
  return `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 ${size} ${size}" role="img" aria-label="Summaverick">
  ${metalDefs("metal", stops)}
  <rect width="${size}" height="${size}" rx="${r}" fill="${bg}"/>
  <g transform="translate(${pad} ${pad}) scale(${scale})">${markPath({ stroke: "url(#metal)" })}</g>
</svg>`;
}

/** Maskable icon: same mark, more breathing room, full-bleed dark ground. */
export function maskableSvg({ size = 512, bg = "#0d0d11", stops } = {}) {
  const pad = size * 0.28;
  const inner = size - pad * 2;
  const scale = inner / 100;
  return `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 ${size} ${size}" role="img" aria-label="Summaverick">
  ${metalDefs("metalM", stops)}
  <rect width="${size}" height="${size}" fill="${bg}"/>
  <g transform="translate(${pad} ${pad}) scale(${scale})">${markPath({ stroke: "url(#metalM)" })}</g>
</svg>`;
}

/**
 * Horizontal lock-up (metallic S + wordmark on ink) for the OG / social card.
 * 1200x630 is the platform standard for og:image.
 */
export function ogSvg({
  width = 1200,
  height = 630,
  bg = "#0d0d11",
  onInk = "#f4f4f7",
  stops,
  fullName = "Summaverick Group",
  tagline = "Personal assistants and enterprise agents.",
} = {}) {
  return `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 ${width} ${height}" role="img" aria-label="${fullName}">
  ${metalDefs("metalOg", stops)}
  <rect width="${width}" height="${height}" fill="${bg}"/>
  <g transform="translate(118 214) scale(1.95)">${markPath({ stroke: "url(#metalOg)" })}</g>
  <text x="430" y="298" textLength="600" lengthAdjust="spacingAndGlyphs" font-family="${FONT}" font-size="70" font-weight="600" fill="${onInk}" letter-spacing="-1">${fullName}</text>
  <text x="432" y="352" textLength="560" lengthAdjust="spacingAndGlyphs" font-family="${FONT}" font-size="33" font-weight="400" fill="#b6b6c2">${tagline}</text>
</svg>`;
}

/**
 * Square lock-up (metallic S over the wordmark) — the About-section brand image.
 * Mirrors the render the founder supplied: S centred on ink, name beneath.
 */
export function squareSvg({
  size = 1024,
  bg = "#0d0d11",
  stops,
  fullName = "Summaverick Group",
} = {}) {
  return `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 ${size} ${size}" role="img" aria-label="${fullName}">
  ${metalDefs("metalSq", stops)}
  <rect width="${size}" height="${size}" fill="${bg}"/>
  <g transform="translate(${size / 2 - 205} 250) scale(4.1)">${markPath({ stroke: "url(#metalSq)" })}</g>
  <text x="${size / 2}" y="850" text-anchor="middle" textLength="720" lengthAdjust="spacingAndGlyphs" font-family="${FONT}" font-size="84" font-weight="500" fill="#e7e7ec" letter-spacing="1">${fullName}</text>
</svg>`;
}

/** The inline SVG used in every page's nav and footer (theme-aware currentColor). */
export function inlineMark({ width = 28, height = 28, cls = "brand-mark" } = {}) {
  return `<svg class="${cls}" viewBox="0 0 100 100" width="${width}" height="${height}" aria-hidden="true" focusable="false"><path d="${S_PATH}" fill="none" stroke="currentColor" stroke-width="${S_STROKE}" stroke-linecap="butt" stroke-linejoin="round"/></svg>`;
}
