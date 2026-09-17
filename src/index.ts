/**
 * Router entry. One origin serves both the API (/api/*, /admin/*) and static
 * assets (everything else, via the ASSETS binding). No CORS — same origin.
 */
import type { Ctx, Env, Handler, RouteDef } from "./types";
import { enforceSameOrigin, securityHeaders } from "./lib/cors";
import { notFound, ok, serverError } from "./lib/json";
import { resolveSession } from "./lib/session";
import { pingDb } from "./db/queries";
import { authRoutes } from "./routes/auth";
import { quizRoutes } from "./routes/quiz";
import { contentRoutes } from "./routes/content";
import { toolsRoutes } from "./routes/tools";
import { upscRoutes } from "./routes/upsc";
import { adminRoutes } from "./routes/admin";
import { researchRoutes } from "./routes/research";
import { advocateRoutes } from "./routes/advocate";
import { runScheduled } from "./scheduled";

/** Pretty paths → static HTML files in /public. */
const PAGES: Record<string, string> = {
  "/": "/index.html",
  "/ask": "/ask.html",
  "/ask-summaverick": "/ask.html",
  "/quiz": "/quiz.html",
  "/research": "/research.html",
  "/learn": "/learn.html",
  "/interviews": "/interviews.html",
  "/search": "/search.html",
  "/flights": "/flights.html",
  "/metals": "/metals.html",
  "/upsc": "/upsc.html",
  "/advocate": "/advocate.html",
  "/signin": "/signin.html",
  "/tools": "/tools.html",
};

function route(method: string, pattern: string, handler: Handler): RouteDef {
  return { method, pattern, handler };
}

// ---- Route table ----------------------------------------------------------
const ROUTES: RouteDef[] = [
  // /api/health is handled directly in fetch() before session resolution.
  ...authRoutes(route),
  ...quizRoutes(route),
  ...contentRoutes(route),
  ...toolsRoutes(route),
  ...upscRoutes(route),
  ...adminRoutes(route),
  ...researchRoutes(route),
  ...advocateRoutes(route),
];

/** Match a path against a pattern, capturing :params. */
function match(
  pattern: string,
  path: string
): Record<string, string> | null {
  const pp = pattern.split("/");
  const ap = path.split("/");
  if (pp.length !== ap.length) return null;
  const params: Record<string, string> = {};
  for (let i = 0; i < pp.length; i++) {
    const seg = pp[i]!;
    const val = ap[i]!;
    if (seg.startsWith(":")) {
      params[seg.slice(1)] = decodeURIComponent(val);
    } else if (seg !== val) {
      return null;
    }
  }
  return params;
}

function withHeaders(res: Response, setCookies: string[]): Response {
  const headers = new Headers(res.headers);
  for (const [k, v] of Object.entries(securityHeaders())) headers.set(k, v);
  for (const c of setCookies) headers.append("set-cookie", c);
  return new Response(res.body, {
    status: res.status,
    statusText: res.statusText,
    headers,
  });
}

export default {
  async fetch(
    req: Request,
    env: Env,
    exec: ExecutionContext
  ): Promise<Response> {
    const url = new URL(req.url);
    const isApi =
      url.pathname.startsWith("/api/") || url.pathname.startsWith("/admin/");

    if (!isApi) {
      const path = url.pathname.replace(/\/+$/, "") || "/";
      if (path.startsWith("/article/") && path.length > "/article/".length) {
        // The library builder pre-renders every essay and interview to
        // /article/<slug>.html. Serve the static page when it exists so an
        // article never depends on the D1/R2 seed; fall back to the
        // API-driven article.html for anything added after the last build.
        const slug = path.slice("/article/".length).split("/")[0] ?? "";
        if (slug && !slug.includes(".")) {
          const staticRes = await env.ASSETS.fetch(
            new Request(new URL(`/article/${slug}.html`, url.origin), req)
          );
          if (staticRes.status !== 404) return staticRes;
        }
        const assetUrl = new URL("/article.html", url.origin);
        return env.ASSETS.fetch(new Request(assetUrl, req));
      }
      const asset = PAGES[path];
      if (asset) {
        const assetUrl = new URL(asset, url.origin);
        return env.ASSETS.fetch(new Request(assetUrl, req));
      }
      return env.ASSETS.fetch(req);
    }

    // Health check must not depend on session/app tables (it runs before any
    // migration during bring-up) — answer it directly.
    if (url.pathname === "/api/health" && req.method === "GET") {
      let db = "unknown";
      try {
        db = (await pingDb(env.DB)) ? "ok" : "error";
      } catch {
        db = "error";
      }
      return withHeaders(ok({ ts: Date.now(), db, env: env.ENVIRONMENT }), []);
    }

    // Same-origin guard for mutations (CSRF defence, not CORS).
    const originBlock = enforceSameOrigin(req, env);
    if (originBlock) return originBlock;

    let setCookies: string[] = [];
    try {
      const { info, setCookies: sc } = await resolveSession(req, env);
      setCookies = sc;

      for (const r of ROUTES) {
        if (r.method !== req.method) continue;
        const params = match(r.pattern, url.pathname);
        if (!params) continue;
        const ctx: Ctx = { env, exec, url, session: info, params };
        const res = await r.handler(req, ctx);
        return withHeaders(res, setCookies);
      }
      return withHeaders(notFound("no such route"), setCookies);
    } catch (err) {
      console.error("unhandled error", err);
      return withHeaders(serverError(), setCookies);
    }
  },

  async scheduled(
    event: ScheduledController,
    env: Env,
    exec: ExecutionContext
  ): Promise<void> {
    exec.waitUntil(runScheduled(event.cron, env));
  },
};
