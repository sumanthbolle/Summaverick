# RUNBOOK — bringing summaverick online

This repo was built and verified **locally** (local D1/R2/KV via `wrangler --local`).
The steps here are the ones that need a real Cloudflare account + `summaverick.com`
zone — the things the handover marks as **human prerequisites** — plus the
**remote Definition-of-Done** checks that could not be run without live infra.

Work top to bottom. Each block ends with the check that proves it worked.

---

## 0. Prerequisites (human)

- [ ] `summaverick.com` nameservers on Cloudflare, zone **Active**.
- [ ] `npx wrangler login` completed locally.
- [ ] Account ID known: `npx wrangler whoami`.
- [ ] Repo `sumanthbolle/summaverick` exists (this code pushed to `main`).
- [ ] Decide: keep the old Worker `wandering-haze-b394` running until cutover
      (recommended); this is a separate deployment.

## 1. Create D1 / KV / R2 and wire ids

```bash
npx wrangler d1 create summaverick --location apac
npx wrangler kv namespace create CONFIG
npx wrangler r2 bucket create summaverick-content
```

Put the returned ids into `wrangler.jsonc`:
- `d1_databases[0].database_id` ← the D1 UUID (replaces `REPLACE_WITH_D1_UUID`)
- `kv_namespaces[0].id` ← the KV id (replaces `REPLACE_WITH_KV_ID`)

**Check:** `npx wrangler d1 execute summaverick --remote --command "SELECT 1 AS ok"`
returns a row.

## 2. Secrets

```bash
npx wrangler secret put PERPLEXITY_API_KEY
npx wrangler secret put UPSC_PUBLISH_TOKEN
npx wrangler secret put SESSION_SECRET          # any long random string
npx wrangler secret put AMADEUS_CLIENT_ID
npx wrangler secret put AMADEUS_CLIENT_SECRET
npx wrangler secret put RESEND_API_KEY
```

`ALLOWED_ORIGIN` is intentionally **not** used (same origin).

## 3. Migrations + seeds (remote)

```bash
npx wrangler d1 migrations apply summaverick --remote           # T3
node scripts/seed-quiz.ts --remote                              # T6  -> 1318 questions
node scripts/seed-content.ts --remote                           # T8  -> 52 posts + 57 interviews (D1 + R2)
```

> `seed-content --remote` uploads 109 bodies to R2 one object at a time; it
> takes a few minutes. Both seeds are idempotent (safe to re-run).

**Checks:**
- `npx wrangler d1 execute summaverick --remote --command "SELECT category_id, COUNT(*) FROM questions GROUP BY category_id"` → csa 1020, csa_dumps_live 100, csm 66, cis_df 55, grc 21, architecture 17, scripting 15, modules 12, fundamentals 12.
- `... "SELECT kind, COUNT(*) FROM content GROUP BY kind"` → post 52, interview 57.

## 4. Deploy + custom domain

`wrangler.jsonc` already declares the custom-domain routes for `summaverick.com`
and `www.summaverick.com`. Deploy:

```bash
npx wrangler deploy
```

**Checks (the remote DoDs):**
- **T1** `curl https://summaverick.com/api/health` → `{"ok":true,"ts":...,"db":"ok"}`; `curl https://summaverick.com/` → HTML.
- **T7** open `https://summaverick.com/quiz.html` with an empty cache; DevTools Network shows < 60 KB before the first question (measured locally at ~18 KB raw / ~5.8 KB gzip). Answer 3, hard-refresh, confirm it resumes at Q4 with the same option order.
- **T8** `curl "https://summaverick.com/api/search?q=GlideAjax"` → ≥ 3 ranked hits with snippets; `GET /api/content/<slug>` returns a body from R2.
- **T9** `curl -sI "https://summaverick.com/api/trending?country=IN" | grep -i summaverick-cache` → `MISS` then `HIT`. (Requires `PERPLEXITY_API_KEY`.) Diff `/api/flights` JSON keys against a saved response from the old worker.
- **T12** submit a ServiceNow question at `https://summaverick.com/research.html`; confirm the answer + trace (classification, layers hit, candidate counts, per-claim verification).

## 5. Backups (T4) — do before real writes

```bash
node scripts/export-backup.ts --remote     # manual dump -> R2 backups/YYYY-MM-DD.sql
```

The nightly Cron Trigger (`0 17 * * *` = 01:00 SGT) runs the same dump from
inside the Worker. Verify a scheduled run locally with
`wrangler dev --test-scheduled` then `curl "http://localhost:8787/cdn-cgi/handler/scheduled?cron=0+17+*+*+*"`.

**Check:** `npx wrangler r2 object get summaverick-content/backups/<date>.sql --file=out.sql --remote` retrieves a non-empty file.

## 6. Auth (T5)

Passkeys need HTTPS + a real browser. On `https://summaverick.com`:
register a passkey, then `SELECT * FROM credentials` shows one row; `GET /api/me`
returns the user; delete the cookie → `GET /api/me` returns 401.
Magic links require a verified Resend sender for `MAGIC_LINK_FROM`.

## 7. Admin behind Cloudflare Access (T10)

In the Cloudflare dashboard → Zero Trust → Access, create an application
covering `summaverick.com/admin*` and `summaverick.com/api/admin*`, policy =
your email. The Worker also checks the `Cf-Access-Authenticated-User-Email`
header as defence in depth. Then `/admin/review` lists flagged UPSC notes and
lets you resolve one.

## 8. UPSC publisher (T10) — point the old workflows here

In `sumanthbolle.github.io`, change the UPSC cron workflows to stop committing
JSON and instead POST to the authenticated Worker routes with the bearer token:

- `POST https://summaverick.com/api/upsc/sources`  (normalized source records)
- `POST https://summaverick.com/api/upsc/notes`    (enriched notes)
- `POST https://summaverick.com/api/upsc/runs`     (open/close a run)

All with `Authorization: Bearer $UPSC_PUBLISH_TOKEN`. The evidence gate is
enforced server-side: a note whose `contentHash` no longer matches its source,
or that carries facts whose locator isn't exactly `officialSummary`, is forced
to `draft`/`needs-review` and enqueued for `/admin/review`.

**Check:** run one publish cycle; `SELECT COUNT(*) FROM upsc_notes WHERE status='published'`
is non-zero and **zero bytes are added to git** in the old repo.

## 9. CI/CD (T11)

Add a repo secret `CLOUDFLARE_API_TOKEN` (Workers Scripts:Edit, D1:Edit,
Workers R2 Storage:Edit, Workers KV:Edit). Push to `main` → `.github/workflows/deploy.yml`
runs typecheck → tests → `d1 migrations apply --remote` → `wrangler deploy` →
`scripts/verify.ts https://summaverick.com`. A failing smoke check fails the deploy.

---

## Known deviations from the brief

- **wrangler.jsonc**: verified against wrangler **4.125.0** — the `assets`,
  `d1_databases` (with `migrations_dir` inside the binding), `kv_namespaces`,
  and `r2_buckets` field names are all current. The brief's `routes[].pattern`
  for www had a stray markdown link artifact; fixed to a plain string.
- **Extra migration `0004_metals.sql`** adds `metals_daily` (T9 asked for it).
- **Seeds** run as chunked SQL files (≤100 statements each) executed via
  `wrangler d1 execute --file`, which gives the same bounded-batch atomicity as
  `db.batch()` while being runnable from CI/local without a deployed Worker.
- **content.id** is namespaced by kind (`post-97`, `interview-218`) because post
  and interview numeric ids overlap and share one PRIMARY KEY; the numeric id is
  preserved in the R2 path (`content/post/97.html`) and recoverable for 301s.
