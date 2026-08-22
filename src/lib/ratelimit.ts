/**
 * Small KV-backed fixed-window rate limiter. Eventual consistency is acceptable
 * here — this protects auth + write endpoints from abuse, it is not a billing
 * meter. Keys are namespaced under `rl:` and expire automatically.
 */
import type { Env } from "../types";
import { nowMs } from "./json";

export interface RateLimitResult {
  allowed: boolean;
  remaining: number;
  resetAt: number;
}

export async function rateLimit(
  env: Env,
  key: string,
  limit: number,
  windowSeconds: number
): Promise<RateLimitResult> {
  const now = nowMs();
  const windowMs = windowSeconds * 1000;
  const windowStart = Math.floor(now / windowMs) * windowMs;
  const resetAt = windowStart + windowMs;
  const kvKey = `rl:${key}:${windowStart}`;

  const current = Number((await env.CONFIG.get(kvKey)) ?? "0");
  if (current >= limit) {
    return { allowed: false, remaining: 0, resetAt };
  }
  // Best-effort increment. A race can undercount slightly; acceptable for abuse
  // protection. TTL padded by one window so the key always outlives its window.
  await env.CONFIG.put(kvKey, String(current + 1), {
    expirationTtl: windowSeconds * 2,
  });
  return { allowed: true, remaining: Math.max(0, limit - current - 1), resetAt };
}

/** Client key derived from CF-Connecting-IP, falling back to a device id. */
export function clientKey(req: Request, deviceId: string): string {
  return req.headers.get("cf-connecting-ip") ?? `dev:${deviceId}`;
}
