/**
 * Customer-advocacy demo on the Worker.
 *
 * POST /api/advocate/demo?scenario=cooperative|stubborn
 * GET  /api/advocate/demo/stream?scenario=…
 * GET  /api/advocate
 */
import type { RouteDef, RouteMaker } from "../types";
import { buildDemoEvents, parseScenario } from "../domain/advocate";
import { ok } from "../lib/json";

function sleep(ms: number): Promise<void> {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

function streamDemo(scenario: ReturnType<typeof parseScenario>): Response {
  const events = buildDemoEvents(scenario);
  const encoder = new TextEncoder();
  const stream = new ReadableStream({
    async start(controller) {
      try {
        for (const evt of events) {
          if (evt.delayMs > 0) await sleep(evt.delayMs);
          controller.enqueue(
            encoder.encode(
              `event: ${evt.event}\ndata: ${JSON.stringify(evt.data)}\n\n`
            )
          );
        }
        controller.enqueue(encoder.encode("event: done\ndata: {}\n\n"));
        controller.close();
      } catch {
        try {
          controller.close();
        } catch {
          /* already closed */
        }
      }
    },
  });
  return new Response(stream, {
    headers: {
      "content-type": "text/event-stream; charset=utf-8",
      "cache-control": "no-cache, no-transform",
      connection: "keep-alive",
    },
  });
}

export function advocateRoutes(route: RouteMaker): RouteDef[] {
  return [
    route("GET", "/api/advocate", async () =>
      ok({
        product: "autonomous customer-advocacy agent",
        demo: "/advocate",
        scenarios: ["cooperative", "stubborn"],
      })
    ),

    route("POST", "/api/advocate/demo", async (_req, ctx) => {
      const scenario = parseScenario(ctx.url.searchParams.get("scenario"));
      return ok({
        scenario,
        stream: `/api/advocate/demo/stream?scenario=${scenario}`,
      });
    }),

    route("GET", "/api/advocate/demo/stream", async (_req, ctx) => {
      return streamDemo(parseScenario(ctx.url.searchParams.get("scenario")));
    }),
  ];
}
