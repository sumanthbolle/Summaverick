/*
 * Shared client for the streamed research endpoint (POST /api/research/stream).
 * Parses the SSE frames and dispatches to handlers. Used by the home agent
 * preview and the Ask Summaverick page.
 *
 *   await streamResearch(query, {
 *     stage(d), blocked(d), answer(d), error(d), done(d), rateLimited(msg)
 *   });
 *
 * Resolves true when the stream ran (including a 429, handled via rateLimited).
 * Throws when the endpoint is unreachable or returns a non-streaming error, so
 * the caller can fall back (e.g. to a captured trace on static hosting).
 */
export async function streamResearch(query, on = {}) {
  const res = await fetch("/api/research/stream", {
    method: "POST",
    headers: { "content-type": "application/json" },
    body: JSON.stringify({ query }),
  });

  if (!res.ok || !res.body) {
    let msg = `HTTP ${res.status}`;
    try { const j = await res.json(); msg = j.message || msg; } catch (e) {}
    if (res.status === 429) { on.rateLimited?.(msg); return true; }
    throw new Error(msg);
  }

  const reader = res.body.getReader();
  const dec = new TextDecoder();
  let buf = "";
  for (;;) {
    const { value, done } = await reader.read();
    if (done) break;
    buf += dec.decode(value, { stream: true });
    let idx;
    while ((idx = buf.indexOf("\n\n")) >= 0) {
      const frame = buf.slice(0, idx);
      buf = buf.slice(idx + 2);
      let type = "message";
      let payload = "";
      for (const line of frame.split("\n")) {
        if (line.startsWith("event:")) type = line.slice(6).trim();
        else if (line.startsWith("data:")) payload += line.slice(5).trim();
      }
      if (!payload) continue;
      let data;
      try { data = JSON.parse(payload); } catch (e) { continue; }
      (on[type])?.(data);
    }
  }
  return true;
}
