# SkyFare / Summaverick Cloudflare Worker (versioned source)

This directory is the **checked-in source of truth** (DEF-002 / DEF-014) for the
Worker deployed at `https://wandering-haze-b394.sumanthbolle312.workers.dev`.

- `worker.js` — the Worker (kept in sync with `../../api/worker.js`).
- `wrangler.toml` — deploy config.

## Routes
- `POST /flights` — SkyFare flight search + book-redirect enrichment.
- `POST /flights/inspire` — Summaverick destination suggestions (JSON) for one-click SkyFare search.
- `POST /` — Summaverick chat.
- `GET /trending`, `GET /servicenow` — landing/hub widgets.
- `GET /metals` — precious-metals spot references (when deployed from `api/worker.js`).

## `/flights` source priority

1. **Amadeus Self-Service** (real inventory) — used when `AMADEUS_CLIENT_ID` +
   `AMADEUS_CLIENT_SECRET` are set and both origin/destination resolve to IATA
   codes. Calls Flight Offers Search v2, maps offers into the SkyFare schema,
   then reuses the existing scoring / booking-timing / redirect logic.
2. **Perplexity Sonar** (AI-researched) — fallback when Amadeus is not
   configured, returns no offers, or errors.

Every response carries `data.source` (`"amadeus"` | `"ai"`) so the UI can label
fare provenance ("Live fares via Amadeus" vs "AI-researched fares").

Get free Amadeus test credentials at https://developers.amadeus.com (the test
environment has a free monthly request quota; production is billed).

## Deploy

```bash
cd workers/flights
wrangler deploy
wrangler secret put PERPLEXITY_API_KEY        # chat + AI fallback fares
wrangler secret put ALLOWED_ORIGIN            # e.g. https://sumanthbolle.com
wrangler secret put AMADEUS_CLIENT_ID         # Amadeus Self-Service key
wrangler secret put AMADEUS_CLIENT_SECRET     # Amadeus Self-Service secret
# AMADEUS_ENV defaults to "test"; set to "production" in wrangler.toml [vars] when ready.
```

## Verify Amadeus after deploy

```bash
curl -s -X POST https://<your-worker-domain>/flights \
  -H 'Content-Type: application/json' \
  -d '{"origin":"Singapore (SIN)","destination":"Tokyo (NRT)","departureDate":"2026-09-10","tripType":"one-way","passengers":1,"cabinClass":"economy","currency":"USD","maxStops":"2+","priorityMode":"best_balance"}' \
  | head -c 400
# Look for "source":"amadeus" in the response.
```

> Keep `worker.js` here identical to `api/worker.js`. The `api/` copy is the
> historical location referenced by existing docs; this folder adds a
> Wrangler-ready project so the Worker can be versioned and redeployed cleanly.

## Frontend resilience

Even if this Worker is down, `flights.html` degrades gracefully:
- automatic retry on transient failures,
- clear error messaging for 405/429/5xx/timeout,
- a **Demo mode** fallback that shows illustrative sample flights so users are
  never left at a dead end (see `buildDemoData` in `flights.html`).
