/** General-purpose chat for /ask-summaverick; the ServiceNow research API stays separate. */
import type { RouteDef, RouteMaker } from "../types";
import { json, readJson } from "../lib/json";
import { callPerplexity, perplexityConfig } from "../lib/perplexity";
import { askFallbackUrl, fetchFallback } from "../lib/fallback";
import { clientKey, rateLimit } from "../lib/ratelimit";
import { buildChatPayload, buildChatResult, sanitize } from "../domain/chat";

const UNAVAILABLE = "Could not get an answer right now. Please try again.";

export function chatRoutes(route: RouteMaker): RouteDef[] {
  return [route("POST", "/api/ask-summaverick", async (req, ctx) => {
    const body = await readJson<{ query?: unknown; context?: unknown; depth?: unknown }>(req);
    const query = sanitize(body?.query);
    if (query.length < 2 || query.length > 6000) {
      return json({ success: false, message: "Enter a question between 2 and 6,000 characters." }, { status: 400 });
    }

    const config = perplexityConfig(ctx.env);
    const fallbackUrl = config ? null : askFallbackUrl(ctx.env);
    if (!config && !fallbackUrl) {
      return json({ success: false, message: "Answers are temporarily unavailable. Please try again later." }, { status: 503 });
    }

    const key = clientKey(req, ctx.session.deviceId);
    for (const [label, limit, window] of [["minute", 10, 60], ["day", 50, 86400]] as const) {
      const allowance = await rateLimit(ctx.env, `chat:${label}:${key}`, limit, window);
      if (!allowance.allowed) {
        return json({ success: false, message: "Too many questions. Please try again later." }, {
          status: 429,
          headers: { "retry-after": String(Math.max(1, Math.ceil((allowance.resetAt - Date.now()) / 1000))) },
        });
      }
    }

    if (!config) {
      const data = await fetchFallback<{ success?: unknown; result?: unknown }>(fallbackUrl!, {
        method: "POST",
        headers: { "content-type": "application/json" },
        body: JSON.stringify({ query, context: body?.context, depth: body?.depth }),
      }, 55_000);
      const result = data?.result && typeof data.result === "object"
        ? buildChatResult({ choices: [{ message: { content: JSON.stringify(data.result) } }] })
        : null;
      if (!result) return json({ success: false, message: UNAVAILABLE }, { status: 502 });
      return json({ success: true, result }, { headers: { "cache-control": "no-store" } });
    }

    try {
      const payload = buildChatPayload(query, body?.context, body?.depth);
      const { data } = await callPerplexity({ ...config, payload, timeoutMs: 55_000 });
      const result = buildChatResult(data);
      if (!result) throw new Error("Empty chat answer");
      return json({ success: true, result }, { headers: { "cache-control": "no-store" } });
    } catch {
      // Never return provider error bodies (or credential details) to a public client.
      return json({ success: false, message: UNAVAILABLE }, { status: 502 });
    }
  })];
}
