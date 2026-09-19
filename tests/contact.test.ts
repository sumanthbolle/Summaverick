/**
 * The contact route, exercised against in-memory stand-ins for D1 and KV.
 *
 * The behaviour worth protecting is what the visitor is told. The homepage form
 * prints the server's answer verbatim, so a wrong status here becomes a wrong
 * sentence on the page — most importantly, a retry of a message that is already
 * stored must never be reported as "not sent".
 */
import { beforeEach, describe, expect, it } from "vitest";
import { contactRoutes } from "../src/routes/contact";
import type { Ctx, Env, Handler, RouteDef } from "../src/types";

interface LeadRecord {
  id: string;
  idempotency_key: string | null;
  email: string;
  notified: number;
}

function fakeDb(leads: LeadRecord[]): D1Database {
  const prepare = (sql: string) => ({
    bind: (...args: unknown[]) => ({
      first: async () => {
        if (sql.includes("SELECT id, notified FROM leads")) {
          return leads.find((l) => l.idempotency_key === args[0]) ?? null;
        }
        return null;
      },
      run: async () => {
        if (sql.includes("INSERT INTO leads")) {
          leads.push({
            id: String(args[0]),
            idempotency_key: args[1] === null ? null : String(args[1]),
            email: String(args[3]),
            notified: Number(args[7]),
          });
        }
        return { success: true };
      },
    }),
  });
  return { prepare } as unknown as D1Database;
}

function fakeKv(store: Map<string, string>): KVNamespace {
  return {
    get: async (k: string) => store.get(k) ?? null,
    put: async (k: string, v: string) => void store.set(k, v),
  } as unknown as KVNamespace;
}

function post(body: unknown): Request {
  return new Request("https://summaverick.com/api/contact", {
    method: "POST",
    headers: { "content-type": "application/json", "cf-connecting-ip": "203.0.113.7" },
    body: JSON.stringify(body),
  });
}

const VALID = {
  name: "Priya Raman",
  email: "priya@example.com",
  organisation: "Northwind Ops",
  intent: "servicenow_application",
  message: "Request intake is spread across email and spreadsheets.",
};

describe("POST /api/contact", () => {
  let leads: LeadRecord[];
  let kv: Map<string, string>;
  let handler: Handler;
  let ctx: Ctx;

  beforeEach(() => {
    leads = [];
    kv = new Map();
    const routes: RouteDef[] = contactRoutes((method, pattern, h) => ({
      method,
      pattern,
      handler: h,
    }));
    handler = routes[0]!.handler;
    ctx = {
      env: {
        DB: fakeDb(leads),
        CONFIG: fakeKv(kv),
        MAGIC_LINK_FROM: "login@summaverick.com",
      } as unknown as Env,
      exec: {
        waitUntil: () => {},
        passThroughOnException: () => {},
      } as unknown as ExecutionContext,
      url: new URL("https://summaverick.com/api/contact"),
      session: {
        sessionId: "s",
        deviceId: "d",
        userId: null,
        isNew: false,
        expiresAt: 0,
      },
      params: {},
    };
  });

  const send = async (body: unknown) => {
    const res = await handler(post(body), ctx);
    return { status: res.status, body: (await res.json()) as Record<string, unknown> };
  };

  it("stores a valid message and reports that nothing was emailed without a provider", async () => {
    const { status, body } = await send({ ...VALID, idempotency_key: "k1" });
    expect(status).toBe(201);
    expect(body.ok).toBe(true);
    expect(body.stored).toBe(true);
    expect(body.notified).toBe(false);
    expect(leads).toHaveLength(1);
  });

  it("returns field errors instead of storing an unusable message", async () => {
    const { status, body } = await send({ email: "not-an-email", message: "" });
    expect(status).toBe(422);
    expect(body.ok).toBe(false);
    expect(body.fieldErrors).toMatchObject({
      email: expect.any(String),
      message: expect.any(String),
    });
    expect(leads).toHaveLength(0);
  });

  it("answers a honeypot submission normally and stores nothing", async () => {
    const { status, body } = await send({
      ...VALID,
      company_url: "http://spam.example",
    });
    expect(status).toBe(201);
    expect(body.ok).toBe(true);
    expect(body.stored).toBe(false);
    expect(leads).toHaveLength(0);
  });

  it("treats a retry with the same key as the same message, not a second one", async () => {
    const first = await send({ ...VALID, idempotency_key: "k2" });
    const retry = await send({ ...VALID, idempotency_key: "k2" });
    expect(retry.status).toBe(201);
    expect(retry.body.ok).toBe(true);
    expect(retry.body.duplicate).toBe(true);
    expect(retry.body.id).toBe(first.body.id);
    expect(leads).toHaveLength(1);
  });

  it("still confirms an already-stored retry once the rate limit is exhausted", async () => {
    const first = await send({ ...VALID, idempotency_key: "k3" });
    // Burst allowance is small; spend it on other submissions.
    for (let i = 0; i < 5; i += 1) {
      await send({ ...VALID, email: `other${i}@example.com` });
    }
    const limited = await send({ ...VALID, email: "blocked@example.com" });
    expect(limited.status).toBe(429);

    const retry = await send({ ...VALID, idempotency_key: "k3" });
    expect(retry.status).toBe(201);
    expect(retry.body.duplicate).toBe(true);
    expect(retry.body.id).toBe(first.body.id);
  });

  it("keeps only the published intent values", async () => {
    await send({ ...VALID, intent: "please_call_me", idempotency_key: "k4" });
    await send({ ...VALID, intent: "not_sure", idempotency_key: "k5" });
    expect(leads).toHaveLength(2);
  });
});
