/**
 * Trending widgets for /ask-summaverick, ported from the legacy Worker
 * (api/worker.js):
 *   GET  /api/trending         landing widgets (news / market / tech), country-aware
 *
 * This module is the IO layer: it does the fetches and the Cloudflare Cache API
 * work, then delegates all pure transforms to src/domain/tools.ts. There is
 * intentionally NO CORS — same-origin app.
 *
 * Cache-API TTL (from api/README.md, preserved exactly): 600 s full / 60 s
 * partial. Cache keys keep the legacy `https://cache.summaverick.internal/...`
 * scheme and responses echo `X-Summaverick-Cache: HIT|MISS`.
 */
import type { Ctx, RouteDef, RouteMaker } from "../types";
import { json } from "../lib/json";
import { callPerplexity, perplexityConfig, type PerplexityConfig } from "../lib/perplexity";
import { askFallbackUrl, fetchFallback } from "../lib/fallback";
import {
  buildLocalNewsPayload,
  buildMarketPayload,
  buildWorldNewsPayload,
  COUNTRY_NAME,
  parseMarketWidget,
  parseNewsWidget,
  parseTechWidget,
} from "../domain/tools";

/* ─────────────────────────── constants ─────────────────────────── */

const TRENDING_TTL_SECONDS = 600; // 10 min for full-success responses
const TRENDING_PARTIAL_TTL_SECONDS = 60; // 1 min when one or more widgets failed
const TRENDING_REQUEST_TIMEOUT_MS = 9000;
/** The page gives up on trending after 12 s; leave the fallback a little less. */
const TRENDING_FALLBACK_TIMEOUT_MS = 11_000;

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

/* ───────────────────────── Perplexity / fetch IO ───────────────────────── */

/** One Perplexity call, as a Sonar-shaped body (see lib/perplexity.ts). */
async function askPerplexity(
  pplx: PerplexityConfig,
  payload: unknown,
  timeoutMs: number
): Promise<any> {
  const { data } = await callPerplexity({
    ...pplx,
    payload: payload as Record<string, unknown>,
    timeoutMs,
  });
  return data;
}

/* ─────────────────────────── trending widgets ─────────────────────────── */

async function fetchNewsWidget(pplx: PerplexityConfig, country: string) {
  const { payload, label } = buildLocalNewsPayload(country);
  const data = await askPerplexity(pplx, payload, TRENDING_REQUEST_TIMEOUT_MS);
  return parseNewsWidget(data, "news", label);
}

async function fetchWorldNewsWidget(pplx: PerplexityConfig) {
  const data = await askPerplexity(
    pplx,
    buildWorldNewsPayload(),
    TRENDING_REQUEST_TIMEOUT_MS
  );
  return parseNewsWidget(data, "worldNews", "#1 worldwide · Top story");
}

async function fetchMarketWidget(pplx: PerplexityConfig, country: string) {
  const data = await askPerplexity(
    pplx,
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

  const pplx = perplexityConfig(ctx.env);
  if (!pplx) {
    const fallbackUrl = askFallbackUrl(ctx.env, "/trending");
    const data = fallbackUrl
      ? await fetchFallback<{ success?: unknown; widgets?: unknown }>(
          country === "GLOBAL" ? fallbackUrl : `${fallbackUrl}?country=${encodeURIComponent(country)}`,
          {},
          TRENDING_FALLBACK_TIMEOUT_MS
        )
      : null;
    if (!data || !data.widgets || typeof data.widgets !== "object") {
      return json({ success: false, error: "API key not configured" });
    }
    const responseBody = JSON.stringify(data);
    putCache(ctx, cacheKey, responseBody, TRENDING_TTL_SECONDS);
    return rawJson(responseBody, { "X-Summaverick-Cache": "MISS" });
  }

  /* Local news would duplicate worldNews when the country is GLOBAL, so we skip
   * that fetch and show three widgets instead of four. */
  const localNewsPromise =
    country === "GLOBAL" ? Promise.resolve(null) : fetchNewsWidget(pplx, country);

  const [newsRes, worldNewsRes, marketRes, techRes] = await Promise.allSettled([
    localNewsPromise,
    fetchWorldNewsWidget(pplx),
    fetchMarketWidget(pplx, country),
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

/* ─────────────────────────── route table ─────────────────────────── */

export function toolsRoutes(route: RouteMaker): RouteDef[] {
  return [
    route("GET", "/api/trending", handleTrending),
  ];
}
