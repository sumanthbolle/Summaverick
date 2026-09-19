# Brand assets

Public-facing brand: **Summaverick Group**. Legal operator: **SUMMAVERICK LLP**
(LLPIN ADC-1832, formed 14 September 2026, registered in Telangana, India).
`Group` is the brand wording on the owner-supplied logo; it does not imply
subsidiaries, a holding structure, or a registered trademark.

## What lives where

| File | Use |
|---|---|
| `public/assets/img/summaverick-mark.svg` | The header/footer mark. One colour, `fill="currentColor"`, so it follows the theme. |
| `public/assets/favicon.svg` | Tab icon. The same silhouette, light on the ink tile. |
| `public/assets/favicon.ico` | Raster fallback for browsers without SVG icon support; holds 16, 32, and 48 px. |
| `public/assets/apple-touch-icon.png` | 180 px, full bleed — iOS applies its own corner mask. |
| `public/assets/android-chrome-{192,512}.png` · `site.webmanifest` | Installed-app icons. |
| `public/assets/android-chrome-maskable-512x512.png` | The `maskable` entry: full bleed, mark shrunk so its whole bounding box clears the 80 % safe circle. |
| `public/assets/img/summaverick-logo.png` · `.webp` | 1200×630 social preview. |
| `public/assets/img/summaverick-group-logo.webp` · `.jpg` | The owner-supplied metallic composition, used once in the dark About band. |
| `public/assets/img/summaverick-mark.png` · `.webp` | Square raster of the mark, for contexts that cannot take SVG. |

## Rules

- The two SVGs are the masters. The rasters were rendered from them and are
  checked in because deploys ship `public/` as-is.
- The favicon is the **silhouette only**. Never shrink the full square
  composition and its wordmark into a tab icon — the wordmark disappears and
  the metal gradient turns to mud below 48 px.
- The mark has been checked at 16, 24, 32, and 48 px in both appearances.
- A launcher may mask an installed icon to a circle. The rounded tile is fine
  for `any`, but its corners and the S's two tails fall outside that circle,
  so `maskable` points at the padded full-bleed file instead — never at the
  same image.
- The company name always exists as real HTML text next to the mark, and the
  mark itself carries `alt=""` because the text already names the brand.
- The header mark does not animate. The About band's reveal runs once and
  respects `prefers-reduced-motion`.
- A true vector master with light/dark production exports has **not** been
  produced. What is here is a one-colour silhouette drawn to match the supplied
  artwork, plus that artwork itself as a raster.
