import { createServer } from "node:http";
import { spawn } from "node:child_process";
import { fileURLToPath } from "node:url";
import { describe, expect, it } from "vitest";

const script = fileURLToPath(new URL("../scripts/verify-perplexity.mjs", import.meta.url));

async function verify(options: { mode?: string; status?: number; body?: unknown } = {}) {
  const requests: { path: string; method: string; body: string }[] = [];
  const server = createServer(async (req, res) => {
    let text = "";
    for await (const chunk of req) text += chunk;
    requests.push({ path: req.url ?? "", method: req.method ?? "", body: text });
    res.setHeader("content-type", "application/json");
    if (req.url === "/api/research/mode") {
      res.end(JSON.stringify({ ok: true, mode: options.mode ?? "model_answer" }));
    } else {
      res.statusCode = options.status ?? 200;
      res.end(JSON.stringify(options.body ?? { success: true, result: { answer: "Two plus two is 4." } }));
    }
  });
  await new Promise<void>(resolve => server.listen(0, "127.0.0.1", resolve));
  const address = server.address();
  if (!address || typeof address === "string") throw new Error("No fixture server");
  try {
    const result = await new Promise<{ code: number | null; output: string }>((resolve, reject) => {
      const child = spawn(process.execPath, [script, `http://127.0.0.1:${address.port}`], { timeout: 5000 });
      let output = "";
      child.stdout.on("data", chunk => { output += chunk; });
      child.stderr.on("data", chunk => { output += chunk; });
      child.on("error", reject);
      child.on("close", code => resolve({ code, output }));
    });
    return { ...result, requests };
  } finally {
    await new Promise<void>((resolve, reject) => server.close(err => err ? reject(err) : resolve()));
  }
}

describe("post-deploy Perplexity check", () => {
  it("requires an active binding and a real assistant answer before exiting successfully", async () => {
    const run = await verify();
    expect(run.code).toBe(0);
    expect(run.output).toContain("PASS Perplexity answered");
    expect(run.requests.map(r => [r.method, r.path])).toEqual([
      ["GET", "/api/research/mode"], ["POST", "/api/ask-summaverick"],
    ]);
    expect(JSON.parse(run.requests[1]!.body)).toMatchObject({ depth: "quick", query: expect.stringContaining("two plus two") });
  });

  it("stops before the paid request when the Worker binding is absent", async () => {
    const run = await verify({ mode: "source_results" });
    expect(run.code).toBe(1);
    expect(run.output).toContain("no PERPLEXITY_API_KEY binding");
    expect(run.requests).toHaveLength(1);
  });

  it.each([
    [502, "key but its model request failed"],
    [503, "no usable key binding"],
    [429, "rate-limited"],
  ])("fails on HTTP %i without logging provider response bodies", async (status, diagnostic) => {
    const run = await verify({ status: Number(status), body: { success: false, message: "private provider diagnostic" } });
    expect(run.code).toBe(1);
    expect(run.output).toContain(diagnostic);
    expect(run.output).not.toContain("private provider diagnostic");
  });

  it.each([
    { success: false, result: { answer: "4" } },
    { success: true, result: { answer: "" } },
    { success: true, result: { answer: "A canned offline draft" } },
  ])("rejects an unusable HTTP 200 response: %j", async body => {
    const run = await verify({ body });
    expect(run.code).toBe(1);
    expect(run.output).toContain("did not return the expected model answer");
  });
});
