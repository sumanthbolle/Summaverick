/// <reference types="@cloudflare/workers-types" />

/**
 * Bindings and secrets available on the Worker. Mirrors wrangler.jsonc plus the
 * secrets set via `wrangler secret put`. Secrets are optional at the type level
 * because local dev may run without them; routes that need one must check.
 */
export interface Env {
  // Bindings
  ASSETS: Fetcher;
  DB: D1Database;
  CONFIG: KVNamespace;
  BODIES: R2Bucket;

  // Plain vars (wrangler.jsonc `vars`)
  ENVIRONMENT: string;
  PUBLIC_ORIGIN: string;
  MAGIC_LINK_FROM: string;

  // Secrets (wrangler secret put)
  PERPLEXITY_API_KEY?: string;
  UPSC_PUBLISH_TOKEN?: string;
  SESSION_SECRET?: string;
  AMADEUS_CLIENT_ID?: string;
  AMADEUS_CLIENT_SECRET?: string;
  RESEND_API_KEY?: string;
}

/** Per-request context threaded to route handlers. */
export interface Ctx {
  env: Env;
  /** ExecutionContext for waitUntil / passThroughOnException. */
  exec: ExecutionContext;
  url: URL;
  /** Resolved session (anonymous or authenticated) — always present. */
  session: SessionInfo;
  /** Path params captured by the router, e.g. { id: "abc" }. */
  params: Record<string, string>;
}

export interface SessionInfo {
  sessionId: string;
  deviceId: string;
  userId: string | null;
  /** True when a fresh session/device cookie must be written on the response. */
  isNew: boolean;
  expiresAt: number;
}

export type Handler = (req: Request, ctx: Ctx) => Promise<Response> | Response;

export interface RouteDef {
  method: string;
  pattern: string;
  handler: Handler;
}

/** Factory injected into each route module so all routes share one matcher. */
export type RouteMaker = (
  method: string,
  pattern: string,
  handler: Handler
) => RouteDef;
