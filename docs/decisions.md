# Decisions

Running log of decisions that shape the build. Newest first.

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
