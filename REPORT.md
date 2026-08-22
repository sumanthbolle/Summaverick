# summaverick — completion report

Built per the handover brief. Every task below was **verified locally** against
`wrangler --local` (local D1 / R2 / KV via miniflare). DoDs that require live
Cloudflare infra or third-party API keys (custom domain, `--remote`, Perplexity,
Amadeus, real-browser passkeys, the metals price providers) are **not runnable
in this build environment** and are called out per task; they are scripted in
**RUNBOOK.md** for you to run against the real account.

## Environment note

This work was produced in a sandbox with **no Cloudflare account, no `wrangler`
credentials, and no third-party API keys**, and the GitHub App here **cannot
create repositories** (403). So: the empty `sumanthbolle/summaverick` repo must
be created by you (a §3 human prerequisite anyway); all live/`--remote` DoDs are
in RUNBOOK.md. Everything that does not require those was built and verified.

---

## Tasks & DoD evidence

### T1 — Scaffold + empty Worker ✅ (local)
`curl /api/health` → `{"ok":true,"ts":1787378797119,"db":"ok","env":"development"}`;
`/` serves placeholder HTML (200, 815 bytes). Typecheck clean.
*Remote DoD (RUNBOOK §4):* same over `https://summaverick.com`.

### T2 — D1/KV/R2 bindings — deferred to RUNBOOK §1 (needs account)
Bindings declared in `wrangler.jsonc`; ids are placeholders to fill after
`wrangler d1 create` etc. Health reports `db:ok` via the `DB` binding (verified
locally). Remote `wrangler d1 execute … --remote "SELECT 1"` is in RUNBOOK.

### T3 — Migrations ✅ (local)
`wrangler d1 migrations apply summaverick --local` applied 0001–0004.
`SELECT name FROM sqlite_master WHERE type='table'` (excluding sqlite_/fts-shadow/d1_)
lists **17**: the brief's 15 + `content_fts` (virtual) + `metals_daily` (0004).
Re-running migrations → `✅ No migrations to apply!`.

### T4 — Backup before writes ✅ (local)
Invoked the scheduled handler: `curl "/cdn-cgi/handler/scheduled?cron=0+17+*+*+*"` → 200.
`wrangler r2 object get summaverick-content/backups/2026-08-22.sql --local` →
**917,262 bytes**, valid SQL (`PRAGMA foreign_keys=OFF; BEGIN TRANSACTION; …`).
30-day prune by key-date + a CLI companion (`scripts/export-backup.ts` using
`wrangler d1 export`) included.

### T5 — Auth ✅ (local, passkey ceremony is remote)
- `GET /api/me` with no cookie → **401**.
- Magic link: `POST /api/auth/magic` stores **SHA-256** of the token
  (`token_hash` = 64-hex), never the raw token.
- **Anonymous claim** end-to-end: anon `POST /api/quiz/attempt` (user_id NULL) →
  magic `GET /api/auth/callback?token=…` → **302**; `GET /api/me` → the new user;
  the earlier attempt's `user_id` is now set (**CLAIMED**).
- WebAuthn via `@simplewebauthn/server` v13 (no hand-rolled CBOR/COSE).
*Remote DoD (RUNBOOK §6):* register a passkey in a real browser over HTTPS.

### T6 — Seed quiz ✅ (local)
`SELECT category_id, COUNT(*) … GROUP BY` → csa 1020, csa_dumps_live 100, csm 66,
cis_df 55, grc 21, architecture 17, scripting 15, modules 12, fundamentals 12 =
**1318**. Re-running the seed leaves COUNT at 1318 (idempotent). Zero empty-string
optionals (absent → NULL). Validator fails loudly on empty/out-of-range `correct`.

### T7 — Quiz API + payload-lean page ✅ (local)
- Initial payload (HTML + app.css + quiz.css + quiz.js + `/api/quiz/categories`)
  = **~18 KB raw / ~5.8 KB gzip**, far under the 60 KB budget (old site shipped
  an 828 KB answer-included bank).
- The attempt payload carries **no `correct`/`explanation`** keys (verified).
- Answered 3 → server graded each; hard-refresh → `GET /api/quiz/attempt/:id`
  resumes at **question 4** with **identical option ordering**; `responses` = 3.
- Correct answers leave the Worker only per-answered-question and at finish.

### T8 — Content migration ✅ (local)
`SELECT kind, COUNT(*)` → **post 52, interview 57**. `forPersona` parsed to real
JSON arrays (`["Developers","Architects","DevOps"]`, zero python-literal strings).
`GET /api/search?q=GlideAjax` → **16** bm25-ranked hits with snippets.
`GET /api/content/servicenow-now-sdk-fluent-guide` → body fetched from R2 (9,742 bytes).
Caught + fixed a real defect: post & interview numeric ids overlap in one PRIMARY
KEY → ids are now kind-namespaced (`post-97`), numeric id preserved in the R2 path.

### T9 — Tools routes ✅ ported/typechecked (live paths remote)
Split into `src/domain/tools.ts` (pure) + `src/routes/tools.ts` (IO). Cache-API
TTLs preserved exactly (trending 600/60, servicenow 3600/300, metals 60), same
cache keys, `X-Summaverick-Cache: HIT|MISS`. `metals` persists to `metals_daily`
and serves merged history. Flights response schema kept identical
(`{success,data:{search_summary,booking_advice,recommendation,flights,warnings,price_insights,source}}`).
*Not locally verifiable:* the live MISS→HIT + persistence needs outbound to
Perplexity/Amadeus/Frankfurter — the sandbox proxy blocks the price providers
(`/api/metals` returned `success:false, "temporarily unavailable"` and correctly
did **not** cache). Remote DoD in RUNBOOK §4.

### T10 — UPSC pipeline off git ✅ (local)
Bearer-authed ingest (`/api/upsc/sources|notes|runs`), public `/api/upsc/feed`,
`/admin/review` (Cloudflare Access-gated; dev bypass). Evidence gate verified:
- backed fact (locator `officialSummary`) + matching hash → **published**
- content_hash mismatch → **forced draft** + review row
- unsupported fact (locator ≠ officialSummary) → **needs-review** + review row
- stored `source_id`/`content_hash` are **copied from the source** (not payload)
- bad bearer → **401**; feed returns published only; admin queue lists both
  reasons and resolve works.
180-day retention sweep archives to R2 JSONL then deletes (cron `0 18 * * *`).

### T11 — CI/CD ✅ (workflow written; `verify.ts` runs green locally)
`.github/workflows/deploy.yml`: push→main runs typecheck → vitest →
`d1 migrations apply --remote` → `wrangler deploy` → `scripts/verify.ts`.
`verify.ts` against the local server: all 4 smoke checks pass (health, 9
categories/1318, content feed, home HTML). Needs repo secret `CLOUDFLARE_API_TOKEN`.

### T12 — Research agent + trace ✅ (local; full trace needs Perplexity)
Ported into `src/domain/research/` (classification, routing, 3 retrieval layers,
evidence gates, security gates, eval harness). `POST /api/research` → full
`trace` (classification, layers, candidate counts, per-claim verdicts, evidence
gates, eval scores); **503 `not_configured`** without `PERPLEXITY_API_KEY`
(retrieval/verification never depend on the LLM). `/research.html` renders the
trace, injection-safe (`textContent`). Bundled 62-case eval suite runs in CI:
**passRate ≥ 0.85, intentAccuracy ≥ 0.85, adversarialBlockRate ≥ 0.85** (green).

---

## Tests
`vitest run` → **13 passed** (11 quiz-domain grading/permutation, 2 research eval/classify).
`tsc --noEmit` → **0 errors** across the whole project.

## D1 row counts (canonical, post-seed, local)

| table | rows |
|---|---|
| quiz_categories | 9 |
| questions | 1318 |
| content | 109 (post 52, interview 57) |
| content_fts | 109 |
| metals_daily | 0 (populated on first successful `/api/metals` fetch) |
| upsc_sources / upsc_notes / upsc_review / publish_runs | 0 canonical (populated by the publisher; exercised with test rows during T10 verification) |
| users / credentials / sessions / magic_links / attempts / responses / events / progress | 0 canonical (created at runtime; exercised during T5/T7 verification) |

## Measured initial payload for /quiz.html
~**18 KB raw / ~5.8 KB gzipped** (HTML + 2 CSS + JS + categories JSON), vs the
old site's ~1.6 MB (incl. the 828 KB scrapeable answer bank).

## Deviations from the brief (with reasons)
1. **wrangler.jsonc** verified against wrangler **4.125.0**; `assets`,
   `d1_databases.migrations_dir` (inside the binding), `kv_namespaces`,
   `r2_buckets` are all current. Fixed the brief's `www` route (stray markdown
   link artifact) to a plain string.
2. **Extra migration `0004_metals.sql`** (`metals_daily`) — T9 required it.
3. **Seeds** run as chunked SQL files (≤100 statements each) via
   `wrangler d1 execute --file` — same bounded-batch atomicity as `db.batch()`,
   but runnable from CI/local without a deployed Worker. Idempotent via ON CONFLICT.
4. **content.id namespaced by kind** (`post-97`, `interview-218`) because post
   and interview numeric ids overlap under one PRIMARY KEY; the numeric id is
   preserved in the R2 key (`content/post/97.html`) for old-URL 301 mapping.
5. **research-agent Node-only pieces shimmed** for the Worker runtime
   (`src/domain/research/runtime/node-shims.ts`, fully commented): fs/child_process/
   crypto → path helpers / empty-fs / unavailable-runner / FNV hash. Layer-1
   SDK-explain and local-repository providers therefore degrade to "no local
   Fluent project" (the truthful Worker state) with all control flow + trace
   preserved. Layer-2 product-docs runs for real (fetch-based).
6. **§8 review-loop skills** (`ui-ux-pro-max`, `gstack`, `ponytail`) were **not**
   run — they live in the old repo's `AGENTS.md` toolchain and need the live app
   + design-review tooling that isn't available here. Flagged for you to run
   post-deploy (they operate on the deployed quiz/research pages).

## Could not verify here (needs live infra — see RUNBOOK)
- T2 remote `d1 execute --remote`; custom-domain `curl https://summaverick.com/*`.
- T9 live `/api/trending`, `/api/servicenow`, `/api/metals`, `/api/flights`
  (Perplexity/Amadeus/Frankfurter outbound blocked by the sandbox proxy).
- T7 DevTools byte measurement in a real browser (measured server-side instead).
- T5 real-browser passkey registration (magic-link + claim verified as the
  equivalent identity path).
- T12 full trace with real Perplexity synthesis (503 path + eval suite verified).
- CI going green/red on GitHub (workflow written; `verify.ts` verified locally).
- **Push to `sumanthbolle/summaverick`** — repo does not exist yet and the GitHub
  App cannot create it. Create it (empty, private) and I/you can push this branch.
