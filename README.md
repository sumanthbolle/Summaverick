# summaverick

Learning + tools platform for **summaverick.com** — quiz, blog, interviews,
tutorials, UPSC, SkyFare (flights), metals, and ServiceNow research — built as a
single Cloudflare **Worker with Static Assets**, backed by **D1** (relational),
**R2** (long bodies / archives), and **KV** (hot config).

One origin serves both the HTML and the API, so there is **no CORS** and no
`ALLOWED_ORIGIN` secret. Correct quiz answers are graded server-side and never
shipped to the browser.

> This repo is the learning/tools half of the estate. The personal brand stays
> on `sumanthbolle.com` / `sumanthbolle.github.io`. Slugs here are kept identical
> to the old paths so the old domain can 301 to them later (Phase 6).

## Stack

| Concern | Choice |
|---|---|
| Hosting | Worker + Static Assets (not Pages) |
| Database | D1 (primary region APAC) |
| Blobs | R2 (`summaverick-content`) |
| Hot config | KV (`CONFIG`) |
| Feed caching | Cache API (trending/servicenow/metals) |
| Auth | Passkeys (WebAuthn) + email magic-link; admin behind Cloudflare Access |
| Language | TypeScript |
| Package manager | pnpm |

## Layout

```
src/
  index.ts         router entry (API + assets, session, cron dispatch)
  routes/          auth, quiz, content, tools, upsc, admin, research
  db/
    schema.ts      row types mirroring migrations
    queries.ts     the ONLY place raw SQL lives
  domain/          pure logic ported from the old api/*.js (no IO)
  lib/             session, webauthn, crypto, ratelimit, cors(=same-origin), json, backup, retention
migrations/        0001_init, 0002_content, 0003_upsc, 0004_metals
scripts/           seed-quiz, seed-content, export-backup, verify (+ _util)
public/            static assets served by the Worker
tests/             vitest unit tests
```

**Hard rule:** no raw SQL outside `src/db/queries.ts`. **Timestamps** are Unix
epoch **milliseconds** as INTEGER, everywhere.

## Local development

```bash
pnpm install
cp .dev.vars.example .dev.vars        # fill in secrets as needed
pnpm run migrate:local                # apply migrations to local D1
pnpm run seed:quiz:local              # 1318 questions -> local D1
pnpm run seed:content:local           # posts+interviews -> local D1 + R2
pnpm run dev                          # wrangler dev (http://localhost:8787)
pnpm run typecheck && pnpm run test
```

> On networks with an outbound proxy, run wrangler with `NO_PROXY=localhost,127.0.0.1`
> so its internal loopback fetches aren't intercepted.

## First-time cloud setup

See **RUNBOOK.md** — it lists the human prerequisites (zone, D1/KV/R2 creation,
secrets, Cloudflare Access) and the exact commands, then the remote Definition-of-Done
checks for every task.

## Secrets (via `wrangler secret put`)

`PERPLEXITY_API_KEY`, `UPSC_PUBLISH_TOKEN`, `SESSION_SECRET`,
`AMADEUS_CLIENT_ID`, `AMADEUS_CLIENT_SECRET`, `RESEND_API_KEY`.
Never commit `.dev.vars`.
