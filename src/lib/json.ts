/** HTTP + JSON helpers. Also the single home for epoch-ms time + id helpers. */

/** Unix epoch milliseconds. Every `*_at` column stores the output of this. */
export const nowMs = (): number => Date.now();

/** URL-safe opaque id (session ids, magic tokens, row ids). */
export function newId(prefix = ""): string {
  const bytes = new Uint8Array(16);
  crypto.getRandomValues(bytes);
  let out = "";
  for (const b of bytes) out += b.toString(16).padStart(2, "0");
  return prefix ? `${prefix}_${out}` : out;
}

const JSON_HEADERS = { "content-type": "application/json; charset=utf-8" };

export function json(data: unknown, init: ResponseInit = {}): Response {
  return new Response(JSON.stringify(data), {
    ...init,
    headers: { ...JSON_HEADERS, ...(init.headers ?? {}) },
  });
}

export function ok(data: Record<string, unknown> = {}): Response {
  return json({ ok: true, ...data });
}

export function error(status: number, code: string, message?: string): Response {
  return json({ ok: false, error: code, message: message ?? code }, { status });
}

export const badRequest = (m?: string) => error(400, "bad_request", m);
export const unauthorized = (m?: string) => error(401, "unauthorized", m);
export const forbidden = (m?: string) => error(403, "forbidden", m);
export const notFound = (m?: string) => error(404, "not_found", m);
export const tooMany = (m?: string) => error(429, "rate_limited", m);
export const serverError = (m?: string) => error(500, "server_error", m);

/** Parse a JSON body defensively; returns null on any failure. */
export async function readJson<T = unknown>(req: Request): Promise<T | null> {
  try {
    return (await req.json()) as T;
  } catch {
    return null;
  }
}
