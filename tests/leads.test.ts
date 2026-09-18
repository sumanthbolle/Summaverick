/**
 * The contact endpoint answers two callers: the homepage's fetch, which wants
 * JSON, and a browser posting the form itself when the script did not load,
 * which needs a page it can read. A regression in either one loses enquiries
 * silently, so both shapes are pinned here along with the honeypot.
 */
import { describe, expect, it } from "vitest";
import { leadRoutes } from "../src/routes/leads";
import type { Ctx, Env, RouteMaker } from "../src/types";

const route: RouteMaker = (method, pattern, handler) => ({ method, pattern, handler });

function postHandler() {
  const def = leadRoutes(route).find((r) => r.method === "POST" && r.pattern === "/api/leads");
  if (!def) throw new Error("POST /api/leads is not registered");
  return def.handler;
}

/** Just enough of the platform for the handler: one KV counter and one insert. */
function fakeCtx(): { ctx: Ctx; inserted: Record<string, unknown>[] } {
  const kv = new Map<string, string>();
  const inserted: Record<string, unknown>[] = [];
  const statement = {
    bind: (...args: unknown[]) => ({
      run: async () => {
        inserted.push({ args });
        return { success: true };
      },
    }),
  };
  const env = {
    ENVIRONMENT: "development",
    CONFIG: {
      get: async (k: string) => kv.get(k) ?? null,
      put: async (k: string, v: string) => void kv.set(k, v),
    },
    DB: { prepare: () => statement },
  } as unknown as Env;

  return {
    ctx: {
      env,
      exec: { waitUntil: () => {}, passThroughOnException: () => {} } as unknown as ExecutionContext,
      url: new URL("https://summaverick.com/api/leads"),
      session: { deviceId: "dev_test" } as Ctx["session"],
      params: {},
    },
    inserted,
  };
}

const formRequest = (fields: Record<string, string>) =>
  new Request("https://summaverick.com/api/leads", {
    method: "POST",
    headers: { "content-type": "application/x-www-form-urlencoded" },
    body: new URLSearchParams(fields),
  });

const jsonRequest = (body: unknown) =>
  new Request("https://summaverick.com/api/leads", {
    method: "POST",
    headers: { "content-type": "application/json" },
    body: JSON.stringify(body),
  });

const valid = {
  email: "someone@example.com",
  message: "Approvals are spread over three tools and nobody can see where a request is.",
  intent: "integration",
};

describe("POST /api/leads", () => {
  it("answers the homepage's fetch with JSON and a reference", async () => {
    const { ctx, inserted } = fakeCtx();
    const res = await postHandler()(jsonRequest(valid), ctx);

    expect(res.status).toBe(200);
    expect(res.headers.get("content-type")).toContain("application/json");
    const body = (await res.json()) as { ok: boolean; reference: string };
    expect(body.ok).toBe(true);
    expect(body.reference).toMatch(/^lead_/);
    expect(inserted).toHaveLength(1);
  });

  it("answers a plain form post with a page, so a visitor without JS is told", async () => {
    const { ctx, inserted } = fakeCtx();
    const res = await postHandler()(formRequest(valid), ctx);

    expect(res.status).toBe(200);
    expect(res.headers.get("content-type")).toContain("text/html");
    const html = await res.text();
    expect(html).toContain("Message received");
    expect(html).toMatch(/lead_[0-9a-f]+/);
    // Unindexed, and a way back to the page they came from.
    expect(html).toContain('name="robots" content="noindex"');
    expect(html).toContain('href="/#contact"');
    expect(inserted).toHaveLength(1);
  });

  it("explains a rejection in the same format it was asked in", async () => {
    const short = { ...valid, message: "too short" };

    const { ctx: a } = fakeCtx();
    const asJson = await postHandler()(jsonRequest(short), a);
    expect(asJson.status).toBe(400);
    expect(asJson.headers.get("content-type")).toContain("application/json");

    const { ctx: b, inserted } = fakeCtx();
    const asPage = await postHandler()(formRequest(short), b);
    expect(asPage.status).toBe(400);
    expect(asPage.headers.get("content-type")).toContain("text/html");
    expect(await asPage.text()).toContain("a sentence is enough");
    expect(inserted).toHaveLength(0);
  });

  it("stores nothing when the honeypot is filled, and does not say so", async () => {
    const { ctx, inserted } = fakeCtx();
    const res = await postHandler()(jsonRequest({ ...valid, company_url: "http://spam" }), ctx);

    expect(res.status).toBe(200);
    expect(((await res.json()) as { ok: boolean }).ok).toBe(true);
    expect(inserted).toHaveLength(0);
  });

  it("holds the rate limit open for a retry rather than refusing outright", async () => {
    const { ctx } = fakeCtx();
    const handler = postHandler();
    const statuses: number[] = [];
    for (let i = 0; i < 7; i += 1) {
      statuses.push((await handler(formRequest(valid), ctx)).status);
    }

    expect(statuses.slice(0, 5)).toEqual([200, 200, 200, 200, 200]);
    expect(statuses.at(-1)).toBe(429);

    const limited = await handler(formRequest(valid), ctx);
    expect(Number(limited.headers.get("retry-after"))).toBeGreaterThan(0);
    expect(await limited.text()).toContain("Try again in about");
  });
});
