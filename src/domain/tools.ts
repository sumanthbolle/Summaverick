/**
 * Pure, IO-free logic for the trending widgets on /ask-summaverick. Ported from
 * the legacy Cloudflare Worker (api/worker.js). Everything here is a
 * deterministic transform, payload builder or lookup table — no fetch, cache or
 * DB access. The IO wrappers live in src/routes/tools.ts.
 */

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
