import { describe, expect, it, vi } from "vitest";
import worker from "../src/index";
import type { Env } from "../src/types";

const exec = {} as ExecutionContext;
const assets = vi.fn(async (req: Request) => new Response(new URL(req.url).pathname));
const env = { ASSETS: { fetch: assets } } as unknown as Env;
const get = (path: string) => worker.fetch(new Request("https://summaverick.com" + path), env, exec);

describe("retired side projects", () => {
  it("redirects the old consumer pages permanently, with and without .html", async () => {
    for (const [path, to] of [
      ["/quiz", "/"], ["/tools.html", "/"], ["/flights", "/"], ["/metals/", "/"], ["/upsc", "/"],
      ["/advocate", "/"], ["/signin", "/"], ["/interviews", "/learn"], ["/search.html", "/learn"],
    ] as const) {
      const res = await get(path);
      expect(res.status, path).toBe(301);
      expect(res.headers.get("location"), path).toBe("https://summaverick.com" + to);
    }
  });

  it("sends retired interview articles to the guides and keeps essays", async () => {
    const interview = "what-is-gliderecord-write-a-basic-gliderecord-query-to-find-all-open-p1-incident";
    for (const path of [`/article/${interview}`, `/article/${interview}.html`]) {
      const res = await get(path);
      expect(res.status, path).toBe(301);
      expect(res.headers.get("location")).toBe("https://summaverick.com/learn");
    }
    const essay = await get("/article/csdm-for-beginners");
    expect(essay.status).toBe(200);
    expect(await essay.text()).toBe("/article/csdm-for-beginners");

    assets.mockImplementationOnce(async () => new Response("missing", { status: 404 }));
    const unbuilt = await get("/article/added-after-the-last-build");
    expect(await unbuilt.text()).toBe("/article");
  });

  it("still serves the company pages", async () => {
    for (const path of ["/", "/ask-summaverick", "/research", "/learn"]) {
      expect((await get(path)).status, path).toBe(200);
    }
  });
});
