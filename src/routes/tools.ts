/**
 * Tool endpoints (T9), ported from the legacy Worker (api/worker.js) and served
 * under the /api/ prefix:
 *   GET  /api/trending         landing widgets (news / market / tech), country-aware
 *   GET  /api/servicenow       latest ServiceNow articles across 4 tracks
 *   GET  /api/metals           live gold/silver spot + FX + daily history (D1-backed)
 *   POST /api/flights          SkyFare flight search (Amadeus → Sonar fallback)
 *   POST /api/flights/inspire  Summaverick destination ideas
 *
 * This module is the IO layer: it does the fetches, the Cloudflare Cache API
 * work and (for metals) D1 persistence, then delegates all pure transforms to
 * src/domain/tools.ts. There is intentionally NO CORS — same-origin app.
 *
 * Cache-API TTLs (from api/README.md, preserved exactly):
 *   trending   600 s full / 60 s partial
 *   servicenow 3600 s full / 300 s partial
 *   metals     60 s
 * Cache keys keep the legacy `https://cache.summaverick.internal/...` scheme and
 * every cached endpoint echoes `X-Summaverick-Cache: HIT|MISS`.
 */
import type { Ctx, Env, RouteDef, RouteMaker } from "../types";
import { json, nowMs } from "../lib/json";
import { getMetalSeries, upsertMetalDaily } from "../db/queries";
import {
  amadeusHost,
  amadeusOffersToRaw,
  buildFlightPrompt,
  buildInspirePrompt,
  buildLocalNewsPayload,
  buildMarketPayload,
  buildMetalRows,
  buildServiceNowPayload,
  buildWorldNewsPayload,
  computeBookingTiming,
  COUNTRY_NAME,
  enforceMaxStops,
  enrichBookingUrls,
  extractFlightJson,
  extractInspireJson,
  extractIataCode,
  FLIGHT_SYSTEM_PROMPT,
  INSPIRE_SYSTEM_PROMPT,
  mapAmadeusCabin,
  mergeBookingAdvice,
  mergeHistory,
  METALS_CURRENCIES,
  normalizeFlightResponse,
  normalizeHistory,
  normalizeInspireSuggestion,
  parseFlightParams,
  parseMarketWidget,
  parseNewsWidget,
  parseServiceNowTrack,
  parseTechWidget,
  sanitize,
  scoreFlights,
  seriesToHistory,
  SNOW_TRACKS,
  type FlightParams,
} from "../domain/tools";

/* ─────────────────────────── constants ─────────────────────────── */

const TRENDING_TTL_SECONDS = 600; // 10 min for full-success responses
const TRENDING_PARTIAL_TTL_SECONDS = 60; // 1 min when one or more widgets failed
const TRENDING_REQUEST_TIMEOUT_MS = 9000;

const SNOW_TTL_SECONDS = 3600; // 1 hour when the fetch fully succeeds
const SNOW_PARTIAL_TTL_SECONDS = 300; // 5 min when one or more tracks failed
const SNOW_REQUEST_TIMEOUT_MS = 12000;

const METALS_TTL_SECONDS = 60;
const METALS_REQUEST_TIMEOUT_MS = 8000;

const FLIGHT_TIMEOUT_MS = 55000;
const INSPIRE_TIMEOUT_MS = 35000;

const CACHE_ORIGIN = "https://cache.summaverick.internal";

/* ─────────────────────────── response + cache helpers ─────────────────────────── */

/** Serialise a pre-built JSON string body with the given headers (no CORS). */
function rawJson(body: string, extraHeaders: Record<string, string>): Response {
  return new Response(body, {
    headers: {
      "content-type": "application/json; charset=utf-8",
      "Cache-Control": "no-cache",
      ...extraHeaders,
    },
  });
}

function cacheKeyFor(path: string): Request {
  return new Request(`${CACHE_ORIGIN}${path}`, { method: "GET" });
}

/** Store a response body in the edge cache without blocking the response. */
function putCache(ctx: Ctx, key: Request, body: string, ttl: number): void {
  const toCache = new Response(body, {
    headers: {
      "Content-Type": "application/json",
      "Cache-Control": `public, max-age=${ttl}`,
    },
  });
  ctx.exec.waitUntil(caches.default.put(key, toCache));
}

function pickFulfilled<T>(settled: PromiseSettledResult<T>): T | null {
  return settled.status === "fulfilled" && settled.value ? settled.value : null;
}

function settledValue(result: PromiseSettledResult<any> | undefined): any {
  return result && result.status === "fulfilled" ? result.value : null;
}

/* ─────────────────────────── Sonar / fetch IO ─────────────────────────── */

async function callSonarWithTimeout(
  apiKey: string,
  payload: unknown,
  timeoutMs: number
): Promise<any> {
  const ctrl = new AbortController();
  const timer = setTimeout(() => ctrl.abort(), timeoutMs);
  try {
    const res = await fetch("https://api.perplexity.ai/v1/sonar", {
      method: "POST",
      headers: {
        "Content-Type": "application/json",
        Authorization: "Bearer " + apiKey,
      },
      body: JSON.stringify(payload),
      signal: ctrl.signal,
    });
    if (!res.ok) throw new Error("Sonar " + res.status);
    return await res.json();
  } finally {
    clearTimeout(timer);
  }
}

async function fetchMarketJson(url: string): Promise<any> {
  const controller = new AbortController();
  const timer = setTimeout(() => controller.abort(), METALS_REQUEST_TIMEOUT_MS);
  try {
    const response = await fetch(url, {
      signal: controller.signal,
      headers: { Accept: "application/json" },
      cf: { cacheTtl: METALS_TTL_SECONDS, cacheEverything: true },
    });
    if (!response.ok) throw new Error(`Market data source returned ${response.status}`);
    return response.json();
  } finally {
    clearTimeout(timer);
  }
}

/* ─────────────────────────── trending widgets ─────────────────────────── */

async function fetchNewsWidget(apiKey: string, country: string) {
  const { payload, label } = buildLocalNewsPayload(country);
  const data = await callSonarWithTimeout(apiKey, payload, TRENDING_REQUEST_TIMEOUT_MS);
  return parseNewsWidget(data, "news", label);
}

async function fetchWorldNewsWidget(apiKey: string) {
  const data = await callSonarWithTimeout(
    apiKey,
    buildWorldNewsPayload(),
    TRENDING_REQUEST_TIMEOUT_MS
  );
  return parseNewsWidget(data, "worldNews", "#1 worldwide · Top story");
}

async function fetchMarketWidget(apiKey: string, country: string) {
  const data = await callSonarWithTimeout(
    apiKey,
    buildMarketPayload(country),
    TRENDING_REQUEST_TIMEOUT_MS
  );
  return parseMarketWidget(data, country);
}

/* Top Hacker News story (free, no key). Walks the first 5 topstories until it
 * finds a live item, surfacing Ask/Show HN posts linked to their HN thread. */
async function fetchTechWidget() {
  const ctrl = new AbortController();
  const timer = setTimeout(() => ctrl.abort(), TRENDING_REQUEST_TIMEOUT_MS);
  try {
    const idsRes = await fetch("https://hacker-news.firebaseio.com/v0/topstories.json", {
      signal: ctrl.signal,
      cf: { cacheTtl: 120, cacheEverything: true },
    });
    if (!idsRes.ok) return null;
    const ids = await idsRes.json();
    if (!Array.isArray(ids) || ids.length === 0) return null;

    for (let i = 0; i < Math.min(5, ids.length); i++) {
      const itemRes = await fetch(
        `https://hacker-news.firebaseio.com/v0/item/${ids[i]}.json`,
        { signal: ctrl.signal, cf: { cacheTtl: 120, cacheEverything: true } }
      );
      if (!itemRes.ok) continue;
      const item = await itemRes.json();
      const widget = parseTechWidget(item);
      if (widget) return widget;
    }
    return null;
  } catch {
    return null;
  } finally {
    clearTimeout(timer);
  }
}

async function handleTrending(req: Request, ctx: Ctx): Promise<Response> {
  const cfCountry = (req.cf && (req.cf.country as string)) || null;
  const override =
    (ctx.url.searchParams.get("country") || "").toUpperCase().slice(0, 2) || null;
  const country = override || cfCountry || "GLOBAL";
  const detected = override ? "override" : cfCountry ? "network" : "fallback";

  const cacheKey = cacheKeyFor(`/trending/${country}`);
  const cached = await caches.default.match(cacheKey);
  if (cached) {
    const body = await cached.text();
    return rawJson(body, { "X-Summaverick-Cache": "HIT" });
  }

  const apiKey = ctx.env.PERPLEXITY_API_KEY;
  if (!apiKey) {
    return json({ success: false, error: "API key not configured" });
  }

  /* Local news would duplicate worldNews when the country is GLOBAL, so we skip
   * that fetch and show three widgets instead of four. */
  const localNewsPromise =
    country === "GLOBAL" ? Promise.resolve(null) : fetchNewsWidget(apiKey, country);

  const [newsRes, worldNewsRes, marketRes, techRes] = await Promise.allSettled([
    localNewsPromise,
    fetchWorldNewsWidget(apiKey),
    fetchMarketWidget(apiKey, country),
    fetchTechWidget(),
  ]);

  const widgets = {
    news: pickFulfilled(newsRes),
    worldNews: pickFulfilled(worldNewsRes),
    market: pickFulfilled(marketRes),
    tech: pickFulfilled(techRes),
  };

  const successCount = Object.values(widgets).filter(Boolean).length;

  const payload = {
    success: successCount > 0,
    country,
    countryName:
      COUNTRY_NAME[country] || (country === "GLOBAL" ? "Global" : country),
    detected,
    generatedAt: new Date().toISOString(),
    widgets,
  };

  // Full success = every widget we *tried* to fetch succeeded (3 for GLOBAL, else 4).
  const expected = country === "GLOBAL" ? 3 : 4;
  const ttl =
    successCount >= expected ? TRENDING_TTL_SECONDS : TRENDING_PARTIAL_TTL_SECONDS;
  const responseBody = JSON.stringify(payload);

  if (successCount > 0) putCache(ctx, cacheKey, responseBody, ttl);

  return rawJson(responseBody, { "X-Summaverick-Cache": "MISS" });
}

/* ─────────────────────────── ServiceNow feed ─────────────────────────── */

async function fetchServiceNowTrack(apiKey: string, key: string, recency: string) {
  const payload = buildServiceNowPayload(key, recency);
  if (!payload) return null;
  const data = await callSonarWithTimeout(apiKey, payload, SNOW_REQUEST_TIMEOUT_MS);
  return parseServiceNowTrack(data, key);
}

async function handleServiceNowFeed(_req: Request, ctx: Ctx): Promise<Response> {
  const trackParam = (ctx.url.searchParams.get("track") || "all").toLowerCase();
  const recency = ctx.url.searchParams.get("fresh") === "month" ? "month" : "week";
  const requested =
    trackParam === "all"
      ? Object.keys(SNOW_TRACKS)
      : trackParam
          .split(",")
          .map((t) => t.trim())
          .filter((t) => SNOW_TRACKS[t]);
  const keys = requested.length ? requested : Object.keys(SNOW_TRACKS);

  const cacheKey = cacheKeyFor(`/servicenow/${keys.join("-")}/${recency}`);
  const cached = await caches.default.match(cacheKey);
  if (cached) {
    const body = await cached.text();
    return rawJson(body, { "X-Summaverick-Cache": "HIT" });
  }

  const apiKey = ctx.env.PERPLEXITY_API_KEY;
  if (!apiKey) {
    return json({ success: false, error: "API key not configured" });
  }

  const settled = await Promise.allSettled(
    keys.map((k) => fetchServiceNowTrack(apiKey, k, recency))
  );

  const tracks = settled
    .map((s) => (s.status === "fulfilled" ? s.value : null))
    .filter(Boolean);

  const successCount = tracks.length;
  const payload = {
    success: successCount > 0,
    recency,
    generatedAt: new Date().toISOString(),
    tracks,
  };

  const ttl = successCount >= keys.length ? SNOW_TTL_SECONDS : SNOW_PARTIAL_TTL_SECONDS;
  const responseBody = JSON.stringify(payload);

  if (successCount > 0) putCache(ctx, cacheKey, responseBody, ttl);

  return rawJson(responseBody, { "X-Summaverick-Cache": "MISS" });
}

/* ─────────────────────────── metals ─────────────────────────── */

async function handleMetals(_req: Request, ctx: Ctx): Promise<Response> {
  const requestedCurrency = (
    ctx.url.searchParams.get("currency") || "USD"
  ).toUpperCase();
  if (!METALS_CURRENCIES.has(requestedCurrency)) {
    return json(
      {
        success: false,
        error: "Unsupported currency",
        supportedCurrencies: Array.from(METALS_CURRENCIES),
      },
      { headers: { "Cache-Control": "no-store" } }
    );
  }

  const cacheKey = cacheKeyFor(`/metals/${requestedCurrency}`);
  const cached = await caches.default.match(cacheKey);
  if (cached) {
    const body = await cached.text();
    return rawJson(body, {
      "Cache-Control": `public, max-age=${METALS_TTL_SECONDS}`,
      "X-Summaverick-Cache": "HIT",
    });
  }

  const endDate = new Date().toISOString().slice(0, 10);
  const start = new Date();
  start.setUTCDate(start.getUTCDate() - 35);
  const startDate = start.toISOString().slice(0, 10);
  const historyQuery = `from=${startDate}&to=${endDate}&quotes=USD`;

  const jobs = [
    fetchMarketJson("https://api.gold-api.com/price/XAU"),
    fetchMarketJson("https://api.gold-api.com/price/XAG"),
    fetchMarketJson(`https://api.frankfurter.dev/v2/rates?base=XAU&${historyQuery}`),
    fetchMarketJson(`https://api.frankfurter.dev/v2/rates?base=XAG&${historyQuery}`),
  ];
  if (requestedCurrency !== "USD") {
    jobs.push(
      fetchMarketJson(
        `https://api.frankfurter.dev/v2/rate/USD/${encodeURIComponent(requestedCurrency)}`
      )
    );
  }

  const results = await Promise.allSettled(jobs);
  const goldQuote = settledValue(results[0]);
  const silverQuote = settledValue(results[1]);

  if (
    !goldQuote ||
    !silverQuote ||
    !Number.isFinite(Number(goldQuote.price)) ||
    !Number.isFinite(Number(silverQuote.price)) ||
    Number(goldQuote.price) <= 0 ||
    Number(silverQuote.price) <= 0
  ) {
    // Non-cacheable failure so the client can fall back to its last success.
    return json(
      {
        success: false,
        error: "Live metal prices are temporarily unavailable",
        generatedAt: new Date().toISOString(),
      },
      { headers: { "Cache-Control": "no-store" } }
    );
  }

  const fxPayload = requestedCurrency === "USD" ? null : settledValue(results[4]);
  const fxRate =
    requestedCurrency === "USD"
      ? 1
      : Number(
          fxPayload &&
            (Array.isArray(fxPayload)
              ? fxPayload[0] && fxPayload[0].rate
              : fxPayload.rate)
        );
  const hasConversion = Number.isFinite(fxRate) && fxRate > 0;
  const quoteTimes = [goldQuote.updatedAt, silverQuote.updatedAt]
    .map((time) => new Date(time).getTime())
    .filter(Number.isFinite);
  const hasQuoteTimestamps = quoteTimes.length === 2;
  const sourceUpdatedAt = quoteTimes.length
    ? new Date(Math.min(...quoteTimes)).toISOString()
    : null;
  const ageSeconds = sourceUpdatedAt
    ? Math.max(0, Math.round((Date.now() - new Date(sourceUpdatedAt).getTime()) / 1000))
    : null;

  // Fresh live history from Frankfurter, then blended with the accumulated D1
  // series so charts keep long-run context even though each fetch only returns
  // ~35 days. D1 failures degrade gracefully to the live-only history.
  const now = nowMs();
  const goldLive = normalizeHistory(settledValue(results[2]));
  const silverLive = normalizeHistory(settledValue(results[3]));
  let goldHistory = goldLive;
  let silverHistory = silverLive;
  try {
    const rows = buildMetalRows(settledValue(results[2]), "XAU", "USD", now).concat(
      buildMetalRows(settledValue(results[3]), "XAG", "USD", now)
    );
    if (rows.length) await upsertMetalDaily(ctx.env.DB, rows);
    const goldSeries = await getMetalSeries(ctx.env.DB, "XAU", "USD", startDate);
    const silverSeries = await getMetalSeries(ctx.env.DB, "XAG", "USD", startDate);
    goldHistory = mergeHistory(seriesToHistory(goldSeries), goldLive);
    silverHistory = mergeHistory(seriesToHistory(silverSeries), silverLive);
  } catch {
    /* D1 unavailable — keep the freshly-fetched live history. */
  }

  const payload = {
    success: true,
    currency: requestedCurrency,
    fxRate: hasConversion ? fxRate : null,
    fxDate:
      requestedCurrency === "USD"
        ? endDate
        : String(
            (fxPayload &&
              (Array.isArray(fxPayload)
                ? fxPayload[0] && fxPayload[0].date
                : fxPayload.date)) ||
              ""
          ).slice(0, 10) || null,
    conversionAvailable: hasConversion,
    generatedAt: new Date().toISOString(),
    sourceUpdatedAt,
    quoteTimestampAvailable: hasQuoteTimestamps,
    freshness:
      hasQuoteTimestamps && ageSeconds !== null && ageSeconds <= 300
        ? "live"
        : "delayed",
    metals: {
      gold: {
        symbol: "XAU",
        usdPerOunce: Number(goldQuote.price),
        history: goldHistory,
      },
      silver: {
        symbol: "XAG",
        usdPerOunce: Number(silverQuote.price),
        history: silverHistory,
      },
    },
    sources: {
      live: { name: "Gold API", url: "https://gold-api.com/" },
      historyAndFx: { name: "Frankfurter", url: "https://frankfurter.dev/" },
    },
  };
  const responseBody = JSON.stringify(payload);

  putCache(ctx, cacheKey, responseBody, METALS_TTL_SECONDS);

  return rawJson(responseBody, {
    "Cache-Control": `public, max-age=${METALS_TTL_SECONDS}`,
    "X-Summaverick-Cache": "MISS",
  });
}

/* ─────────────────────────── Amadeus (IO) ─────────────────────────── */

/* Cached client-credentials token, refreshed ~30 s before expiry. */
let amadeusToken: { value: string | null; exp: number } = { value: null, exp: 0 };

async function getAmadeusToken(env: Env): Promise<string> {
  const now = Date.now();
  if (amadeusToken.value && amadeusToken.exp > now + 30000) return amadeusToken.value;
  const body =
    "grant_type=client_credentials" +
    "&client_id=" +
    encodeURIComponent(env.AMADEUS_CLIENT_ID || "") +
    "&client_secret=" +
    encodeURIComponent(env.AMADEUS_CLIENT_SECRET || "");
  const res = await fetch(amadeusHost(env) + "/v1/security/oauth2/token", {
    method: "POST",
    headers: { "Content-Type": "application/x-www-form-urlencoded" },
    body,
  });
  if (!res.ok) throw new Error("Amadeus auth " + res.status);
  const j = (await res.json()) as any;
  if (!j || !j.access_token) throw new Error("Amadeus auth: no token");
  amadeusToken = { value: j.access_token, exp: now + (j.expires_in || 1500) * 1000 };
  return amadeusToken.value!;
}

async function amadeusFetch(url: string, token: string, timeoutMs: number): Promise<Response> {
  const controller = new AbortController();
  const t = setTimeout(() => controller.abort(), timeoutMs || 20000);
  try {
    return await fetch(url, {
      headers: { Authorization: "Bearer " + token },
      signal: controller.signal,
    });
  } finally {
    clearTimeout(t);
  }
}

async function searchAmadeus(
  env: Env,
  p: FlightParams,
  depIata: string,
  arrIata: string
): Promise<any> {
  const token = await getAmadeusToken(env);
  const qs = new URLSearchParams();
  qs.set("originLocationCode", depIata);
  qs.set("destinationLocationCode", arrIata);
  qs.set("departureDate", p.departureDate);
  if (p.tripType === "round-trip" && p.returnDate) qs.set("returnDate", p.returnDate);
  qs.set("adults", String(Math.max(1, p.passengers)));
  qs.set("travelClass", mapAmadeusCabin(p.cabinClass));
  qs.set("currencyCode", p.currency);
  qs.set("max", "20");
  if (p.maxStops === "nonstop") qs.set("nonStop", "true");
  if (p.maxBudget) qs.set("maxPrice", String(Math.floor(p.maxBudget)));
  const res = await amadeusFetch(
    amadeusHost(env) + "/v2/shopping/flight-offers?" + qs.toString(),
    token,
    20000
  );
  if (!res.ok) {
    let body = "";
    try {
      body = await res.text();
    } catch {
      /* ignore */
    }
    throw new Error("Amadeus search " + res.status + " " + body.slice(0, 160));
  }
  const j = await res.json();
  return amadeusOffersToRaw(j, p, depIata, arrIata);
}

/* ─────────────────────────── flight search ─────────────────────────── */

async function handleFlightSearch(req: Request, ctx: Ctx): Promise<Response> {
  try {
    let body: any;
    try {
      body = await req.json();
    } catch {
      return json({ success: false, error: "Invalid request body." });
    }
    const p = parseFlightParams(body);
    if (!p.origin) return json({ success: false, error: "Origin is required." });
    if (!p.destination) return json({ success: false, error: "Destination is required." });
    if (!p.departureDate)
      return json({ success: false, error: "Departure date is required." });

    /* Primary source: Amadeus real inventory (when configured and IATA codes resolve). */
    const depIata = extractIataCode(p.origin);
    const arrIata = extractIataCode(p.destination);
    if (ctx.env.AMADEUS_CLIENT_ID && ctx.env.AMADEUS_CLIENT_SECRET && depIata && arrIata) {
      try {
        const amRaw = await searchAmadeus(ctx.env, p, depIata, arrIata);
        if (amRaw && amRaw.flights && amRaw.flights.length) {
          const an = normalizeFlightResponse(amRaw, p);
          an.flights = enforceMaxStops(an.flights, p.maxStops);
          enrichBookingUrls(an, p);
          const aScored = scoreFlights(an, p.priorityMode, p.maxBudget);
          aScored.booking_advice = mergeBookingAdvice(computeBookingTiming(p), null);
          aScored.source = "amadeus";
          return json({ success: true, data: aScored });
        }
        // No offers from Amadeus → fall through to Sonar (broader, if configured).
      } catch (amErr) {
        try {
          console.log(
            "Amadeus failed; falling back to Sonar:",
            amErr && (amErr as Error).message
          );
        } catch {
          /* ignore */
        }
      }
    }

    const apiKey = ctx.env.PERPLEXITY_API_KEY;
    if (!apiKey)
      return json({ success: false, error: "Flight search service is not configured." });
    const payload = {
      model: "sonar-pro",
      messages: [
        { role: "system", content: FLIGHT_SYSTEM_PROMPT },
        { role: "user", content: buildFlightPrompt(p) },
      ],
      temperature: 0.1,
      max_tokens: 4096,
      web_search_options: { search_context_size: "high" },
    };
    const data = await callSonarWithTimeout(apiKey, payload, FLIGHT_TIMEOUT_MS);
    const content =
      data && data.choices && data.choices[0] && data.choices[0].message
        ? data.choices[0].message.content
        : null;
    if (!content || typeof content !== "string")
      throw new Error("Empty response from search engine.");
    const jsonStr = extractFlightJson(content);
    let parsed: any;
    try {
      parsed = JSON.parse(jsonStr);
    } catch {
      throw new Error("Failed to parse flight data.");
    }
    const normalized = normalizeFlightResponse(parsed, p);
    normalized.flights = enforceMaxStops(normalized.flights, p.maxStops);
    enrichBookingUrls(normalized, p);
    const scored = scoreFlights(normalized, p.priorityMode, p.maxBudget);
    const timing = computeBookingTiming(p);
    scored.booking_advice = mergeBookingAdvice(timing, parsed.booking_advice);
    scored.source = "ai";
    return json({ success: true, data: scored });
  } catch (e) {
    let msg = "Unable to search for flights right now. Please try again later.";
    const m = e && (e as Error).message ? (e as Error).message : "";
    if (m) {
      if (m.indexOf("abort") >= 0)
        msg =
          "The search timed out. AI flight searches can take up to a minute — please try again.";
      else if (m.indexOf("parse") >= 0)
        msg =
          "The AI returned an unexpected response format. Please try a different route or try again.";
      else if (m.indexOf("Sonar") >= 0)
        msg =
          "The AI search service returned an error. This may be a temporary issue — please try again in a moment.";
      else if (m.indexOf("Empty") >= 0)
        msg =
          "The AI search returned no content. Please try a more specific route (e.g. use airport codes like SIN, NRT).";
    }
    return json({ success: false, error: msg });
  }
}

/* ─────────────────────────── flight inspiration ─────────────────────────── */

async function handleFlightInspire(req: Request, ctx: Ctx): Promise<Response> {
  try {
    let body: any;
    try {
      body = await req.json();
    } catch {
      return json({ success: false, error: "Invalid request body." });
    }
    const query = sanitize(String(body.query || "")).slice(0, 400);
    const originHint = sanitize(String(body.origin || "")).slice(0, 80);
    const currency =
      sanitize(String(body.currency || "USD")).toUpperCase().slice(0, 3) || "USD";
    if (!query || query.length < 5) {
      return json({ success: false, error: "Describe your trip in a short sentence." });
    }

    const apiKey = ctx.env.PERPLEXITY_API_KEY;
    if (!apiKey) {
      return json({ success: false, error: "Inspiration service is not configured." });
    }

    const payload = {
      model: "sonar",
      messages: [
        { role: "system", content: INSPIRE_SYSTEM_PROMPT },
        { role: "user", content: buildInspirePrompt(query, originHint, currency) },
      ],
      temperature: 0.2,
      max_tokens: 1600,
      web_search_options: { search_context_size: "medium" },
    };

    const data = await callSonarWithTimeout(apiKey, payload, INSPIRE_TIMEOUT_MS);
    const content =
      data && data.choices && data.choices[0] && data.choices[0].message
        ? data.choices[0].message.content
        : null;
    const parsed = extractInspireJson(content);
    if (!parsed || !Array.isArray(parsed.suggestions)) {
      throw new Error("Could not parse destination suggestions.");
    }
    const suggestions = parsed.suggestions
      .map((s: any) => normalizeInspireSuggestion(s, currency, query))
      .filter(Boolean)
      .slice(0, 3);
    if (!suggestions.length) throw new Error("No usable destination suggestions returned.");

    return json({
      success: true,
      data: { query, suggestions, source: "summaverick" },
    });
  } catch (e) {
    return json({
      success: false,
      error:
        (e && (e as Error).message) ||
        "Could not suggest destinations. Try a clearer trip description.",
    });
  }
}

/* ─────────────────────────── route table ─────────────────────────── */

export function toolsRoutes(route: RouteMaker): RouteDef[] {
  return [
    route("GET", "/api/trending", handleTrending),
    route("GET", "/api/servicenow", handleServiceNowFeed),
    route("GET", "/api/metals", handleMetals),
    route("POST", "/api/flights", handleFlightSearch),
    route("POST", "/api/flights/inspire", handleFlightInspire),
  ];
}
