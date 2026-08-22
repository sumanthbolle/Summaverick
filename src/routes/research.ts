/**
 * Research API (T12).
 *
 * Surfaces the ported ServiceNow 3-layer retrieval pipeline
 * (src/domain/research) behind a single endpoint that returns the answer PLUS a
 * fully visible trace: query classification, retrieval hops (which layer answered
 * + candidate-doc counts), the evidence gates that fired, per-claim evidence
 * verification, and the adversarial/regression eval scores.
 *
 * The pipeline itself is pure domain code; this module only handles HTTP.
 */
import type { Ctx, RouteDef, RouteMaker } from "../types";
import { badRequest, error, ok, readJson, serverError } from "../lib/json";
import { runResearchPipeline } from "../domain/research";

export function researchRoutes(route: RouteMaker): RouteDef[] {
  return [
    route("POST", "/api/research", async (req, ctx: Ctx) => {
      const body = await readJson<{ query?: unknown }>(req);
      const query =
        typeof body?.query === "string" ? body.query.trim() : "";
      if (!query) return badRequest("a non-empty `query` string is required");
      if (query.length > 2000) return badRequest("query is too long (max 2000 chars)");

      // Perplexity is the only external LLM. Guard a missing key with a clear
      // "not configured" error rather than crashing mid-request.
      if (!ctx.env.PERPLEXITY_API_KEY) {
        return error(
          503,
          "not_configured",
          "Perplexity is not configured on this deployment (set the PERPLEXITY_API_KEY secret)."
        );
      }

      try {
        const { answer, trace } = await runResearchPipeline({
          query,
          env: ctx.env as unknown as Record<string, string | undefined>,
          perplexityApiKey: ctx.env.PERPLEXITY_API_KEY,
        });
        return ok({ answer, trace });
      } catch (err) {
        console.error("research pipeline error", err);
        return serverError(
          err instanceof Error ? err.message : "research pipeline failed"
        );
      }
    }),
  ];
}
