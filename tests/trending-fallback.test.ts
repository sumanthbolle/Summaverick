import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { toolsRoutes } from "../src/routes/tools";
import type { Ctx, Env } from "../src/types";

const handler = toolsRoutes((method, pattern, handler) => ({ method, pattern, handler }))
  .find((r) => r.pattern === "/api/trending")!.handler;

describe("trending widgets without a Perplexity key", () => {
  const put = vi.fn(async () => {});
  const ctxFor = (env: Partial<Env>, search = ""): Ctx => ({
    env: env as Env,
    exec: { waitUntil: () => {} } as unknown as ExecutionContext,
    url: new URL("https://summaverick.com/api/trending" + search),
    session: { deviceId: "d" },
    params: {},
  } as Ctx);

  beforeEach(() => {
    put.mockClear();
    vi.stubGlobal("caches", { default: { match: async () => undefined, put } });
  });
  afterEach(() => vi.unstubAllGlobals());

  it("serves the fallback service's widgets and caches them", async () => {
    const widgets = { worldNews: { headline: "Story" }, news: null, market: null, tech: null };
    vi.stubGlobal("fetch", vi.fn(async () => Response.json({ success: true, country: "IN", widgets })));
    const res = await handler(new Request("https://summaverick.com/api/trending?country=in"),
      ctxFor({ ASK_FALLBACK_URL: "https://fallback.example.workers.dev" }, "?country=in"));
    expect(await res.json()).toMatchObject({ success: true, widgets });
    expect(vi.mocked(fetch).mock.calls[0]![0]).toBe("https://fallback.example.workers.dev/trending?country=IN");
    expect(put).toHaveBeenCalledOnce();
  });

  it("reports the missing key when there is no usable fallback", async () => {
    vi.stubGlobal("fetch", vi.fn(async () => Response.json({ success: false })));
    for (const env of [{}, { ASK_FALLBACK_URL: "" }, { ASK_FALLBACK_URL: "https://fallback.example.workers.dev" }]) {
      const res = await handler(new Request("https://summaverick.com/api/trending"), ctxFor(env));
      expect(await res.json()).toEqual({ success: false, error: "API key not configured" });
    }
    expect(put).not.toHaveBeenCalled();
  });
});
