# Decisions

Running log of decisions that shape the build. Newest first.

## 2026-10-07 — Homepage UI layer (home.css)

**Decision.** The homepage is restyled by a separate stylesheet,
`public/assets/css/home.css`, loaded after `site.css` and scoped to
`body[data-home]`. The words, anchors, section order and scripts are
unchanged; no other page is affected.

**Reference.** The structure follows patterns seen on serval.com: a floating
dark pill nav with one white call to action; a left-aligned hero over a dotted
ground with a large framed dark product panel; alternating light grounds and
ink bands that blend into each other with a soft glow; a bento grid of light
cards on an ink band; a segmented tab control; tall cards for guides; and an
outlined wordmark closing the footer. Nothing of theirs is copied: no copy,
logos, imagery or colours.

**Kept.** The palette stays monochrome, with the existing gradient accent on a
key phrase per section. There is no "trusted by" logo band, because there are
no client logos we are cleared to show; the platforms section takes that place.

**How.** An ink band is a token remap (`.band-ink` points the page ramp at the
ink ramp), so the stage, the demo and the form re-skin themselves with their
existing rules. All colour is a token or a `color-mix` of one. Nothing in the
file animates; it also turns off the hero bloom that used to breathe forever.

**On mobile** the resource cards stack. A sideways-scrolling strip tripped
`scripts/verify/responsive.mjs`'s overflow check and hides two of the three
cards behind a gesture nothing announces.

**Tests.** The five `Homepage public contract` tests that failed before this
change still fail, for the same reasons: they assert hero wording the page no
longer has. They were left alone. `scripts/verify/responsive.mjs` also still
stops at its mobile-order check, which looks for a `.studio-kicker` that the
hero lost earlier.

## 2026-10-06 — One Perplexity client; follow-up answers are server-signed

**Decision.** Every Perplexity call goes through `src/lib/perplexity.ts`, and a
follow-up question's earlier answer reaches the model only if this server
signed it.

**Why the client.** Perplexity announced the end of the Sonar chat-completions
endpoints for 2026-09-27 and moved to the Agent API (`POST /v1/responses`).
Its notes say synchronous and streaming Sonar requests are being rewritten as
Agent API requests, model by model, so the old calls may still work, on
borrowed time. The research answers and the five tool endpoints (trending,
ServiceNow feed, flights, flight inspiration, news widgets) used to build
six Sonar requests, sent by two separate HTTP helpers on two different URLs
(`/chat/completions` and `/v1/sonar`), with different timeout and error handling.

Callers still build Sonar-shaped payloads and read `choices[0].message.content`.
The client sends the request over the Agent API, falls back to Sonar when the
Agent API fails in a way Sonar would not share (rejected request, wrong shape,
5xx, timeout), and returns the Sonar shape either way. `PERPLEXITY_WIRE=agent`
or `sonar` pins one API; unset is `auto`. A failed Agent API call puts it on a
five-minute cooldown per isolate. 401, 403 and 429 are never retried, and a
stream that fails after text has reached the reader is not restarted.

**Not confirmed.** Perplexity's reference was not reachable when this was
written, so what the Agent API accepts comes from secondary descriptions. All of
it is in `buildAgentRequest` and `readAgentResponse`. Not forwarded because
their Agent API names are unknown: `temperature`, `max_tokens`,
`web_search_options` (`user_location`, `search_context_size`). In `auto` a wrong
guess shows up as a `perplexity: agent wire failed (…)` warning carrying
Perplexity's own error text, and the call is still answered over Sonar. Run
one real request on each wire before relying on it.

**A leak, now visible.** The research call sets `disable_search` so `[n]`
can only point at a retrieved source. If a wire drops that flag, or the
response shows the model searched anyway, the answer carries a notice and
`trace.llm.webSearchLeaked` is true. `trace.llm.wire` says which API answered.

**Why signing.** A follow-up carries the earlier turns from the browser. The
model reads an assistant turn as its own earlier words, so a visitor who wrote
one could tell it anything with that authority; the injection scan only looked
at the user turns. Each answer now leaves with `historySig`, an HMAC over its
text keyed by `SESSION_SECRET`; the page echoes it back, and
`authenticateHistory` replays the text only if it verifies and passes the
injection scan. Otherwise the turn becomes "(The earlier answer is not
available.)" and the follow-up is answered without it. The signature is not
stored in the run trace. **With no `SESSION_SECRET` set, no assistant text is
replayed**: that fails closed, and follow-ups lose the previous answer's text.

## 2026-09-28 — /research is a chat assistant; Research is in the main menu

**Decision.** `/research` works like Perplexity, ChatGPT or Claude: a start
screen with a centred composer and suggestions, then a conversation thread
with the composer docked at the bottom, follow-up questions, Stop, Retry,
Copy, Share and New chat. Research is a primary menu item on every page with
the company header (and in the article generator).

**Follow-ups.** The browser sends the last few turns as `history`
(`src/domain/research/conversation.ts` shape-checks it, keeps three pairs,
caps lengths and drops any pair whose question trips the injection scan). A
follow-up that does not name ServiceNow things on its own is searched together
with the previous question; the model sees the thread but may still cite only
the sources retrieved for the current turn. The thread is kept in
sessionStorage for the tab; nothing about a conversation is stored server-side
beyond the individual runs.

## 2026-09-28 — One research product at /research, written answers streamed with citations

**Decision.** `/research` is the single ServiceNow research product, on the
company design system. `/ask`, `/ask.html` and `/ask-summaverick` 301 to it
with the query string kept. The old `/research` page (legacy `app.css` chrome)
and `/ask` are gone.

**What changed.**
- The answer model is on wherever `PERPLEXITY_API_KEY` is set (production).
  The answer streams to the page as it is written (`delta` events), after the
  layers, the evidence gate and the numbered sources (`sources` event), so a
  reader can open the sources before the answer finishes.
- The model is asked for `[n]` citations against the numbered sources and runs
  with `disable_search`, so a number can only point at a retrieved page. If an
  account rejects that flag the call is retried without it. `checks` now
  reports how many sources the answer cites and any number that matches no
  source; an answer with no citations carries a notice saying so.
- Answers render from Markdown into DOM nodes (`lib/markdown.js`); nothing
  from the model is assigned as HTML, and only http(s) links are built.
- Every finished run is saved with its result, so `/research?run=<id>` replays
  it. `GET /api/research/evals` serves the eval scoreboard shown on the page.
- `RESEARCH_MODEL` optionally overrides the Perplexity model (default `sonar`).

**Unchanged.** Rate limits, the injection gate before any model call, the
read-only policy, the instance layer off by default, and the rule that answer
quality is reported as not evaluated rather than "verified".

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
