# Brand assets & placeholder variables

Everything visual about the Summaverick identity — the logo, the tab icon, the
social preview, the header/footer mark, and the contact link — is driven from
**one file:**

```
brand.config.json
```

Change a value there, run one command, and every page + every image is
regenerated. You never hand-edit 100+ HTML files or open an image editor.

## The logo is your render — never redrawn

The master logo is your supplied metallic "S" lock-up:

```
public/assets/img/summaverick-group-logo.jpg
```

Every other image is **cropped and resized from that file** — the artwork is
never recreated. `source.markCrop` in `brand.config.json` is the square (in
master-image pixels) that isolates just the "S" for the icon and the
header/footer marks:

```json
"source": {
  "masterImage": "assets/img/summaverick-group-logo.jpg",
  "markCrop": { "x": 312, "y": 288, "size": 620 }
}
```

## The variables

| Key | What it controls |
|---|---|
| `name` / `fullName` / `legalName` | Brand names |
| `tagline` | Footer / About strapline |
| `inquiryEmail` / `contactHref` | The address + link behind every "Build with us" / "Contact us" CTA |
| `colors.ink` | The ink field behind the social card |
| `source.masterImage` | Your master logo render |
| `source.markCrop` | The square that isolates the "S" for small marks |
| `marks.navSize` / `marks.footerSize` | Rendered pixel size of the header/footer badge |
| `assets.*` | Where each generated file lives |

## Replace the logo

1. Drop your new render over `public/assets/img/summaverick-group-logo.jpg`.
2. If the "S" sits in a different spot, update `source.markCrop` (x/y/size are
   pixels in the new file — the top-left of a square around just the "S").
3. Run:

```bash
pnpm brand
```

This runs two steps:

1. **`pnpm brand:assets`** — re-crops/resizes every derived file from the master:
   - `favicon.svg` + `favicon.ico` (browser tab icon)
   - `apple-touch-icon.png`, the three `android-chrome-*` icons
   - `img/summaverick-mark.{png,webp}` — the header/footer badge
   - `img/summaverick-logo.{png,webp}` — the 1200×630 social / OG card
   - `img/summaverick-group-logo.webp` — a webp sibling of the master
2. **`pnpm brand:apply`** — rewrites the nav/footer `<img>` badge and re-points
   every contact CTA in `public/**/*.html`.

> Cropping/resizing uses the bundled Chromium (no ImageMagick/sharp). If the
> browser lives elsewhere, set `CHROME_BIN=/path/to/chrome`.

## Change just the contact email

Set `inquiryEmail` **and** `contactHref`, then:

```bash
pnpm brand:apply
```

Every "Build with us", "Contact us", and the "Prefer email?" link on the
homepage — anything tagged `data-brand="contact"` — updates in one pass.

## Resize the header/footer mark only

Change `marks.navSize` / `marks.footerSize` and run `pnpm brand:apply`.
