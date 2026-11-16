/**
 * Upstream answer service for /ask-summaverick when this Worker has no
 * PERPLEXITY_API_KEY: the sumanthbolle.github.io Worker, which takes the same
 * request bodies and returns the same `{ success, result }` / trending shapes.
 * Set `ASK_FALLBACK_URL` to "" to turn it off.
 */
import type { Env } from "../types";

export function askFallbackUrl(env: Pick<Env, "ASK_FALLBACK_URL">, path = "/"): string | null {
  const base = (env.ASK_FALLBACK_URL || "").trim();
  if (!base) return null;
  try {
    const url = new URL(base);
    if (url.protocol !== "https:") return null;
    url.pathname = path;
    url.search = "";
    return url.toString();
  } catch {
    return null;
  }
}

/** Fetches JSON from the fallback service; null on any failure or non-success body. */
export async function fetchFallback<T extends { success?: unknown }>(
  url: string,
  init: RequestInit,
  timeoutMs: number,
): Promise<T | null> {
  const ctrl = new AbortController();
  const timer = setTimeout(() => ctrl.abort(), timeoutMs);
  try {
    const res = await fetch(url, { ...init, signal: ctrl.signal });
    if (!res.ok) return null;
    const data = (await res.json()) as T;
    return data && data.success === true ? data : null;
  } catch {
    return null;
  } finally {
    clearTimeout(timer);
  }
}
