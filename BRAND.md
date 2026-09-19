# Brand assets & placeholder variables

Everything visual about the Summaverick identity — the logo, the tab icon, the
social preview, and the contact link — is driven from **one file:**

```
brand.config.json
```

Change a value there, run one command, and every page + every image is
regenerated. You never hand-edit 100+ HTML files or open an image editor.

## The variables

`brand.config.json` holds:

| Key | What it controls |
|---|---|
| `name` / `fullName` / `legalName` | Brand names used in artwork |
| `tagline` / `ogTagline` | Strapline on the About logo and the social card |
| `inquiryEmail` | The address people reach you at |
| `contactHref` | The link behind every "Build with us" / "Contact us" CTA (`mailto:…`) |
| `colors.ink` / `colors.onInk` | Dark ground and light text of the marks |
| `colors.metalStops` | The five-stop brushed-metal gradient of the "S" |
| `assets.*` | Where each generated file lives (paths referenced by the site) |

## Update the logo, icon, or social card

The logo is **vector art**, defined once in `scripts/lib/brand-art.mjs` (the
folded-ribbon "S"). To restyle it, edit the metal colours in
`brand.config.json`, or the `S_PATH` / `S_STROKE` geometry in that file, then:

```bash
pnpm brand
```

This runs two steps:

1. **`pnpm brand:assets`** — redraws every raster/vector file from the config:
   - `favicon.svg` + `favicon.ico` (the browser tab icon)
   - `apple-touch-icon.png`, the three `android-chrome-*` icons
   - `img/summaverick-mark.{svg,png,webp}` (bare mark)
   - `img/summaverick-logo.{png,webp}` (1200×630 social / OG card)
   - `img/summaverick-group-logo.{jpg,webp}` (the square lock-up in the About section)
2. **`pnpm brand:apply`** — rewrites the inline nav/footer "S" and re-points
   every contact CTA in `public/**/*.html`.

> Asset generation rasterises the SVG with the bundled Chromium (no ImageMagick
> needed). If the browser lives elsewhere, set `CHROME_BIN=/path/to/chrome`.

## Change the contact email

Set `inquiryEmail` **and** `contactHref` in `brand.config.json`, then:

```bash
pnpm brand:apply
```

Every "Build with us", "Contact us", and the "Prefer email?" link on the
homepage — anything tagged `data-brand="contact"` — is updated in one pass.

## Swap in a completely different logo image

If you'd rather use a supplied render than the vector art for the large
lock-ups, drop your file over the matching path in `public/assets/img/` (keep
the filename), and skip `brand:assets` for that file. The favicon and nav mark
should stay vector so they stay sharp at 16px.
