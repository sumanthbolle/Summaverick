# Decisions

Running log of decisions that shape the build. Newest first.

## 2026-08-26 — Scroll narrative: Option A (vanilla + GSAP), libraries vendored

**Decision.** The five-scene scroll narrative (T10) is built Option A from the
handover §1.2: a static page with GSAP ScrollTrigger, no build framework. Smooth
scrolling uses **Lenis** (MIT) rather than GSAP ScrollSmoother, which is a Club
GSAP plugin and not free to redistribute — Lenis is the world-class,
self-hostable equivalent. All three libraries (GSAP, ScrollTrigger, Lenis) are
**vendored** under `public/assets/vendor/`, and the typeface (Geist + Geist Mono,
OFL) under `public/assets/fonts/`. Nothing loads from a CDN, keeping the site
self-contained per the Cloudflare-decoupling decision below.

Pinning uses CSS `position: sticky` on each scene's stage (not ScrollTrigger's
`pin`), which sidesteps the `position: fixed` / `overflow` pin bug the handover
warns about; `overflow: clip` is used throughout regardless. Every scene is
scrub-linked, so scroll-up reverses scroll-down for free, and every scene renders
its final readable state under `prefers-reduced-motion` with no timeline built.

Fixtures for scenes 2–4 and the scoreboard are representative of the agent's real
output shape but are marked `provisional` and must be regenerated from live
`research_runs` once T8/T9 land (they are not rendered as verified metrics).

## 2026-08-26 — Defer the Cloudflare dependency; keep everything runnable from the repo

**Decision.** For now, nothing on the push/PR path depends on a Cloudflare
account, API token, or provisioned D1/KV/R2. The Worker code stays as-is (this
is not a re-platform off Workers), but the repo is self-contained: it builds,
typechecks, tests, and runs locally with no external credentials.

**Why.** The recent history was a run of failed CI deploys fighting Cloudflare
auth (7403s, a D1-scoped token the "Edit Workers" template doesn't grant,
account-vs-zone-ID mix-ups). That coupling blocked every push. Decoupling it
lets the product work move forward under the repo's own control.

**What changed.**

- `.github/workflows/ci.yml` (new) runs on every push/PR: install → typecheck
  → test → `wrangler deploy --dry-run`. The dry run compiles the Worker and
  validates `wrangler.jsonc` bindings **without authenticating**, so the build
  stays honest with no secrets.
- `.github/workflows/deploy.yml` is now **`workflow_dispatch` only** (manual).
  The Cloudflare deploy path is preserved verbatim for when it's wanted, but it
  never runs automatically, so a push or PR can never fail on Cloudflare again.

**Local development is unchanged and needs no account.** `pnpm dev` runs the
Worker under Miniflare with a local D1/KV/R2 simulation; the resource IDs in
`wrangler.jsonc` matter only for a real remote deploy. `pnpm typecheck`,
`pnpm test`, and `pnpm exec wrangler deploy --dry-run` all pass offline.

**Re-enabling Cloudflare later.** Add repo secrets `CLOUDFLARE_API_TOKEN`
(Workers + D1 Edit) and `CLOUDFLARE_ACCOUNT_ID`, point `wrangler.jsonc` at real
D1/KV/R2 resources, then run the `deploy` workflow from the Actions tab. No code
changes are required to turn it back on.

**Scope note.** This is the only change in this pass. The larger handover work
(cutting out-of-scope content, the five scroll scenes, the live agent demo, the
eval scoreboard, the consulting surface) is untouched and still ahead.
