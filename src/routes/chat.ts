/** General-purpose chat for /ask-summaverick; the ServiceNow research API stays separate. */
import type { RouteDef, RouteMaker } from "../types";
import { json, readJson } from "../lib/json";
import { callPerplexity, perplexityConfig } from "../lib/perplexity";
import { clientKey, rateLimit } from "../lib/ratelimit";
import { buildChatPayload, buildChatResult, sanitize } from "../domain/chat";

export function chatRoutes(route: RouteMaker): RouteDef[] {
  return [route("POST", "/api/ask-summaverick", async (req, ctx) => {
    const body = await readJson<{ query?: unknown; context?: unknown; depth?: unknown }>(req);
    const query = sanitize(body?.query);
    if (query.length < 2 || query.length > 6000) {
      return json({ success: false, message: "Enter a question between 2 and 6,000 characters." }, { status: 400 });
    }

    const config = perplexityConfig(ctx.env);
    if (!config) {
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
    try {
      const payload = buildChatPayload(query, body?.context, body?.depth);
      const { data } = await callPerplexity({ ...config, payload, timeoutMs: 55_000 });
      const result = buildChatResult(data);
      if (!result) throw new Error("Empty chat answer");
      return json({ success: true, result }, { headers: { "cache-control": "no-store" } });
    } catch {
      // Never return provider error bodies (or credential details) to a public client.
      return json({ success: false, message: "Could not get an answer right now. Please try again." }, { status: 502 });
    }
  })];
}
