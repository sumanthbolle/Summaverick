# Decisions

Running log of decisions that shape the build. Newest first.

## 2026-08-31 — Company homepage, not a scroll-scene personal site

**Decision.** Replace the five-scene GSAP/Lenis homepage with a conventional
company site. Copy, information architecture, and chrome live in HTML. The
research agent remains a product on `/` (preview) and `/ask`, not the identity
of the firm.

**Why.** The scroll narrative read as generated marketing HTML: numbered scenes,
first-person "I build", pill CTAs, empty `data-*` shells hydrated from JS.
Enterprise buyers looking for Store certification, an implementation kickstart,
AI work, a product collaboration, or fine-tuning could not find those offers.

**What shipped.** Five capability sections matching those offers; engagement
shapes; selected work in company voice; a four-column footer; a mobile nav;
contact intents that match the work. GSAP/Lenis are no longer loaded on `/`.
Scene modules remain in the repo unused.

## 2026-08-26 — Re-enable auto-deploy on merge to master

**Decision.** Reverses the "defer Cloudflare" gate below now that the site is
meant to go live. `deploy.yml` runs on every push to `master` (and still on
manual dispatch); `ci.yml` now runs on pull requests only, so a master commit
isn't tested twice.

**Why.** A merge was expected to appear on summaverick.com and didn't. Two
causes: (1) deploy was manual-only, so merges never deployed; (2) more
fundamentally, the Cloudflare deploy had never once succeeded — every run
failed in <1s at the credential preflight because `CLOUDFLARE_API_TOKEN` /
`CLOUDFLARE_ACCOUNT_ID` are not set as repo secrets, so `wrangler deploy` never
ran and the Worker was never published.

**Prerequisite (human, one-time).** Auto-deploy only produces a live site once
these repo secrets exist: `CLOUDFLARE_API_TOKEN` (Workers + D1 + KV + R2 Edit),
`CLOUDFLARE_ACCOUNT_ID` (Account ID, not a Zone ID), optional
`PERPLEXITY_API_KEY`; and the D1/KV/R2 resources exist with summaverick.com on
that account (RUNBOOK §1). Until then the deploy job fails fast at the
credential check while `ci.yml` stays green — the failure is the signal that
setup is incomplete, not a code regression.

## 2026-08-26 — Live agent (T8): single streaming endpoint, no Durable Object

**Decision.** The live agent streams over a single SSE connection
(`POST /api/research/stream`) rather than the handover's three-route +
Durable Object design. No DO means it runs entirely in local `wrangler dev`
(Miniflare) with no Cloudflare account — consistent with the Cloudflare
deferral below. The DO can be added later, when a run needs to be resumed or
watched by more than one client; nothing here blocks that.

**Routes.** `POST /api/research/stream` (SSE: accepted → classify → expand →
injection gate → retrieve → per-layer → dedup → verify → answer → done),
`POST /api/research` (non-streaming `{answer, trace}`), and
`GET /api/research/:id` (a completed run, from D1 + the trace in R2).

**Safety + limits, as the handover requires.**
- Rate limited per client (IP, falling back to device): 6/min burst and 50/day,
  returning a clean 429 with `retry-after`.
- The query is scanned for prompt injection **before any model call**; a hit is
  streamed as a visible `blocked` event and the run stops. Query capped at 500.
- Instance query stays off by default; `permitWriteOperations` stays false.

**Degrades offline.** With no `PERPLEXITY_API_KEY` the pipeline returns its
deterministic evidence-backed draft (the retrieval/verification trace is
unchanged), so the demo works with no key. The front-end streams the live
trace and falls back to a captured trace only when the Worker isn't present
(e.g. static hosting).

**Fixed along the way.** The KV binding in `wrangler.jsonc` was `KV`, but the
Worker reads `env.CONFIG` everywhere — renamed the binding to `CONFIG` so the
rate limiter (and all KV use) works at runtime. Added migration
`0005_research.sql` (`research_runs`, `eval_results`).

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
