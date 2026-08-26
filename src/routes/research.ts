/**
 * Research API (T8 — live agent).
 *
 * Surfaces the ported ServiceNow 3-layer retrieval pipeline
 * (src/domain/research) as a live, streamed trace the browser renders stage by
 * stage. Three routes:
 *
 *   POST /api/research/stream  SSE: accepted → classify → expand → injection
 *                              gate → retrieve → per-layer → dedup → verify →
 *                              answer → done. Rate-limited; a query that trips
 *                              the injection detector is rejected BEFORE the
 *                              model and the block is streamed to the UI.
 *   POST /api/research         Non-streaming; returns { answer, trace }.
 *   GET  /api/research/:id     A completed run (best-effort, from D1 + R2).
 *
 * No Durable Object: the run streams over a single connection, so this works in
 * local `wrangler dev` with no Cloudflare account (see docs/decisions.md). The
 * pipeline degrades to a deterministic evidence-backed draft when no Perplexity
 * key is set, so the demo works offline.
 */
import type { Ctx, Env, RouteDef, RouteMaker } from "../types";
import { badRequest, json, newId, nowMs, notFound, ok, readJson, serverError } from "../lib/json";
import { rateLimit, clientKey } from "../lib/ratelimit";
import { createSseStream } from "../lib/sse";
import { runResearchPipeline } from "../domain/research";
import { classifyServiceNowIntent } from "../domain/research/retrieval/query-classifier";
import { expandServiceNowQuery } from "../domain/research/retrieval/query-expander";
import { scanForPromptInjection } from "../domain/research/security/prompt-injection";

const MAX_QUERY = 500;                 // public demo cap
const BURST = { limit: 6, window: 60 };        // 6 / minute
const DAILY = { limit: 50, window: 86400 };    // 50 / day

interface RunRecord {
  id: string;
  deviceId: string;
  query: string;
  intent: string | null;
  layersJson: string | null;
  citations: number | null;
  verified: number | null;
  injectionFlagged: number;
  latencyMs: number | null;
  traceKey: string | null;
  traceJson: string | null;
  status: "done" | "failed" | "blocked";
  createdAt: number;
  finishedAt: number | null;
}

/** Best-effort persistence. A missing table or binding never breaks a run. */
function persistRun(env: Env, exec: ExecutionContext, r: RunRecord): void {
  exec.waitUntil(
    (async () => {
      try {
        if (r.traceKey && r.traceJson && env.BODIES) {
          await env.BODIES.put(r.traceKey, r.traceJson, {
            httpMetadata: { contentType: "application/json" },
          });
        }
        await env.DB.prepare(
          `INSERT OR REPLACE INTO research_runs
            (id, device_id, query, intent, layers_json, citations, verified,
             injection_flagged, latency_ms, tokens_in, tokens_out, trace_r2_key,
             status, created_at, finished_at)
           VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)`
        )
          .bind(
            r.id, r.deviceId, r.query, r.intent, r.layersJson, r.citations,
            r.verified, r.injectionFlagged, r.latencyMs, null, null, r.traceKey,
            r.status, r.createdAt, r.finishedAt
          )
          .run();
      } catch (e) {
        console.error("persistRun failed (non-fatal)", e);
      }
    })()
  );
}

function rateLimited(resetAt: number): Response {
  const secs = Math.max(1, Math.ceil((resetAt - nowMs()) / 1000));
  return json(
    { error: "rate_limited", message: `Too many requests. Try again in ${secs}s.` },
    { status: 429, headers: { "retry-after": String(secs) } }
  );
}

async function readQuery(req: Request, ctx: Ctx): Promise<{ query?: string; err?: Response }> {
  const body = await readJson<{ query?: unknown }>(req);
  const raw =
    typeof body?.query === "string" ? body.query : ctx.url.searchParams.get("q") ?? "";
  const query = raw.trim();
  if (!query) return { err: badRequest("a non-empty `query` string is required") };
  if (query.length > MAX_QUERY) return { err: badRequest(`query is too long (max ${MAX_QUERY} chars)`) };
  return { query };
}

export function researchRoutes(route: RouteMaker): RouteDef[] {
  return [
    // ---- Streamed live trace -------------------------------------------
    route("POST", "/api/research/stream", async (req, ctx: Ctx) => {
      const { query, err } = await readQuery(req, ctx);
      if (err) return err;

      const key = clientKey(req, ctx.session.deviceId);
      const burst = await rateLimit(ctx.env, `res:b:${key}`, BURST.limit, BURST.window);
      const daily = await rateLimit(ctx.env, `res:d:${key}`, DAILY.limit, DAILY.window);
      if (!daily.allowed) return rateLimited(daily.resetAt);
      if (!burst.allowed) return rateLimited(burst.resetAt);

      const q = query!;
      const started = nowMs();
      const id = newId("run");
      const sse = createSseStream();

      const pump = (async () => {
        try {
          await sse.send("stage", { key: "accepted", label: "request accepted", detail: `run ${id}`, kind: "ok" });

          const cls = classifyServiceNowIntent(q);
          await sse.send("stage", {
            key: "classify", label: "classify",
            detail: `intent → ${cls.intent} · confidence ${cls.confidence}`, kind: "ok",
          });

          const expansions = expandServiceNowQuery(q);
          await sse.send("stage", {
            key: "expand", label: "expand",
            detail: `+${Math.max(0, expansions.length - 1)} query variants`, kind: "ok",
          });

          // Injection gate — before any model call.
          const scan = scanForPromptInjection(q);
          if (scan.suspicious) {
            await sse.send("stage", { key: "injection", label: "injection check", detail: "prompt-injection pattern detected", kind: "block" });
            await sse.send("blocked", {
              reason: "The query tripped the injection detector and was rejected before any model call.",
              matched: scan.matchedPatterns.slice(0, 3),
            });
            persistRun(ctx.env, ctx.exec, {
              id, deviceId: ctx.session.deviceId, query: q, intent: cls.intent,
              layersJson: null, citations: 0, verified: 0, injectionFlagged: 1,
              latencyMs: nowMs() - started, traceKey: null, traceJson: null,
              status: "blocked", createdAt: started, finishedAt: nowMs(),
            });
            await sse.send("done", { id, status: "blocked" });
            return;
          }
          await sse.send("stage", { key: "injection", label: "injection check", detail: "clean", kind: "ok" });

          await sse.send("stage", { key: "retrieve", label: "retrieve", detail: "consulting the retrieval layers…", kind: "active" });

          const result = await runResearchPipeline({
            query: q,
            env: ctx.env as unknown as Record<string, string | undefined>,
            perplexityApiKey: ctx.env.PERPLEXITY_API_KEY,
          });
          const t = result.trace;

          if (t.routed) {
            const totalCandidates = t.layers.reduce((n, l) => n + l.candidateCount, 0);
            for (const layer of t.layers) {
              const locked = layer.sourceType === "live_instance" && !t.security.liveInstanceEnabled;
              if (!layer.planned && layer.candidateCount === 0 && !locked) continue;
              await sse.send("stage", {
                key: "layer", label: layer.layer,
                detail: locked
                  ? "off by default — skipped"
                  : `${layer.candidateCount} candidate(s)${layer.answered ? " · answered here" : ""}`,
                kind: locked ? "muted" : "ok",
              });
            }
            await sse.send("stage", {
              key: "dedup", label: "dedup + rank",
              detail: `${t.candidateDocumentCount} ranked of ${totalCandidates} candidate(s)`, kind: "ok",
            });
            if (t.verification) {
              const v = t.verification;
              await sse.send("stage", {
                key: "verify", label: "verify",
                detail: `${v.citationCount} citation(s) · ${v.unsupportedClaimCount} unsupported claim(s)`,
                kind: v.ok ? "ok" : "block",
              });
            }
          } else {
            await sse.send("stage", { key: "route", label: "domain routing", detail: "outside the ServiceNow domain — no layers engaged", kind: "muted" });
          }

          await sse.send("answer", {
            text: result.answer,
            citations: t.evidence.slice(0, 6).map((e) => ({ title: e.title, url: e.url ?? null, sourceType: e.sourceType })),
            verification: t.verification,
            llmUsed: t.llm.used,
            llmModel: t.llm.model,
            // Why the model was not used (missing key, or an upstream/egress
            // failure), so a fallback to the draft is explainable in the UI.
            llmError: t.llm.error ?? null,
            // Compact summary only — the full case list is large and the public
            // scoreboard (T9) reads eval_results from D1, not this payload.
            evalScores: {
              total: t.evalScores.total,
              passed: t.evalScores.passed,
              passRate: t.evalScores.passRate,
              adversarialBlockRate: t.evalScores.adversarialBlockRate,
            },
          });

          persistRun(ctx.env, ctx.exec, {
            id, deviceId: ctx.session.deviceId, query: q, intent: t.classification.intent,
            layersJson: JSON.stringify(t.layers), citations: t.verification?.citationCount ?? 0,
            verified: t.verification?.ok ? 1 : 0, injectionFlagged: 0,
            latencyMs: nowMs() - started, traceKey: `traces/${id}.json`,
            traceJson: JSON.stringify(t), status: "done", createdAt: started, finishedAt: nowMs(),
          });
          await sse.send("done", { id, status: "done" });
        } catch (e) {
          console.error("stream research error", e);
          await sse.send("error", { message: e instanceof Error ? e.message : "research failed" }).catch(() => {});
          persistRun(ctx.env, ctx.exec, {
            id, deviceId: ctx.session.deviceId, query: q, intent: null, layersJson: null,
            citations: 0, verified: 0, injectionFlagged: 0, latencyMs: nowMs() - started,
            traceKey: null, traceJson: null, status: "failed", createdAt: started, finishedAt: nowMs(),
          });
          await sse.send("done", { id, status: "failed" }).catch(() => {});
        } finally {
          await sse.close();
        }
      })();

      ctx.exec.waitUntil(pump);
      return sse.response;
    }),

    // ---- Non-streaming (programmatic) ----------------------------------
    route("POST", "/api/research", async (req, ctx: Ctx) => {
      const { query, err } = await readQuery(req, ctx);
      if (err) return err;

      const key = clientKey(req, ctx.session.deviceId);
      const daily = await rateLimit(ctx.env, `res:d:${key}`, DAILY.limit, DAILY.window);
      if (!daily.allowed) return rateLimited(daily.resetAt);

      try {
        const { answer, trace } = await runResearchPipeline({
          query: query!,
          env: ctx.env as unknown as Record<string, string | undefined>,
          perplexityApiKey: ctx.env.PERPLEXITY_API_KEY,
        });
        return ok({ answer, trace });
      } catch (e) {
        console.error("research pipeline error", e);
        return serverError(e instanceof Error ? e.message : "research pipeline failed");
      }
    }),

    // ---- Fetch a completed run -----------------------------------------
    route("GET", "/api/research/:id", async (_req, ctx: Ctx) => {
      const id = ctx.params.id;
      try {
        const row = await ctx.env.DB.prepare(
          `SELECT id, query, intent, citations, verified, injection_flagged,
                  latency_ms, trace_r2_key, status, created_at, finished_at
             FROM research_runs WHERE id = ?`
        )
          .bind(id)
          .first<Record<string, unknown>>();
        if (!row) return notFound("no such run");

        let trace: unknown = null;
        const traceKey = row.trace_r2_key as string | null;
        if (traceKey && ctx.env.BODIES) {
          const obj = await ctx.env.BODIES.get(traceKey);
          if (obj) trace = await obj.json();
        }
        return ok({ run: row, trace });
      } catch (e) {
        console.error("get run error", e);
        return serverError("could not load run");
      }
    }),
  ];
}
