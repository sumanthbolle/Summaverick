/**
 * Pure, IO-free logic for the landing-page tool endpoints (trending widgets,
 * ServiceNow feed, metals report, flight search + inspiration). Ported from the
 * legacy Cloudflare Worker (api/worker.js). Everything here is a deterministic
 * transform, prompt/payload builder, scoring function or lookup table — no
 * fetch, cache or DB access. The IO wrappers live in src/routes/tools.ts.
 */
import type { Env } from "../types";
import type { MetalsDailyRow } from "../db/schema";

/* ─────────────────────────── shared utilities ─────────────────────────── */

/** Strip control characters and trim. Mirrors the legacy `sanitize`. */
export function sanitize(str: unknown): string {
  if (!str || typeof str !== "string") return "";
  // eslint-disable-next-line no-control-regex
  return str.replace(/[\x00-\x08\x0B\x0C\x0E-\x1F\x7F]/g, "").trim();
}

/** Return a normalised http(s) URL string, or null when the input is unusable. */
export function safeUrl(url: unknown): string | null {
  try {
    const u = new URL(String(url));
    if (u.protocol !== "https:" && u.protocol !== "http:") return null;
    return u.toString();
  } catch {
    return null;
  }
}

/** Bare hostname (no leading www.), or "" when the URL cannot be parsed. */
export function hostnameOf(url: string): string {
  try {
    return new URL(url).hostname.replace(/^www\./, "");
  } catch {
    return "";
  }
}

/* ───────────────────────── country lookup tables ───────────────────────── */

/* Country → local stock index. Used to bias the markets widget query. "Top
 * mover" is the largest absolute % move (gainer or loser) in the local
 * benchmark index on the most recent trading day. */
export const COUNTRY_TO_INDEX: Record<string, string> = {
  US: "S&P 500", CA: "S&P/TSX Composite", MX: "IPC Mexico",
  GB: "FTSE 100", DE: "DAX 40", FR: "CAC 40", IT: "FTSE MIB",
  ES: "IBEX 35", NL: "AEX", CH: "SMI", SE: "OMXS30",
  IN: "NIFTY 50", JP: "Nikkei 225", CN: "CSI 300", HK: "Hang Seng",
  KR: "KOSPI", SG: "STI", TW: "TAIEX", AU: "ASX 200",
  BR: "Bovespa", AR: "Merval", ZA: "JSE Top 40",
  AE: "ADX General", SA: "Tadawul", IL: "TA-35",
};

export const COUNTRY_NAME: Record<string, string> = {
  US: "United States", CA: "Canada", MX: "Mexico",
  GB: "United Kingdom", DE: "Germany", FR: "France", IT: "Italy",
  ES: "Spain", NL: "Netherlands", CH: "Switzerland", SE: "Sweden",
  IN: "India", JP: "Japan", CN: "China", HK: "Hong Kong",
  KR: "South Korea", SG: "Singapore", TW: "Taiwan", AU: "Australia",
  BR: "Brazil", AR: "Argentina", ZA: "South Africa",
  AE: "United Arab Emirates", SA: "Saudi Arabia", IL: "Israel",
};

/** Build the Sonar web_search_options, adding user_location for a real country. */
export function countryToSonarLocation(
  country: string,
  ctxSize?: string
): Record<string, unknown> {
  const opts: Record<string, unknown> = {
    search_context_size: ctxSize || "medium",
  };
  if (country && country !== "GLOBAL" && /^[A-Z]{2}$/.test(country)) {
    opts.user_location = { country };
  }
  return opts;
}

/** Extract the structured (JSON) content from a Sonar completion, or null. */
export function parseStructured(sonarData: any): any {
  try {
    const content = sonarData?.choices?.[0]?.message?.content;
    if (!content) return null;
    if (typeof content === "object") return content;
    return JSON.parse(content);
  } catch {
    return null;
  }
}

/* ───────────────────────── trending widgets ───────────────────────── */

/* Shared JSON schema for any news fetcher. */
const NEWS_WIDGET_SCHEMA = {
  type: "object",
  properties: {
    headline: { type: "string" },
    summary: { type: "string" },
    source: { type: "string" },
    url: { type: "string" },
  },
  required: ["headline", "summary", "source", "url"],
};

function buildNewsPayload(
  prompt: string,
  webSearchOptions: Record<string, unknown>
): Record<string, unknown> {
  return {
    model: "sonar",
    messages: [
      {
        role: "system",
        content:
          "You return strict JSON only. Do not invent facts. Use only verified, citable news from the past 24 hours.",
      },
      { role: "user", content: prompt },
    ],
    temperature: 0.1,
    max_tokens: 400,
    search_recency_filter: "day",
    web_search_options: webSearchOptions,
    response_format: {
      type: "json_schema",
      json_schema: { schema: NEWS_WIDGET_SCHEMA },
    },
  };
}

/** Payload + label for the local-country top-story widget. */
export function buildLocalNewsPayload(country: string): {
  payload: Record<string, unknown>;
  label: string;
} {
  const region = COUNTRY_NAME[country] || "the world";
  const scope = country === "GLOBAL" ? "around the world" : `in ${region}`;
  const payload = buildNewsPayload(
    `Pick the single most significant news story being widely reported right now ${scope}. Return JSON with the headline, a one-sentence neutral summary, the publishing source name, and the article URL. The story must be from the past 24 hours.`,
    countryToSonarLocation(country, "medium")
  );
  const label =
    country === "GLOBAL"
      ? "Top story · Global"
      : `Top story · ${COUNTRY_NAME[country] || country}`;
  return { payload, label };
}

/** Payload for the #1-worldwide top-story widget. */
export function buildWorldNewsPayload(): Record<string, unknown> {
  return buildNewsPayload(
    "Pick the SINGLE most significant news story being widely reported anywhere in the world in the past 24 hours — the story that the most international outlets are leading with right now. It must be globally notable (not a story only relevant to one country). Return JSON with the headline, a one-sentence neutral summary, the publishing source name, and the article URL.",
    { search_context_size: "high" }
  );
}

/** Shape a parsed news completion into a widget object, or null when empty. */
export function parseNewsWidget(
  data: any,
  kind: string,
  label: string
): Record<string, unknown> | null {
  const parsed = parseStructured(data);
  if (!parsed || !parsed.headline) return null;
  return {
    kind,
    label,
    title: String(parsed.headline).slice(0, 200),
    summary: String(parsed.summary || "").slice(0, 280),
    source: String(parsed.source || "").slice(0, 80),
    url: safeUrl(parsed.url),
  };
}

/** Payload for the market top-mover widget in a country's local index. */
export function buildMarketPayload(country: string): Record<string, unknown> {
  const indexName = COUNTRY_TO_INDEX[country] || "MSCI World";
  const schema = {
    type: "object",
    properties: {
      symbol: { type: "string" },
      name: { type: "string" },
      changePercent: { type: "number" },
      index: { type: "string" },
      reason: { type: "string" },
    },
    required: ["symbol", "name", "changePercent", "index"],
  };
  return {
    model: "sonar",
    messages: [
      {
        role: "system",
        content:
          "You return strict JSON only. Use only verified market data from the most recent trading session.",
      },
      {
        role: "user",
        content: `Identify the single biggest mover (largest absolute percentage change, gainer OR loser) in the ${indexName} index during the most recent completed trading session. Return JSON with the ticker symbol, full company name, percent change with sign (e.g. -4.7 or 12.1), the index name, and a one-sentence reason for the move.`,
      },
    ],
    temperature: 0.1,
    max_tokens: 400,
    search_recency_filter: "day",
    web_search_options: countryToSonarLocation(country, "medium"),
    response_format: { type: "json_schema", json_schema: { schema } },
  };
}

/** Shape a parsed market completion into a widget object, or null. */
export function parseMarketWidget(
  data: any,
  country: string
): Record<string, unknown> | null {
  const indexName = COUNTRY_TO_INDEX[country] || "MSCI World";
  const parsed = parseStructured(data);
  if (!parsed || !parsed.symbol) return null;
  const change = Number(parsed.changePercent);
  if (!Number.isFinite(change)) return null;
  return {
    kind: "market",
    label: `Top mover · ${parsed.index || indexName}`,
    symbol: String(parsed.symbol).slice(0, 12),
    name: String(parsed.name || "").slice(0, 80),
    changePercent: Math.round(change * 100) / 100,
    direction: change >= 0 ? "up" : "down",
    index: String(parsed.index || indexName).slice(0, 60),
    reason: String(parsed.reason || "").slice(0, 240),
  };
}

/** Shape one Hacker News item into the tech widget, or null when unusable. */
export function parseTechWidget(item: any): Record<string, unknown> | null {
  if (!item || item.dead || item.deleted) return null;
  const hnUrl = `https://news.ycombinator.com/item?id=${item.id}`;
  return {
    kind: "tech",
    label: "Top on Hacker News",
    title: String(item.title || "").slice(0, 200),
    url: safeUrl(item.url) || hnUrl,
    hnUrl,
    source: item.url ? hostnameOf(item.url) : "news.ycombinator.com",
    score: Number(item.score) || 0,
    comments: Number(item.descendants) || 0,
    by: String(item.by || "").slice(0, 40),
  };
}

/* ───────────────────── ServiceNow live article feed ───────────────────── */

export const SNOW_TRACKS: Record<
  string,
  { label: string; emoji: string; query: string }
> = {
  ai: {
    label: "Platform AI",
    emoji: "✨",
    query:
      "List the most recent, notable news and articles about ServiceNow platform AI — Now Assist, Now Intelligence, generative AI features, AI Search, and predictive intelligence. Prefer official ServiceNow announcements, blog.servicenow.com, community.servicenow.com, and reputable tech/enterprise-IT outlets.",
  },
  agents: {
    label: "AI Agents",
    emoji: "🤖",
    query:
      "List the most recent, notable news and articles about ServiceNow AI Agents and agentic AI — the AI Agent Orchestrator, AI Agent Studio, autonomous agents, and agentic workflows on the Now Platform. Prefer official ServiceNow sources and reputable enterprise-IT outlets.",
  },
  llm: {
    label: "LLM & GenAI",
    emoji: "🧠",
    query:
      "List the most recent, notable news and articles about ServiceNow large language models and generative AI — the Now LLM, domain-specific models, partnerships with model providers (e.g. Nvidia, Microsoft, Google), and LLM governance on the Now Platform. Prefer official ServiceNow sources and reputable outlets.",
  },
  cost: {
    label: "Cost Optimization",
    emoji: "💰",
    query:
      "List the most recent, notable articles and guidance about ServiceNow cost optimization — licensing and subscription optimization, entitlement management, platform efficiency, reducing technical debt, FinOps for ServiceNow, and lowering total cost of ownership. Prefer official ServiceNow sources, established ServiceNow partners, and reputable enterprise-IT outlets.",
  },
};

const SNOW_FEED_SCHEMA = {
  type: "object",
  properties: {
    articles: {
      type: "array",
      items: {
        type: "object",
        properties: {
          title: { type: "string" },
          summary: { type: "string" },
          source: { type: "string" },
          url: { type: "string" },
          date: { type: "string" },
        },
        required: ["title", "summary", "source", "url"],
      },
    },
  },
  required: ["articles"],
};

/** Payload for one ServiceNow editorial track, or null for an unknown key. */
export function buildServiceNowPayload(
  key: string,
  recency: string
): Record<string, unknown> | null {
  const track = SNOW_TRACKS[key];
  if (!track) return null;
  return {
    model: "sonar",
    messages: [
      {
        role: "system",
        content:
          "You return strict JSON only. Do not invent articles, titles, or URLs. Only include real, citable articles you found via web search, each with a working source URL. Order by most recent first.",
      },
      {
        role: "user",
        content: `${track.query}\n\nReturn JSON: { "articles": [ { "title", "summary" (one neutral sentence), "source" (publication name), "url" (article URL), "date" (ISO or human date if known) } ] }. Return up to 5 articles, most recent first. Only include items you can cite with a real URL.`,
      },
    ],
    temperature: 0.1,
    max_tokens: 900,
    search_recency_filter: recency,
    web_search_options: { search_context_size: "medium" },
    response_format: {
      type: "json_schema",
      json_schema: { schema: SNOW_FEED_SCHEMA },
    },
  };
}

/** Shape a parsed ServiceNow completion into a track object, or null. */
export function parseServiceNowTrack(
  data: any,
  key: string
): Record<string, unknown> | null {
  const track = SNOW_TRACKS[key];
  if (!track) return null;
  const parsed = parseStructured(data);
  if (!parsed || !Array.isArray(parsed.articles)) return null;

  const articles = parsed.articles
    .map((a: any) => ({
      title: String(a.title || "").slice(0, 200),
      summary: String(a.summary || "").slice(0, 300),
      source: String(a.source || "").slice(0, 80),
      url: safeUrl(a.url),
      date: a.date ? String(a.date).slice(0, 40) : null,
    }))
    .filter((a: any) => a.title && a.url);

  if (articles.length === 0) return null;

  return {
    key,
    label: track.label,
    emoji: track.emoji,
    articles: articles.slice(0, 5),
  };
}

/* ───────────────────── precious-metal market report ───────────────────── */

export const METALS_CURRENCIES = new Set([
  "USD", "AED", "AUD", "BRL", "CAD", "CHF", "CNY", "CZK", "DKK", "EUR", "GBP",
  "HKD", "HUF", "IDR", "ILS", "INR", "ISK", "JPY", "KRW", "MXN", "MYR",
  "NOK", "NZD", "PHP", "PLN", "RON", "SEK", "SGD", "THB", "TRY", "ZAR",
]);

export interface MetalHistoryPoint {
  date: string;
  usdPerOunce: number;
}

/** Frankfurter time-series rows → [{ date, usdPerOunce }], date-sorted. */
export function normalizeHistory(rows: any): MetalHistoryPoint[] {
  if (!Array.isArray(rows)) return [];
  return rows
    .map((row: any) => ({
      date: String((row && row.date) || "").slice(0, 10),
      usdPerOunce: Number(row && row.rate),
    }))
    .filter(
      (row) =>
        /^\d{4}-\d{2}-\d{2}$/.test(row.date) && Number.isFinite(row.usdPerOunce)
    )
    .sort((a, b) => a.date.localeCompare(b.date));
}

/** Build D1 daily rows for one metal from a Frankfurter time-series response. */
export function buildMetalRows(
  frankfurterJson: any,
  base: string,
  quote: string,
  fetchedAt: number
): MetalsDailyRow[] {
  return normalizeHistory(frankfurterJson).map((p) => ({
    day: p.date,
    base,
    quote,
    rate: p.usdPerOunce,
    fetched_at: fetchedAt,
  }));
}

/** D1 series rows → chart history points. */
export function seriesToHistory(rows: MetalsDailyRow[]): MetalHistoryPoint[] {
  return rows
    .map((r) => ({ date: r.day, usdPerOunce: Number(r.rate) }))
    .filter((p) => Number.isFinite(p.usdPerOunce))
    .sort((a, b) => a.date.localeCompare(b.date));
}

/**
 * Merge the accumulated D1 series with the freshly-fetched live history so the
 * chart still works on first run (empty DB) and always reflects the newest day.
 * Later entries for the same day win (live overwrites persisted).
 */
export function mergeHistory(
  persisted: MetalHistoryPoint[],
  live: MetalHistoryPoint[]
): MetalHistoryPoint[] {
  const byDay = new Map<string, MetalHistoryPoint>();
  for (const p of persisted) byDay.set(p.date, p);
  for (const p of live) byDay.set(p.date, p);
  return Array.from(byDay.values()).sort((a, b) => a.date.localeCompare(b.date));
}

/* ─────────────────────────── flight search ─────────────────────────── */

export const FLIGHT_SYSTEM_PROMPT =
  'You are a real-time flight search and travel pricing assistant. Your task is to find current publicly available flight options for the provided route, dates, passenger count, cabin class, budget, and preferences.\n\nYou must:\n- Search current web results for flight prices.\n- Prefer airline official websites and reputable travel providers (Google Flights, Kayak, Skyscanner, Expedia, etc.).\n- Return only structured JSON. No markdown, no code fences, no explanation text outside JSON.\n- Do not invent flights, prices, booking links, or airlines.\n- If exact prices are not available, mark the confidence as "low" and explain why in the notes field.\n- Include source URLs wherever possible.\n- Include a freshness note indicating when the data was retrieved.\n- Compare flights based on price, duration, stops, layovers, budget fit, baggage, and carbon emissions.\n- Do not claim that a booking is confirmed.\n- Every flight MUST include a usable booking_url that opens a real search or book page (Google Flights, Kayak, Skyscanner, Expedia, or the airline). Prefer deep links prefilled with the route and dates. If only a source article is available, put it in source_url and still provide a Google Flights or Kayak search URL in booking_url. SkyFare redirects users to complete booking on those sites.\n- Each flight must have a unique "id" field (e.g., "flight_1", "flight_2").\n- The "badges" array can include values like "Cheapest", "Fastest", "Best Overall", "Best Timing", "Under Budget", "Nonstop", "Low CO2", "Great Deal".\n- The "score" field should be your best estimate from 0-100 based on overall value (price, duration, stops, timing, airline quality).\n\nFor every flight also estimate, where known:\n- "co2_kg": estimated carbon emissions per passenger for the whole itinerary, in kilograms (number). Use typical aircraft/route emissions if a precise figure is unavailable; otherwise omit.\n- "fare_brand": the fare family / branded fare (e.g. "Basic Economy", "Main Cabin", "Economy Flex", "Business Saver").\n- "carry_on_included": true/false — whether a full-size cabin bag is included.\n- "checked_bags_included": number of checked bags included in the quoted fare (0 if none).\n- "refundable": true/false if known.\n- "self_transfer": true if the itinerary mixes separate tickets / virtual-interline / non-protected connections (Kiwi-style self-transfer) where the traveller must re-check bags and missed connections are not protected.\nBe honest: prefer omitting a field over guessing a specific figure. Basic Economy / "light" fares usually do not include a carry-on or checked bag — reflect that.';

/** Normalised flight-search request parameters. */
export interface FlightParams {
  origin: string;
  destination: string;
  departureDate: string;
  returnDate: string;
  tripType: string;
  passengers: number;
  cabinClass: string;
  flexibleDays: number;
  maxBudget: number | null;
  currency: string;
  preferredAirlines: string[];
  avoidAirlines: string[];
  maxStops: string;
  priorityMode: string;
}

/* "Best time to book" engine. Sweet-spot windows are derived from widely-cited
 * fare studies (Google Flights / Hopper / CheapAir): the cheapest fares cluster
 * a few weeks out for short/domestic trips and a few months out for long-haul
 * international ones. The AI's live price read is layered on in mergeBookingAdvice. */
const BOOKING_WINDOWS: Record<
  string,
  { tooEarly: number; sweetStart: number; sweetEnd: number; late: number; lastMin: number }
> = {
  domestic: { tooEarly: 120, sweetStart: 21, sweetEnd: 60, late: 14, lastMin: 3 },
  international: { tooEarly: 300, sweetStart: 60, sweetEnd: 150, late: 30, lastMin: 7 },
};

/* Northern-hemisphere peak travel months by rough demand. Fallback hint only;
 * the AI provides the route-specific seasonality note. */
const PEAK_MONTHS = [6, 7, 8, 12]; // Jun, Jul, Aug, Dec
const SHOULDER_MONTHS = [4, 5, 9, 10]; // Apr, May, Sep, Oct

/* IATA code → country, mirroring the airport DB used by the SkyFare UI. Lets the
 * timing engine tell domestic from international trips. */
const AIRPORT_COUNTRY: Record<string, string> = {
  JFK: "USA", LAX: "USA", ORD: "USA", ATL: "USA", DFW: "USA", SFO: "USA", MIA: "USA", SEA: "USA", BOS: "USA", EWR: "USA",
  IAD: "USA", DEN: "USA", LAS: "USA", MCO: "USA", HNL: "USA", PHX: "USA", IAH: "USA", MSP: "USA", DTW: "USA", PHL: "USA",
  LHR: "UK", LGW: "UK", STN: "UK", MAN: "UK", EDI: "UK", CDG: "France", ORY: "France", NCE: "France",
  FRA: "Germany", MUC: "Germany", BER: "Germany", AMS: "Netherlands", MAD: "Spain", BCN: "Spain",
  FCO: "Italy", MXP: "Italy", VCE: "Italy", ZRH: "Switzerland", GVA: "Switzerland", VIE: "Austria",
  CPH: "Denmark", OSL: "Norway", ARN: "Sweden", HEL: "Finland", IST: "Turkey", SAW: "Turkey", ATH: "Greece",
  LIS: "Portugal", DUB: "Ireland", BRU: "Belgium", WAW: "Poland", PRG: "Czech Republic", BUD: "Hungary",
  DXB: "UAE", AUH: "UAE", DOH: "Qatar", RUH: "Saudi Arabia", JED: "Saudi Arabia", BAH: "Bahrain", MCT: "Oman",
  AMM: "Jordan", TLV: "Israel", CAI: "Egypt", NRT: "Japan", HND: "Japan", KIX: "Japan", ICN: "South Korea",
  GMP: "South Korea", PEK: "China", PKX: "China", PVG: "China", CAN: "China", HKG: "Hong Kong", TPE: "Taiwan",
  SIN: "Singapore", KUL: "Malaysia", BKK: "Thailand", DMK: "Thailand", CGK: "Indonesia", DPS: "Indonesia",
  MNL: "Philippines", SGN: "Vietnam", HAN: "Vietnam", DEL: "India", BOM: "India", BLR: "India", MAA: "India",
  HYD: "India", CCU: "India", CMB: "Sri Lanka", DAC: "Bangladesh", KTM: "Nepal", SYD: "Australia", MEL: "Australia",
  BNE: "Australia", PER: "Australia", AKL: "New Zealand", YYZ: "Canada", YVR: "Canada", YUL: "Canada",
  MEX: "Mexico", CUN: "Mexico", GRU: "Brazil", GIG: "Brazil", EZE: "Argentina", SCL: "Chile", BOG: "Colombia",
  LIM: "Peru", JNB: "South Africa", CPT: "South Africa", NBO: "Kenya", ADD: "Ethiopia", CMN: "Morocco",
  LOS: "Nigeria", ACC: "Ghana", MRU: "Mauritius",
};

const CITY_COUNTRY: Record<string, string> = {
  "new york": "USA", "los angeles": "USA", chicago: "USA", atlanta: "USA", dallas: "USA", "san francisco": "USA",
  miami: "USA", seattle: "USA", boston: "USA", newark: "USA", washington: "USA", denver: "USA",
  "las vegas": "USA", orlando: "USA", honolulu: "USA", phoenix: "USA", houston: "USA",
  london: "UK", manchester: "UK", edinburgh: "UK", paris: "France", nice: "France", frankfurt: "Germany",
  munich: "Germany", berlin: "Germany", amsterdam: "Netherlands", madrid: "Spain", barcelona: "Spain",
  rome: "Italy", milan: "Italy", venice: "Italy", zurich: "Switzerland", geneva: "Switzerland",
  vienna: "Austria", copenhagen: "Denmark", oslo: "Norway", stockholm: "Sweden", helsinki: "Finland",
  istanbul: "Turkey", athens: "Greece", lisbon: "Portugal", dublin: "Ireland", brussels: "Belgium",
  warsaw: "Poland", prague: "Czech Republic", budapest: "Hungary", dubai: "UAE", "abu dhabi": "UAE", doha: "Qatar",
  riyadh: "Saudi Arabia", jeddah: "Saudi Arabia", muscat: "Oman", amman: "Jordan", "tel aviv": "Israel", cairo: "Egypt",
  tokyo: "Japan", osaka: "Japan", seoul: "South Korea", beijing: "China", shanghai: "China", guangzhou: "China",
  "hong kong": "Hong Kong", taipei: "Taiwan", singapore: "Singapore", "kuala lumpur": "Malaysia", bangkok: "Thailand",
  jakarta: "Indonesia", bali: "Indonesia", manila: "Philippines", "ho chi minh city": "Vietnam", hanoi: "Vietnam",
  "new delhi": "India", delhi: "India", mumbai: "India", bangalore: "India", chennai: "India", hyderabad: "India",
  kolkata: "India", colombo: "Sri Lanka", dhaka: "Bangladesh", kathmandu: "Nepal", sydney: "Australia",
  melbourne: "Australia", brisbane: "Australia", perth: "Australia", auckland: "New Zealand", toronto: "Canada",
  vancouver: "Canada", montreal: "Canada", "mexico city": "Mexico", cancun: "Mexico", "sao paulo": "Brazil",
  "rio de janeiro": "Brazil", "buenos aires": "Argentina", santiago: "Chile", bogota: "Colombia", lima: "Peru",
  johannesburg: "South Africa", "cape town": "South Africa", nairobi: "Kenya", "addis ababa": "Ethiopia",
  casablanca: "Morocco", lagos: "Nigeria", accra: "Ghana", mauritius: "Mauritius",
};

/** Resolve a free-text place ("Tokyo (NRT)", "SIN", "singapore") to a country. */
export function countryForPlace(place: unknown): string | null {
  if (!place) return null;
  const s = String(place).trim();
  const paren = s.match(/\(([A-Za-z]{3})\)/);
  if (paren) {
    const c = AIRPORT_COUNTRY[paren[1]!.toUpperCase()];
    if (c) return c;
  }
  const bare = s.match(/\b([A-Za-z]{3})\b/);
  if (bare) {
    const c = AIRPORT_COUNTRY[bare[1]!.toUpperCase()];
    if (c) return c;
  }
  const lower = s.toLowerCase();
  for (const city in CITY_COUNTRY) {
    if (lower.indexOf(city) >= 0) return CITY_COUNTRY[city]!;
  }
  return null;
}

function daysBetween(fromISO: string, toISO: string): number | null {
  const a = Date.parse(fromISO + "T00:00:00Z");
  const b = Date.parse(toISO + "T00:00:00Z");
  if (isNaN(a) || isNaN(b)) return null;
  return Math.round((b - a) / 86400000);
}

/* Infer whether the trip crosses a border; defaults to international (the more
 * conservative, longer window) when either endpoint's country is unknown. */
function inferScope(origin: string, destination: string): string {
  const oc = countryForPlace(origin);
  const dc = countryForPlace(destination);
  if (oc && dc) return oc === dc ? "domestic" : "international";
  return "international";
}

export function computeBookingTiming(p: FlightParams, todayISO?: string): any {
  const today = todayISO || new Date().toISOString().split("T")[0]!;
  const daysOut = p.departureDate ? daysBetween(today, p.departureDate) : null;
  const scope = inferScope(p.origin, p.destination);
  const w = BOOKING_WINDOWS[scope]!;

  let recommendation: string;
  let urgency: string;
  let headline: string;
  let window: string;

  if (daysOut === null) {
    recommendation = "monitor";
    urgency = "info";
    headline = "Add a departure date for booking-timing advice";
    window = "";
  } else if (daysOut < 0) {
    recommendation = "book_now";
    urgency = "high";
    headline = "This date is in the past — pick an upcoming date";
    window = "";
  } else if (daysOut <= w.lastMin) {
    recommendation = "book_now";
    urgency = "high";
    headline = "Book now — last-minute fares rarely fall";
    window = "Book today";
  } else if (daysOut <= w.late) {
    recommendation = "book_soon";
    urgency = "elevated";
    headline = "Book soon — prices usually climb in the final weeks";
    window = "Within the next few days";
  } else if (daysOut < w.sweetStart) {
    recommendation = "book_soon";
    urgency = "elevated";
    headline = "Good to book — you are approaching the cheapest window";
    window = "Within 1–2 weeks";
  } else if (daysOut <= w.sweetEnd) {
    recommendation = "book_now";
    urgency = "good";
    headline = "You're in the sweet spot — a strong time to book";
    window = "Now through the next couple of weeks";
  } else if (daysOut <= w.tooEarly) {
    recommendation = "monitor";
    urgency = "info";
    headline = "Plenty of time — track the price and book in the sweet spot";
    window = "Aim for " + w.sweetStart + "–" + w.sweetEnd + " days before departure";
  } else {
    recommendation = "wait";
    urgency = "info";
    headline = "Very early — fares are often not optimized yet";
    window = "Revisit around " + w.sweetEnd + " days before departure";
  }

  const month = p.departureDate ? parseInt(p.departureDate.slice(5, 7), 10) || 0 : 0;
  const season =
    PEAK_MONTHS.indexOf(month) >= 0
      ? "peak"
      : SHOULDER_MONTHS.indexOf(month) >= 0
      ? "shoulder"
      : month
      ? "off-peak"
      : "unknown";

  return {
    days_until_departure: daysOut,
    route_scope: scope,
    season,
    recommendation, // book_now | book_soon | wait | monitor
    urgency, // good | elevated | high | info
    headline,
    best_booking_window: window,
    sweet_spot_days: { start: w.sweetStart, end: w.sweetEnd },
  };
}

const REC_HEADLINE: Record<string, string> = {
  book_now: "Book now",
  book_soon: "Book soon",
  wait: "Consider waiting",
  monitor: "Track the price",
};

function capitalize(s: string): string {
  return s ? s.charAt(0).toUpperCase() + s.slice(1) : s;
}

/* Merge the deterministic timing skeleton with the AI's live price read. The AI
 * is authoritative on price level / trend / seasonality; our engine is
 * authoritative on advance-purchase position. */
export function mergeBookingAdvice(timing: any, aiAdvice: any): any {
  const a = aiAdvice && typeof aiAdvice === "object" ? aiAdvice : {};
  const priceLevel =
    ["low", "typical", "high"].indexOf(a.price_assessment) >= 0
      ? a.price_assessment
      : "unknown";
  const trend =
    ["rising", "stable", "falling"].indexOf(a.expected_trend) >= 0
      ? a.expected_trend
      : "unknown";
  const confidence =
    ["high", "medium", "low"].indexOf(a.confidence) >= 0 ? a.confidence : "low";

  let rec = timing.recommendation;
  let urgency = timing.urgency;
  const hasDate =
    typeof timing.days_until_departure === "number" &&
    timing.days_until_departure >= 0;

  // Let a strong live price signal override the calendar-only verdict (only
  // when we have a valid future departure date to reason about).
  if (hasDate && priceLevel === "low" && (rec === "monitor" || rec === "wait")) {
    rec = "book_now";
    urgency = "good";
  } else if (
    hasDate &&
    priceLevel === "high" &&
    trend === "falling" &&
    rec === "book_now" &&
    timing.urgency !== "high"
  ) {
    rec = "wait";
    urgency = "info";
  } else if (hasDate && trend === "rising" && rec === "monitor") {
    rec = "book_soon";
    urgency = "elevated";
  }

  const summaryBits: string[] = [];
  if (
    typeof timing.days_until_departure === "number" &&
    timing.days_until_departure >= 0
  ) {
    summaryBits.push(
      timing.days_until_departure + " days out (" + timing.route_scope + " route)"
    );
  }
  if (priceLevel !== "unknown") summaryBits.push("current fares look " + priceLevel);
  if (trend !== "unknown") summaryBits.push("prices trending " + trend);

  const summary =
    a.summary && typeof a.summary === "string"
      ? a.summary.slice(0, 300)
      : timing.headline +
        (summaryBits.length ? ". " + capitalize(summaryBits.join(", ")) + "." : ".");

  return {
    recommendation: rec, // book_now | book_soon | wait | monitor
    urgency, // good | elevated | high | info
    headline: REC_HEADLINE[rec] || timing.headline,
    summary,
    price_assessment: priceLevel, // low | typical | high | unknown
    expected_trend: trend, // rising | stable | falling | unknown
    confidence,
    days_until_departure: timing.days_until_departure,
    route_scope: timing.route_scope,
    season: timing.season,
    best_booking_window:
      a.best_booking_window && typeof a.best_booking_window === "string"
        ? a.best_booking_window.slice(0, 120)
        : timing.best_booking_window,
    seasonality_note:
      a.seasonality_note && typeof a.seasonality_note === "string"
        ? a.seasonality_note.slice(0, 240)
        : "",
    cheaper_alternative_dates:
      a.cheaper_alternative_dates && typeof a.cheaper_alternative_dates === "string"
        ? a.cheaper_alternative_dates.slice(0, 240)
        : "",
    sweet_spot_days: timing.sweet_spot_days,
  };
}

export function buildFlightPrompt(p: FlightParams): string {
  const preferred =
    p.preferredAirlines && p.preferredAirlines.length > 0
      ? p.preferredAirlines.join(", ")
      : "No preference";
  const avoid =
    p.avoidAirlines && p.avoidAirlines.length > 0
      ? p.avoidAirlines.join(", ")
      : "None";
  const budget = p.maxBudget ? p.maxBudget + " " + p.currency : "No limit";
  return (
    "Find current flight options using the following search criteria:\n\n" +
    "Origin: " + p.origin + "\nDestination: " + p.destination + "\n" +
    "Departure date: " + p.departureDate + "\nReturn date: " + (p.returnDate || "N/A (one-way)") + "\n" +
    "Trip type: " + p.tripType + "\nPassengers: " + p.passengers + "\n" +
    "Cabin class: " + p.cabinClass + "\nFlexible date range: +/- " + (p.flexibleDays || 0) + " days\n" +
    "Maximum budget: " + budget + "\nPreferred airlines: " + preferred + "\n" +
    "Avoid airlines: " + avoid + "\nMaximum stops: " + p.maxStops + "\n" +
    "Priority mode: " + p.priorityMode + "\n\n" +
    "In addition to listing flights, give booking-timing guidance for THIS route and date. " +
    "Assess whether current fares are low/typical/high versus the usual price for this route and season, " +
    "whether prices are likely rising/stable/falling between now and departure, the best window to book, " +
    "a short seasonality note, and (if helpful) nearby cheaper dates. Base this on real fare-trend and " +
    "seasonality information from your web search; if unsure, say so and use a lower confidence.\n\n" +
    "Return the result in this exact JSON structure (no markdown, no code fences, just raw JSON):\n\n" +
    '{"search_summary":{"origin":"","destination":"","departure_date":"","return_date":"","trip_type":"","passengers":0,"cabin_class":"","currency":"","budget":0,"freshness_note":"","result_confidence":"high | medium | low"},"booking_advice":{"price_assessment":"low | typical | high","expected_trend":"rising | stable | falling","confidence":"high | medium | low","best_booking_window":"","seasonality_note":"","cheaper_alternative_dates":"","summary":""},"recommendation":{"best_overall_flight_id":"","cheapest_flight_id":"","fastest_flight_id":"","best_under_budget_flight_id":"","explanation":""},"flights":[{"id":"","airline":"","flight_numbers":[],"provider":"","price":0,"currency":"","is_under_budget":true,"departure_airport":"","arrival_airport":"","departure_time":"","arrival_time":"","total_duration_minutes":0,"stops":0,"layovers":[{"airport":"","duration_minutes":0}],"co2_kg":0,"fare_brand":"","carry_on_included":true,"checked_bags_included":0,"refundable":false,"self_transfer":false,"booking_url":"","source_url":"","source_name":"","confidence":"high | medium | low","notes":"","score":0,"badges":[]}],"warnings":[]}'
  );
}

/** Pull the JSON blob out of a model response that may wrap it in prose/fences. */
export function extractFlightJson(text: string): string {
  const fence = text.match(/```(?:json)?\s*\n?([\s\S]*?)```/);
  if (fence) return fence[1]!.trim();
  const start = text.indexOf("{");
  const end = text.lastIndexOf("}");
  if (start !== -1 && end > start) return text.substring(start, end + 1);
  return text.trim();
}

/** Coerce a possibly-string numeric value; fall back to `fallback` if not finite. */
export function flightNum(v: unknown, fallback: number): number {
  if (typeof v === "number") return Number.isFinite(v) ? v : fallback;
  if (typeof v === "string") {
    const n = parseFloat(v.replace(/[^0-9.\-]/g, ""));
    return Number.isFinite(n) ? n : fallback;
  }
  return fallback;
}

/** Like flightNum but returns null (not a default) when there is no usable value. */
export function flightNumOrNull(v: unknown): number | null {
  if (typeof v === "number") return Number.isFinite(v) ? v : null;
  if (typeof v === "string") {
    const n = parseFloat(v.replace(/[^0-9.\-]/g, ""));
    return Number.isFinite(n) ? n : null;
  }
  return null;
}

/** Enforce the user's max-stops preference in case the model ignores it. */
export function enforceMaxStops(flights: any[], maxStops: string): any[] {
  if (maxStops === "nonstop") return flights.filter((f) => f.stops === 0);
  if (maxStops === "1") return flights.filter((f) => f.stops <= 1);
  return flights;
}

export function extractIataCode(str: unknown): string {
  const s = String(str || "").trim();
  const m = s.match(/\(([A-Za-z]{3})\)/);
  if (m) return m[1]!.toUpperCase();
  if (/^[A-Za-z]{3}$/.test(s)) return s.toUpperCase();
  return "";
}

function buildGoogleFlightsUrl(params: FlightParams, flight: any): string {
  const origin =
    extractIataCode(flight && flight.departure_airport) ||
    extractIataCode(params.origin);
  const dest =
    extractIataCode(flight && flight.arrival_airport) ||
    extractIataCode(params.destination);
  const dep = String((flight && flight.departure_date) || params.departureDate || "").slice(0, 10);
  const ret =
    params.tripType === "round-trip" ? String(params.returnDate || "").slice(0, 10) : "";
  if (!origin || !dest || !dep) return "";
  let q = "Flights from " + origin + " to " + dest + " on " + dep;
  if (ret) q += " through " + ret;
  const pax = parseInt(String(params.passengers), 10) || 1;
  if (pax > 1) q += " for " + pax + " adults";
  return (
    "https://www.google.com/travel/flights?hl=en&curr=" +
    encodeURIComponent(params.currency || "USD") +
    "&q=" +
    encodeURIComponent(q)
  );
}

function buildKayakUrl(params: FlightParams, flight: any): string {
  const origin =
    extractIataCode(flight && flight.departure_airport) ||
    extractIataCode(params.origin);
  const dest =
    extractIataCode(flight && flight.arrival_airport) ||
    extractIataCode(params.destination);
  const dep = String((flight && flight.departure_date) || params.departureDate || "").slice(0, 10);
  const ret =
    params.tripType === "round-trip" ? String(params.returnDate || "").slice(0, 10) : "";
  const pax = Math.max(1, parseInt(String(params.passengers), 10) || 1);
  if (!origin || !dest || !dep) return "";
  let url = "https://www.kayak.com/flights/" + origin + "-" + dest + "/" + dep;
  if (ret) url += "/" + ret;
  return url + "?sort=bestflight_a&adults=" + pax;
}

/** Ensure every flight has a bookable redirect URL for SkyFare's Book CTAs. */
export function enrichBookingUrls(response: any, params: FlightParams): any {
  if (!response || !Array.isArray(response.flights)) return response;
  response.flights = response.flights.map((f: any) => {
    let booking = safeUrl(f.booking_url) || "";
    const source = safeUrl(f.source_url) || "";
    if (!booking) {
      booking = buildGoogleFlightsUrl(params, f) || buildKayakUrl(params, f) || source;
    }
    if (!f.provider && booking) {
      try {
        const host = new URL(booking).hostname.replace(/^www\./, "");
        if (/google\./i.test(host)) f.provider = "Google Flights";
        else if (/kayak\./i.test(host)) f.provider = "Kayak";
        else if (/skyscanner\./i.test(host)) f.provider = "Skyscanner";
        else f.provider = host;
      } catch {
        /* ignore */
      }
    }
    f.booking_url = booking;
    f.source_url = source;
    f.book_links = {
      primary: booking,
      google: buildGoogleFlightsUrl(params, f),
      kayak: buildKayakUrl(params, f),
    };
    return f;
  });
  return response;
}

export function normalizeFlightResponse(raw: any, params: FlightParams): any {
  if (!raw.search_summary) {
    raw.search_summary = {
      origin: params.origin,
      destination: params.destination,
      departure_date: params.departureDate,
      return_date: params.returnDate || "",
      trip_type: params.tripType,
      passengers: params.passengers,
      cabin_class: params.cabinClass,
      currency: params.currency,
      budget: params.maxBudget || 0,
      freshness_note: "Data freshness unknown",
      result_confidence: "low",
    };
  }
  if (!raw.recommendation) {
    raw.recommendation = {
      best_overall_flight_id: "",
      cheapest_flight_id: "",
      fastest_flight_id: "",
      best_under_budget_flight_id: "",
      explanation: "",
    };
  }
  if (!Array.isArray(raw.flights)) raw.flights = [];
  if (!Array.isArray(raw.warnings)) raw.warnings = [];
  raw.flights = raw.flights.map((f: any, i: number) => {
    // Models frequently return numbers as strings ("450"); coerce safely so
    // scoring, deal quality, budget checks and filters don't silently break.
    const price = flightNum(f.price, 0);
    const checkedBags = flightNumOrNull(f.checked_bags_included);
    const co2 = flightNumOrNull(f.co2_kg);
    return {
      id: f.id || "flight_" + (i + 1),
      airline: f.airline || "Unknown Airline",
      flight_numbers: Array.isArray(f.flight_numbers) ? f.flight_numbers : [],
      provider: f.provider || "",
      price,
      currency: f.currency || params.currency,
      is_under_budget:
        typeof f.is_under_budget === "boolean"
          ? f.is_under_budget
          : params.maxBudget
          ? price <= params.maxBudget
          : true,
      departure_airport: f.departure_airport || params.origin,
      arrival_airport: f.arrival_airport || params.destination,
      departure_time: f.departure_time || "",
      arrival_time: f.arrival_time || "",
      total_duration_minutes: flightNum(f.total_duration_minutes, 0),
      stops: flightNum(f.stops, 0),
      layovers: Array.isArray(f.layovers)
        ? f.layovers.map((l: any) => ({
            airport: (l && l.airport) || "",
            duration_minutes: flightNum(l && l.duration_minutes, 0),
          }))
        : [],
      co2_kg: co2 !== null && co2 > 0 ? Math.round(co2) : null,
      fare_brand: typeof f.fare_brand === "string" ? f.fare_brand.slice(0, 60) : "",
      carry_on_included:
        typeof f.carry_on_included === "boolean" ? f.carry_on_included : null,
      checked_bags_included:
        checkedBags !== null ? Math.max(0, Math.round(checkedBags)) : null,
      refundable: typeof f.refundable === "boolean" ? f.refundable : null,
      self_transfer: f.self_transfer === true,
      emissions_level: "unknown",
      deal_quality: "unknown",
      day_offset: 0,
      overnight: false,
      booking_url: safeUrl(f.booking_url) || "",
      source_url: safeUrl(f.source_url) || "",
      source_name: f.source_name || "",
      confidence: ["high", "medium", "low"].indexOf(f.confidence) >= 0 ? f.confidence : "low",
      notes: f.notes || "",
      score: flightNum(f.score, 0),
      badges: Array.isArray(f.badges) ? f.badges : [],
    };
  });
  return raw;
}

function flightNorm100(val: number, min: number, max: number): number {
  if (max === min) return 100;
  return Math.round(((max - val) / (max - min)) * 100);
}

function flightMedian(arr: number[]): number {
  if (!arr.length) return 0;
  const s = arr.slice().sort((a, b) => a - b);
  const mid = Math.floor(s.length / 2);
  return s.length % 2 ? s[mid]! : (s[mid - 1]! + s[mid]!) / 2;
}

/* "HH:MM" (24h) or "8:00 AM" -> minutes since midnight, or null. */
function flightTimeToMinutes(t: unknown): number | null {
  if (!t || typeof t !== "string") return null;
  const ampm = t.match(/(\d{1,2}):(\d{2})\s*([AaPp][Mm])/);
  if (ampm) {
    let h = parseInt(ampm[1]!, 10) % 12;
    if (/[Pp]/.test(ampm[3]!)) h += 12;
    return h * 60 + parseInt(ampm[2]!, 10);
  }
  const m = t.match(/(\d{1,2}):(\d{2})/);
  if (!m) return null;
  return (parseInt(m[1]!, 10) % 24) * 60 + parseInt(m[2]!, 10);
}

/* Derive arrival day offset (+1, +2…) and red-eye flag from local clock times
 * and total duration, the way Google Flights shows "11:05 +1". */
function flightDayShape(f: any): { day_offset: number; overnight: boolean } {
  const dep = flightTimeToMinutes(f.departure_time);
  const arr = flightTimeToMinutes(f.arrival_time);
  const dur = f.total_duration_minutes;
  let offset = 0;
  if (dep !== null && dur > 0) {
    offset = Math.floor((dep + dur) / 1440);
  } else if (dep !== null && arr !== null && arr < dep) {
    offset = 1;
  }
  // Red-eye: departs late evening / overnight and lands the next day, or a
  // long-haul that flies through the night.
  const overnight = offset >= 1 && dep !== null && (dep >= 18 * 60 || dep <= 5 * 60);
  return { day_offset: offset, overnight };
}

/* SkyFare Value Score: Price 40%, Duration 25%, Stops/layovers 15%,
 * Departure/arrival timing 10%, Airline quality 10%. Emissions remain
 * informational badges, not part of the core score. */
function flightTimingScore(f: any): number {
  const dep = flightTimeToMinutes(f.departure_time);
  const arr = flightTimeToMinutes(f.arrival_time);
  let score = 70;
  if (dep !== null) {
    // Prefer mid-morning / afternoon departures; penalise red-eyes and very late.
    if (dep >= 8 * 60 && dep < 11 * 60) score = 100;
    else if (dep >= 11 * 60 && dep < 18 * 60) score = 90;
    else if (dep >= 6 * 60 && dep < 8 * 60) score = 75;
    else if (dep >= 18 * 60 && dep < 21 * 60) score = 65;
    else if (dep >= 21 * 60 || dep < 5 * 60) score = 30;
    else score = 50;
  }
  if (arr !== null) {
    if (arr >= 22 * 60 || arr < 6 * 60) score = Math.min(score, 45);
    else if (arr >= 20 * 60) score = Math.min(score, 65);
  }
  if (f.overnight) score = Math.min(score, 35);
  if ((f.day_offset || 0) >= 2) score = Math.min(score, 40);
  return score;
}

function flightLayoverQuality(f: any): number {
  let score = 100;
  if (!f.stops) return score;
  score -= Math.min(60, f.stops * 25);
  const layovers = Array.isArray(f.layovers) ? f.layovers : [];
  layovers.forEach((l: any) => {
    const mins = typeof l.duration_minutes === "number" ? l.duration_minutes : 0;
    if (mins > 0 && mins < 50) score -= 25; // tight connection risk
    else if (mins >= 240) score -= 20; // long layover fatigue
    else if (mins >= 180) score -= 10;
  });
  if (f.self_transfer) score -= 30;
  return Math.max(0, Math.min(100, score));
}

function flightAirlineQuality(f: any): number {
  const name = String(f.airline || "").toLowerCase();
  const brand = String(f.fare_brand || "").toLowerCase();
  // Heuristic tiers until we have a real reliability feed.
  const premium = /singapore|qatar|emirates|ana |all nippon|japan airlines|cathay|swiss|lufthansa|british airways|qantas|air new zealand|delta|united|american|air france|klm|finnair|turkish|eva air|korean air|virgin/;
  const solid = /indigo|vistara|air india|scoot|jetstar|peach|airasia|vietjet|thai airways|malaysia airlines|garuda|philippine|etihad|oman air|saudia|iberia|tap |alitalia|ita airways|aeromexico|copa|latam|avianca|alaska|westjet|porter|ryanair|easyjet|wizz/;
  const ulcc = /spirit|frontier|allegiant|cape air|nok air|lion air|cebu pacific|spicejet|go first|zipair/;
  let score = 55;
  if (premium.test(name)) score = 88;
  else if (solid.test(name)) score = 72;
  else if (ulcc.test(name)) score = 42;
  if (/basic|light|saver|blue basic|basic economy/.test(brand)) score = Math.max(25, score - 18);
  if (f.refundable === true) score = Math.min(100, score + 5);
  if (f.carry_on_included === false) score = Math.max(20, score - 8);
  if (f.confidence === "high") score = Math.min(100, score + 4);
  else if (f.confidence === "low") score = Math.max(20, score - 8);
  return score;
}

export function scoreFlights(
  response: any,
  priorityMode: string,
  maxBudget: number | null
): any {
  const flights = response.flights;
  if (flights.length === 0) return response;
  const prices = flights.map((f: any) => f.price).filter((p: number) => p > 0);
  const durs = flights
    .map((f: any) => f.total_duration_minutes)
    .filter((d: number) => d > 0);
  const stopsArr = flights.map((f: any) => f.stops);
  const co2s = flights
    .map((f: any) => f.co2_kg)
    .filter((c: any) => typeof c === "number" && c > 0);
  // Guard against empty arrays: Math.min/max of [] yield ±Infinity, which
  // propagate NaN into every score. Fall back to neutral bounds.
  const minP = prices.length ? Math.min.apply(null, prices) : 0;
  const maxP = prices.length ? Math.max.apply(null, prices) : 0;
  const minD = durs.length ? Math.min.apply(null, durs) : 0;
  const maxD = durs.length ? Math.max.apply(null, durs) : 0;
  const minS = stopsArr.length ? Math.min.apply(null, stopsArr) : 0;
  const maxS = stopsArr.length ? Math.max.apply(null, stopsArr) : 0;
  const minC = co2s.length ? Math.min.apply(null, co2s) : 0;
  const maxC = co2s.length ? Math.max.apply(null, co2s) : 0;
  const medP = flightMedian(prices);
  const medC = flightMedian(co2s);
  const hasEmissions = co2s.length >= 2 && maxC > minC;

  // Value Score weights (tunable).
  const W = { price: 0.4, dur: 0.25, stops: 0.15, timing: 0.1, airline: 0.1 };

  flights.forEach((f: any) => {
    const shape = flightDayShape(f);
    f.day_offset = shape.day_offset;
    f.overnight = shape.overnight;

    let ps = f.price > 0 ? flightNorm100(f.price, minP, maxP) : 50;
    const ds =
      f.total_duration_minutes > 0
        ? flightNorm100(f.total_duration_minutes, minD, maxD)
        : 50;
    // Blend stop count with layover quality ("stops + layover quality").
    const stopCountScore = flightNorm100(f.stops, minS, maxS);
    const layoverScore = flightLayoverQuality(f);
    const ss = Math.round(stopCountScore * 0.55 + layoverScore * 0.45);
    const ts = flightTimingScore(f);
    const as = flightAirlineQuality(f);

    // Soft budget nudge: keep under-budget options visible without rewriting the formula.
    if (maxBudget && maxBudget > 0 && f.price > 0) {
      if (f.price > maxBudget) ps = Math.max(0, ps - 15);
      else ps = Math.min(100, ps + 5);
    }

    f.score = Math.round(
      ps * W.price + ds * W.dur + ss * W.stops + ts * W.timing + as * W.airline
    );
    f.timing_score = ts;
    f.value_breakdown = { price: ps, duration: ds, stops: ss, timing: ts, airline: as };
    f.is_under_budget = maxBudget ? f.price <= maxBudget : true;

    // Emissions level relative to the result set's median.
    if (typeof f.co2_kg === "number" && f.co2_kg > 0 && medC > 0) {
      f.emissions_level =
        f.co2_kg <= medC * 0.88 ? "low" : f.co2_kg >= medC * 1.12 ? "high" : "typical";
    } else {
      f.emissions_level = "unknown";
    }

    // Deal quality vs the median fare (Kayak/Hopper-style read).
    if (f.price > 0 && medP > 0) {
      f.deal_quality =
        f.price <= medP * 0.82
          ? "great"
          : f.price <= medP * 0.96
          ? "good"
          : f.price <= medP * 1.18
          ? "typical"
          : "high";
    } else {
      f.deal_quality = "unknown";
    }

    f.badges = [];
  });
  // Only consider flights with a usable value; a naive reduce could "walk off"
  // to the last flight when no flight had a valid price/duration.
  const pricedFlights = flights.filter((f: any) => f.price > 0);
  const timedFlights = flights.filter((f: any) => f.total_duration_minutes > 0);
  const cheapest = (pricedFlights.length ? pricedFlights : flights).reduce(
    (a: any, b: any) => (a.price <= b.price ? a : b)
  );
  const fastest = (timedFlights.length ? timedFlights : flights).reduce((a: any, b: any) =>
    a.total_duration_minutes <= b.total_duration_minutes ? a : b
  );
  const best = flights.reduce((a: any, b: any) => (a.score >= b.score ? a : b));
  const bestTiming = flights.reduce((a: any, b: any) =>
    (a.timing_score || 0) >= (b.timing_score || 0) ? a : b
  );
  const greenest = co2s.length
    ? flights.reduce((a: any, b: any) => {
        const av = typeof a.co2_kg === "number" && a.co2_kg > 0 ? a.co2_kg : Infinity;
        const bv = typeof b.co2_kg === "number" && b.co2_kg > 0 ? b.co2_kg : Infinity;
        return av <= bv ? a : b;
      })
    : null;
  const underBudget = flights.filter((f: any) => f.is_under_budget);
  const bestUB =
    underBudget.length > 0
      ? underBudget.reduce((a: any, b: any) => (a.score >= b.score ? a : b))
      : null;
  function addFlightBadge(id: string, badge: string) {
    const f = flights.find((fl: any) => fl.id === id);
    if (f && f.badges.indexOf(badge) < 0) f.badges.push(badge);
  }
  addFlightBadge(cheapest.id, "Cheapest");
  addFlightBadge(fastest.id, "Fastest");
  addFlightBadge(best.id, "Best Overall");
  addFlightBadge(bestTiming.id, "Best Timing");
  if (bestUB) addFlightBadge(bestUB.id, "Under Budget");
  flights
    .filter((f: any) => f.stops === 0)
    .forEach((f: any) => addFlightBadge(f.id, "Nonstop"));
  // "Great Deal" for fares meaningfully below the median (only with a few quotes).
  if (flights.length >= 3) {
    flights
      .filter((f: any) => f.deal_quality === "great")
      .forEach((f: any) => addFlightBadge(f.id, "Great Deal"));
  }
  if (hasEmissions && greenest) addFlightBadge(greenest.id, "Low CO2");
  let sorted: any[];
  switch (priorityMode) {
    case "cheapest":
      sorted = flights.slice().sort((a: any, b: any) => a.price - b.price);
      break;
    case "fastest":
      sorted = flights
        .slice()
        .sort((a: any, b: any) => a.total_duration_minutes - b.total_duration_minutes);
      break;
    case "best_under_budget":
      sorted = underBudget
        .slice()
        .sort((a: any, b: any) => b.score - a.score)
        .concat(
          flights
            .filter((f: any) => !f.is_under_budget)
            .sort((a: any, b: any) => b.score - a.score)
        );
      break;
    default:
      sorted = flights.slice().sort((a: any, b: any) => b.score - a.score);
  }
  // Price-insights summary (Google-Flights-style low / median / high range).
  response.price_insights = {
    currency: flights[0] ? flights[0].currency : "",
    low: prices.length ? Math.round(minP) : 0,
    median: prices.length ? Math.round(medP) : 0,
    high: prices.length ? Math.round(maxP) : 0,
    cheapest_deal_quality: cheapest.deal_quality || "unknown",
    co2_low: co2s.length ? Math.round(minC) : 0,
    co2_median: co2s.length ? Math.round(medC) : 0,
    co2_high: co2s.length ? Math.round(maxC) : 0,
    greenest_flight_id: greenest ? greenest.id : "",
  };
  const rec = response.recommendation;
  rec.cheapest_flight_id = cheapest.id;
  rec.fastest_flight_id = fastest.id;
  rec.best_overall_flight_id = best.id;
  rec.best_timing_flight_id = bestTiming.id;
  rec.best_under_budget_flight_id = bestUB ? bestUB.id : "";
  if (!rec.explanation) {
    const parts = [
      best.airline +
        " at " +
        best.currency +
        " " +
        best.price +
        " ranks Best Overall (Value Score " +
        best.score +
        ").",
    ];
    if (cheapest.id !== best.id)
      parts.push(
        cheapest.airline + " is cheapest at " + cheapest.currency + " " + cheapest.price + "."
      );
    if (fastest.id !== best.id)
      parts.push(fastest.airline + " is fastest at " + fastest.total_duration_minutes + " min.");
    if (bestTiming.id !== best.id)
      parts.push(bestTiming.airline + " has the best departure/arrival timing.");
    rec.explanation = parts.join(" ");
  }
  response.flights = sorted;
  response.recommendation = rec;
  return response;
}

/* ───────────── Amadeus response mapping (pure) ───────────── */

/** Amadeus API host for the configured environment (no IO). */
export function amadeusHost(env: Env): string {
  const anyEnv = env as unknown as Record<string, string | undefined>;
  if (anyEnv.AMADEUS_BASE_URL) return anyEnv.AMADEUS_BASE_URL.replace(/\/$/, "");
  return anyEnv.AMADEUS_ENV === "production"
    ? "https://api.amadeus.com"
    : "https://test.api.amadeus.com";
}

export function mapAmadeusCabin(cabinClass: unknown): string {
  switch (String(cabinClass || "").toLowerCase()) {
    case "premium_economy":
      return "PREMIUM_ECONOMY";
    case "business":
      return "BUSINESS";
    case "first":
      return "FIRST";
    default:
      return "ECONOMY";
  }
}

function isoDurationToMinutes(str: unknown): number {
  if (!str || typeof str !== "string") return 0;
  const m = str.match(/P(?:(\d+)D)?T(?:(\d+)H)?(?:(\d+)M)?/);
  if (!m) return 0;
  return (
    parseInt(m[1] || "0", 10) * 1440 +
    parseInt(m[2] || "0", 10) * 60 +
    parseInt(m[3] || "0", 10)
  );
}

function atTime(at: unknown): string {
  return typeof at === "string" && at.length >= 16 ? at.slice(11, 16) : "";
}
function atDate(at: unknown): string {
  return typeof at === "string" && at.length >= 10 ? at.slice(0, 10) : "";
}
function minutesBetweenAt(a: unknown, b: unknown): number {
  const ta = Date.parse(String(a));
  const tb = Date.parse(String(b));
  if (isNaN(ta) || isNaN(tb)) return 0;
  return Math.max(0, Math.round((tb - ta) / 60000));
}

function titleCaseAirline(name: unknown): string {
  return String(name || "")
    .toLowerCase()
    .replace(/\b([a-z])/g, (_m0, c: string) => c.toUpperCase());
}

/* Map an Amadeus Flight Offers Search response into the SkyFare `raw` shape that
 * normalizeFlightResponse() / scoreFlights() already understand. */
export function amadeusOffersToRaw(
  json: any,
  p: FlightParams,
  depIata: string,
  arrIata: string
): any {
  const carriers = (json && json.dictionaries && json.dictionaries.carriers) || {};
  const offers = json && Array.isArray(json.data) ? json.data : [];
  const flights = offers
    .map((offer: any, i: number) => {
      const itin = (offer.itineraries && offer.itineraries[0]) || { segments: [] };
      const segs = itin.segments || [];
      if (!segs.length) return null;
      const first = segs[0];
      const last = segs[segs.length - 1];

      const layovers: any[] = [];
      for (let s = 0; s < segs.length - 1; s++) {
        layovers.push({
          airport: (segs[s].arrival && segs[s].arrival.iataCode) || "",
          duration_minutes: minutesBetweenAt(
            segs[s].arrival && segs[s].arrival.at,
            segs[s + 1].departure && segs[s + 1].departure.at
          ),
        });
      }

      // Per-traveller price (SkyFare price is per person; card multiplies for totals).
      const tp = (offer.travelerPricings && offer.travelerPricings[0]) || null;
      const perPerson =
        tp && tp.price && tp.price.total
          ? parseFloat(tp.price.total)
          : offer.price && offer.price.grandTotal
          ? parseFloat(offer.price.grandTotal) /
            Math.max(1, (offer.travelerPricings || []).length || 1)
          : 0;

      const fd =
        tp && tp.fareDetailsBySegment && tp.fareDetailsBySegment[0]
          ? tp.fareDetailsBySegment[0]
          : {};
      const checkedBags =
        fd.includedCheckedBags && typeof fd.includedCheckedBags.quantity === "number"
          ? fd.includedCheckedBags.quantity
          : null;
      const carryOn =
        fd.includedCabinBags && typeof fd.includedCabinBags.quantity === "number"
          ? fd.includedCabinBags.quantity > 0
          : null;

      let co2 = 0;
      ((tp && tp.fareDetailsBySegment) || []).forEach((seg: any) => {
        if (
          seg.co2Emissions &&
          seg.co2Emissions[0] &&
          typeof seg.co2Emissions[0].weight === "number"
        )
          co2 += seg.co2Emissions[0].weight;
      });

      const carrierCode =
        (offer.validatingAirlineCodes && offer.validatingAirlineCodes[0]) ||
        first.carrierCode ||
        "";
      const airline = carriers[carrierCode]
        ? titleCaseAirline(carriers[carrierCode])
        : carrierCode || "Airline";

      return {
        id: offer.id ? "amadeus_" + offer.id : "amadeus_" + (i + 1),
        airline,
        flight_numbers: segs
          .map((sg: any) => (sg.carrierCode || "") + (sg.number || ""))
          .filter(Boolean),
        provider: "Amadeus",
        price: Math.round(perPerson),
        currency: (offer.price && offer.price.currency) || p.currency,
        is_under_budget: p.maxBudget ? perPerson <= p.maxBudget : true,
        departure_airport: (first.departure && first.departure.iataCode) || depIata,
        arrival_airport: (last.arrival && last.arrival.iataCode) || arrIata,
        departure_date: atDate(first.departure && first.departure.at) || p.departureDate,
        departure_time: atTime(first.departure && first.departure.at),
        arrival_time: atTime(last.arrival && last.arrival.at),
        total_duration_minutes: isoDurationToMinutes(itin.duration),
        stops: Math.max(0, segs.length - 1),
        layovers,
        co2_kg: co2 > 0 ? Math.round(co2) : 0,
        fare_brand:
          fd.brandedFare || fd.fareBasis || (fd.cabin ? titleCaseAirline(fd.cabin) : ""),
        carry_on_included: carryOn,
        checked_bags_included: checkedBags,
        refundable: null,
        self_transfer: false,
        booking_url: "",
        source_url: "",
        source_name: "Amadeus",
        confidence: "high",
        notes: "",
        score: 0,
        badges: [],
      };
    })
    .filter(Boolean);

  return {
    search_summary: {
      origin: depIata,
      destination: arrIata,
      departure_date: p.departureDate,
      return_date: p.tripType === "round-trip" ? p.returnDate : "",
      trip_type: p.tripType,
      passengers: p.passengers,
      cabin_class: p.cabinClass,
      currency: p.currency,
      budget: p.maxBudget || 0,
      freshness_note:
        "Live fares from Amadeus, retrieved " +
        new Date().toISOString().slice(0, 16).replace("T", " ") +
        " UTC.",
      result_confidence: "high",
    },
    recommendation: {
      best_overall_flight_id: "",
      cheapest_flight_id: "",
      fastest_flight_id: "",
      best_under_budget_flight_id: "",
      explanation: "",
    },
    flights,
    warnings: [],
  };
}

/* ───────────── Flight inspiration (destination ideas) ───────────── */

export const INSPIRE_SYSTEM_PROMPT =
  "You are Summaverick planning flights for SkyFare. Given a natural-language trip request, suggest 3 concrete destinations the traveller can search right away. Return ONLY JSON — no markdown, no code fences, no prose outside JSON. Prefer real IATA codes. Suggest realistic departure/return ISO dates (YYYY-MM-DD) within the stated month/season or the next 90 days if unspecified. Rough fares should be typical economy round-trip totals including taxes when possible. Never invent fake airport codes.";

/** Build the inspire user prompt from the trip request. */
export function buildInspirePrompt(
  query: string,
  originHint: string,
  currency: string
): string {
  return (
    'Trip request: "' + query + '"\n' +
    (originHint ? "Preferred origin (if not in the request): " + originHint + "\n" : "") +
    "Currency for rough fares: " + currency + "\n" +
    "Return JSON with this exact shape:\n" +
    '{"suggestions":[{"destination":"Bali","destination_airport":"DPS","origin":"Singapore","origin_airport":"SIN","best_weeks":["mid September","late September"],"price_range":{"min":280,"max":450,"currency":"' +
    currency +
    '"},"why":"Warm beaches in shoulder season","trip_type":"round-trip","suggested_departure":"2026-09-12","suggested_return":"2026-09-19"}]}\n' +
    "Rules: exactly 3 suggestions; use real IATA codes; dates must be YYYY-MM-DD; keep \"why\" under 180 characters; if origin is unknown leave origin fields empty."
  );
}

/** Pull the JSON object out of an inspire model response, or null. */
export function extractInspireJson(text: unknown): any {
  if (!text || typeof text !== "string") return null;
  let cleaned = text.trim();
  const fence = cleaned.match(/```(?:json)?\s*([\s\S]*?)```/i);
  if (fence) cleaned = fence[1]!.trim();
  const start = cleaned.indexOf("{");
  const end = cleaned.lastIndexOf("}");
  if (start < 0 || end <= start) return null;
  try {
    return JSON.parse(cleaned.slice(start, end + 1));
  } catch {
    return null;
  }
}

const INSPIRE_MONTH_NAMES = [
  "january", "february", "march", "april", "may", "june",
  "july", "august", "september", "october", "november", "december",
];

/* The model is told to use YYYY-MM-DD, but sonar occasionally writes day-first
 * (e.g. treats "12 Sep" as month=12 day=09). Both orderings can be valid dates
 * (when day <= 12), so a shape-only regex isn't enough — cross-check against any
 * month mentioned in the suggestion text and reorder if that resolves it. */
function inspireMonthHint(text: unknown): number {
  if (!text) return 0;
  const t = String(text).toLowerCase();
  for (let i = 0; i < INSPIRE_MONTH_NAMES.length; i++) {
    if (t.indexOf(INSPIRE_MONTH_NAMES[i]!) !== -1) return i + 1;
  }
  return 0;
}
function inspirePad2(n: number): string {
  return (n < 10 ? "0" : "") + n;
}
function inspireIsValidYmd(y: number, m: number, d: number): boolean {
  if (m < 1 || m > 12 || d < 1 || d > 31) return false;
  const dt = new Date(Date.UTC(y, m - 1, d));
  return dt.getUTCFullYear() === y && dt.getUTCMonth() === m - 1 && dt.getUTCDate() === d;
}
function inspireFixDateOrder(iso: string, monthHint: number): string {
  const m = iso.match(/^(\d{4})-(\d{2})-(\d{2})$/);
  if (!m) return "";
  const y = parseInt(m[1]!, 10);
  const a = parseInt(m[2]!, 10);
  const b = parseInt(m[3]!, 10);
  const asIs = inspireIsValidYmd(y, a, b);
  const swapped = a !== b && inspireIsValidYmd(y, b, a);
  if (asIs && swapped) {
    // Ambiguous (both day <= 12): trust the month the suggestion text agrees with.
    if (monthHint && b === monthHint && a !== monthHint)
      return y + "-" + inspirePad2(b) + "-" + inspirePad2(a);
    return iso;
  }
  if (asIs) return iso;
  if (swapped) return y + "-" + inspirePad2(b) + "-" + inspirePad2(a);
  return "";
}

export function normalizeInspireSuggestion(
  raw: any,
  fallbackCurrency: string,
  queryHint?: string
): any {
  if (!raw || typeof raw !== "object") return null;
  const destCity = sanitize(String(raw.destination || raw.city || "")).slice(0, 80);
  let destCode = sanitize(String(raw.destination_airport || raw.iata || raw.airport || ""))
    .toUpperCase()
    .slice(0, 3);
  if (!destCity && !destCode) return null;
  if (destCode && !/^[A-Z]{3}$/.test(destCode)) destCode = "";
  const originCity = sanitize(String(raw.origin || raw.origin_hint || "")).slice(0, 80);
  let originCode = sanitize(String(raw.origin_airport || "")).toUpperCase().slice(0, 3);
  if (originCode && !/^[A-Z]{3}$/.test(originCode)) originCode = "";
  let weeks = Array.isArray(raw.best_weeks) ? raw.best_weeks : [];
  weeks = weeks
    .map((w: any) => sanitize(String(w)).slice(0, 40))
    .filter(Boolean)
    .slice(0, 4);
  const price = raw.price_range && typeof raw.price_range === "object" ? raw.price_range : {};
  const min = parseInt(price.min, 10);
  const max = parseInt(price.max, 10);
  const cur =
    sanitize(String(price.currency || fallbackCurrency || "USD")).toUpperCase().slice(0, 3) ||
    "USD";
  let dep = sanitize(String(raw.suggested_departure || raw.departure_date || "")).slice(0, 10);
  let ret = sanitize(String(raw.suggested_return || raw.return_date || "")).slice(0, 10);
  if (dep && !/^\d{4}-\d{2}-\d{2}$/.test(dep)) dep = "";
  if (ret && !/^\d{4}-\d{2}-\d{2}$/.test(ret)) ret = "";
  const monthHint = inspireMonthHint(
    weeks.join(" ") + " " + (raw.why || "") + " " + (queryHint || "")
  );
  if (dep) dep = inspireFixDateOrder(dep, monthHint);
  if (ret) ret = inspireFixDateOrder(ret, monthHint);
  if (dep && ret && ret < dep) {
    // Return landed before departure — most likely dep/ret still disagree on
    // day/month order between themselves. Re-derive ret from dep's month.
    const reordered = inspireFixDateOrder(ret, parseInt(dep.slice(5, 7), 10));
    ret = reordered && reordered >= dep ? reordered : "";
  }
  const destLabel = destCity
    ? destCode
      ? destCity + " (" + destCode + ")"
      : destCity
    : destCode;
  const originLabel = originCity
    ? originCode
      ? originCity + " (" + originCode + ")"
      : originCity
    : originCode || "";
  return {
    destination: destLabel,
    destination_city: destCity || destCode,
    destination_airport: destCode,
    origin: originLabel,
    origin_city: originCity,
    origin_airport: originCode,
    best_weeks: weeks,
    price_range: {
      min: Number.isFinite(min) ? min : null,
      max: Number.isFinite(max) ? max : null,
      currency: cur,
    },
    why: sanitize(String(raw.why || raw.reason || raw.notes || "")).slice(0, 220),
    trip_type: /one.?way/i.test(String(raw.trip_type || "")) ? "one-way" : "round-trip",
    suggested_departure: dep,
    suggested_return: ret,
  };
}

/** Parse the flight-search request body into normalised FlightParams. */
export function parseFlightParams(body: any): FlightParams {
  return {
    origin: sanitize(String(body.origin || "")),
    destination: sanitize(String(body.destination || "")),
    departureDate: sanitize(String(body.departureDate || "")),
    returnDate: sanitize(String(body.returnDate || "")),
    tripType: sanitize(String(body.tripType || "one-way")),
    passengers: parseInt(body.passengers, 10) || 1,
    cabinClass: sanitize(String(body.cabinClass || "economy")),
    flexibleDays: Math.min(Math.max(parseInt(body.flexibleDays, 10) || 0, 0), 7),
    maxBudget: body.maxBudget ? parseInt(body.maxBudget, 10) : null,
    currency: sanitize(String(body.currency || "USD")),
    preferredAirlines: Array.isArray(body.preferredAirlines)
      ? body.preferredAirlines.map((a: any) => sanitize(String(a))).filter(Boolean)
      : [],
    avoidAirlines: Array.isArray(body.avoidAirlines)
      ? body.avoidAirlines.map((a: any) => sanitize(String(a))).filter(Boolean)
      : [],
    maxStops: sanitize(String(body.maxStops || "2+")),
    priorityMode: sanitize(String(body.priorityMode || "best_balance")),
  };
}
