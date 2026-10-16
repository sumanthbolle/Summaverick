/** Post-deploy check: verify the active Worker's binding AND an actual AI answer.
 *  No Perplexity key is sent to this script or printed. The deployed Worker
 *  reads its own secret. This makes one small model request.
 *  node scripts/verify-perplexity.mjs [origin]
 */
const base = process.argv[2] ?? "https://summaverick.com";

async function request(path, init = {}) {
  const response = await fetch(new URL(path, base), {
    ...init,
    headers: { "content-type": "application/json", ...init.headers },
    signal: AbortSignal.timeout(120_000),
    redirect: "error",
  });
  let body = null;
  try { body = await response.json(); } catch { /* fail the contract check below */ }
  return { status: response.status, body };
}

async function main() {
  const mode = await request("/api/research/mode");
  if (mode.status !== 200 || mode.body?.ok !== true) {
    throw new Error(`Worker configuration check returned HTTP ${mode.status}. Check the deployment and domain route.`);
  }
  if (mode.body.mode !== "model_answer") {
    throw new Error("The active summaverick Worker has no PERPLEXITY_API_KEY binding. Check the secret-upload step and Cloudflare account/Worker name.");
  }
  console.log("PASS active Worker has an answer-model key configured");

  const answer = await request("/api/ask-summaverick", {
    method: "POST",
    body: JSON.stringify({ query: "What is two plus two? Answer in one short sentence.", depth: "quick" }),
  });
  if (answer.status === 503) {
    throw new Error("The assistant has no usable key binding (HTTP 503). Check the active Worker version.");
  }
  if (answer.status === 502) {
    throw new Error("The Worker has a key but its model request failed (HTTP 502). Check Perplexity key validity, credits and provider availability.");
  }
  if (answer.status === 429) {
    throw new Error("The assistant is rate-limited (HTTP 429). Retry after the rate-limit window.");
  }
  const text = answer.body?.result?.answer;
  if (answer.status !== 200 || answer.body?.success !== true || typeof text !== "string" || !/\b(4|four)\b/i.test(text)) {
    throw new Error(`The assistant did not return the expected model answer (HTTP ${answer.status}).`);
  }
  console.log("PASS Perplexity answered through the deployed /api/ask-summaverick route");
}

main().catch(error => {
  // Only controlled diagnostics: never print request headers or provider bodies.
  console.error(`::error::${error instanceof Error ? error.message : "Perplexity connection check failed"}`);
  process.exitCode = 1;
});
