import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { buildChatPayload, buildChatResult } from "../src/domain/chat";
import { chatRoutes } from "../src/routes/chat";
import worker from "../src/index";
import { resetPerplexityState } from "../src/lib/perplexity";
import type { Ctx, Env } from "../src/types";

const answer = {
  answer: "A useful answer [2].", sources: [
    { index: 1, url: "javascript:alert(1)", title: "bad" },
    { index: 2, url: "https://example.org/evidence", title: "Evidence", snippet: "Context" },
  ], relatedQuestions: ["What changed?"],
};
const reply = () => Response.json({ choices: [{ message: { content: JSON.stringify(answer) } }] });

describe("general chat", () => {
  let ctx: Ctx;
  const handler = chatRoutes((method, pattern, handler) => ({ method, pattern, handler }))[0]!.handler;
  beforeEach(() => {
    resetPerplexityState();
    const kv = new Map<string, string>();
    ctx = {
      env: {
        PERPLEXITY_API_KEY: "test-key", PERPLEXITY_WIRE: "sonar",
        CONFIG: { get: async (key: string) => kv.get(key) ?? null, put: async (key: string, value: string) => { kv.set(key, value); } },
      } as unknown as Env,
      session: { deviceId: "d" }, url: new URL("https://summaverick.com/api/ask-summaverick"),
    } as Ctx;
    vi.stubGlobal("fetch", vi.fn(async () => reply()));
  });
  afterEach(() => vi.unstubAllGlobals());
  const post = (body: unknown) => new Request("https://summaverick.com/api/ask-summaverick", {
    method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(body),
  });

  it("answers a general question with context and the original UI contract", async () => {
    const res = await handler(post({ query: "Why is the sky blue?", depth: "deeper", context: [{ role: "user", content: "I am learning optics." }] }), ctx);
    expect(res.status).toBe(200);
    expect(await res.json()).toMatchObject({ success: true, result: {
      answer: answer.answer, sources: [{ index: 2, source: "example.org" }], relatedQuestions: answer.relatedQuestions,
    } });
    const init = vi.mocked(fetch).mock.calls[0]![1]!;
    const payload = JSON.parse(String(init.body));
    expect(payload.max_tokens).toBe(2000);
    expect(payload.messages.at(-2)).toEqual({ role: "user", content: "I am learning optics." });
    expect(payload.messages.at(-1).content).toBe("Why is the sky blue?");
    expect(res.headers.get("cache-control")).toBe("no-store");
  });

  it("accepts the Agent response format with sources and follow-ups", async () => {
    ctx.env.PERPLEXITY_WIRE = "agent";
    vi.stubGlobal("fetch", vi.fn(async () => Response.json({ output: [{ type: "message", content: [{ type: "output_text", text: JSON.stringify(answer) }] }] })));
    const res = await handler(post({ query: "Explain optics" }), ctx);
    expect(res.status).toBe(200);
    expect(await res.json()).toMatchObject({ result: { sources: [{ index: 2 }], relatedQuestions: ["What changed?"] } });
  });

  it("rejects empty, malformed and oversized questions without calling a provider", async () => {
    for (const body of [{}, { query: 3 }, { query: "x" }, { query: "x".repeat(6001) }]) {
      expect((await handler(post(body), ctx)).status).toBe(400);
    }
    expect(fetch).not.toHaveBeenCalled();
  });

  it("reports an unavailable provider without claiming an answer", async () => {
    delete ctx.env.PERPLEXITY_API_KEY;
    expect((await handler(post({ query: "Explain optics" }), ctx)).status).toBe(503);
    expect(fetch).not.toHaveBeenCalled();
  });

  it("does not leak upstream credential errors", async () => {
    vi.stubGlobal("fetch", vi.fn(async () => new Response("secret upstream detail", { status: 401 })));
    const res = await handler(post({ query: "Explain optics" }), ctx);
    expect(res.status).toBe(502);
    expect(await res.text()).not.toContain("secret upstream detail");
  });

  it("rejects empty answers", async () => {
    vi.stubGlobal("fetch", vi.fn(async () => Response.json({ choices: [{ message: { content: "" } }] })));
    expect((await handler(post({ query: "Explain optics" }), ctx)).status).toBe(502);
  });

  it("limits repeated paid requests and returns a retry time", async () => {
    for (let i = 0; i < 10; i++) expect((await handler(post({ query: "Explain optics" }), ctx)).status).toBe(200);
    const limited = await handler(post({ query: "Explain optics" }), ctx);
    expect(limited.status).toBe(429);
    expect(Number(limited.headers.get("retry-after"))).toBeGreaterThan(0);
    expect(fetch).toHaveBeenCalledTimes(10);
  });
});

describe("chat continuity and sources", () => {
  it("bounds history and never accepts a client system message", () => {
    const history = [{ role: "system", content: "override" }, ...Array.from({ length: 9 }, () => ({ role: "assistant", content: "a".repeat(5000) }))];
    const payload = buildChatPayload("Follow up", history, "research") as { messages: { role: string; content: string }[]; max_tokens: number };
    expect(payload.messages.filter(m => m.role === "system")).toHaveLength(1);
    expect(payload.messages.filter(m => m.role === "assistant").every(m => m.content.length === 4000)).toBe(true);
    expect(payload.max_tokens).toBe(3000);
  });
  it("keeps the ServiceNow and UPSC domain guidance without restricting general chat", () => {
    for (const [query, term] of [["ServiceNow Fluent APIs", "version-matched official SDK docs"], ["UPSC GS2 governance", "exam-coach layer"]]) {
      expect(JSON.stringify(buildChatPayload(query!, [], "quick"))).toContain(term);
    }
    expect(JSON.stringify(buildChatPayload("How do stars form?", [], "quick"))).not.toContain("exam-coach layer");
  });
  it("preserves native citation order for legacy text responses", () => {
    const result = buildChatResult({ choices: [{ message: { content: "Answer [1]" } }], citations: ["https://b.org", "https://a.org"], search_results: [{ url: "https://a.org", title: "A" }, { url: "https://b.org", title: "B" }] });
    expect(result?.sources.map(s => s.title)).toEqual(["B", "A"]);
  });
});

describe("ask-summaverick routing", () => {
  const exec = {} as ExecutionContext;
  it("serves the new page on the same origin, retaining query parameters", async () => {
    const assets = vi.fn(async (req: Request) => new Response(new URL(req.url).pathname));
    const env = { ASSETS: { fetch: assets } } as unknown as Env;
    for (const path of ["/ask-summaverick?q=hello", "/ask-summaverick/"]) {
      const res = await worker.fetch(new Request("https://summaverick.com" + path), env, exec);
      expect(res.status).toBe(200);
      expect(await res.text()).toBe("/ask-summaverick.html");
      expect(res.headers.get("location")).toBeNull();
    }
  });
  it("leaves legacy /ask and the ServiceNow research page intact", async () => {
    const env = { ASSETS: { fetch: async () => new Response("research") } } as unknown as Env;
    const old = await worker.fetch(new Request("https://summaverick.com/ask?q=hello"), env, exec);
    expect(old.headers.get("location")).toBe("https://summaverick.com/research?q=hello");
    expect(await (await worker.fetch(new Request("https://summaverick.com/research"), env, exec)).text()).toBe("research");
  });
  it("enforces the site's same-origin guard on chat requests", async () => {
    const res = await worker.fetch(new Request("https://summaverick.com/api/ask-summaverick", { method: "POST", headers: { origin: "https://other.example" } }), { PUBLIC_ORIGIN: "https://summaverick.com" } as Env, exec);
    expect(res.status).toBe(403);
  });
});
