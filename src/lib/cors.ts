/**
 * There is intentionally NO CORS here.
 *
 * summaverick serves HTML and API from one origin (Worker + Static Assets), so
 * cross-origin requests are never expected and `Access-Control-Allow-Origin`
 * would only widen the attack surface. The old `ALLOWED_ORIGIN` secret is gone
 * on purpose. If you ever feel the need to add CORS, that is the signal an
 * architecture mistake was made — reach for same-origin instead.
 *
 * What we DO keep is a same-origin guard for state-changing requests. This is a
 * CSRF defence (checking the request came from our own page), not CORS — it
 * emits no ACAO headers and grants no cross-origin access.
 */
import type { Env } from "../types";
import { forbidden } from "./json";

const SAFE_METHODS = new Set(["GET", "HEAD", "OPTIONS"]);

/**
 * Returns a 403 Response if a mutating request did not originate from our own
 * origin, otherwise null. Relies on the Origin header (sent by browsers on all
 * non-GET cross-origin requests and same-origin POSTs).
 */
export function enforceSameOrigin(req: Request, env: Env): Response | null {
  if (SAFE_METHODS.has(req.method)) return null;

  const origin = req.headers.get("origin");
  // Server-to-server callers (GitHub Actions -> /api/upsc/*) send no Origin and
  // authenticate with a bearer token instead; those routes check the token.
  if (!origin) return null;

  const allowed = new Set<string>([env.PUBLIC_ORIGIN]);
  // Accept the request's own host too (covers custom domain + localhost dev).
  try {
    allowed.add(new URL(req.url).origin);
  } catch {
    /* ignore */
  }
  return allowed.has(origin) ? null : forbidden("cross-origin request rejected");
}

/** Baseline security headers applied to every response. */
export function securityHeaders(): Record<string, string> {
  return {
    "x-content-type-options": "nosniff",
    "referrer-policy": "strict-origin-when-cross-origin",
    "x-frame-options": "DENY",
  };
}
