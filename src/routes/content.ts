/**
 * Content API (T8): blog posts, interviews, tutorials.
 *   GET /api/content?kind=&category=&page=   feed (metadata only)
 *   GET /api/content/:slug                   single item + body from R2
 *   GET /api/search?q=                       FTS5 bm25 ranked hits + snippets
 */
import type { RouteDef, RouteMaker } from "../types";
import {
  getContentBySlugAny,
  listContent,
  searchContent,
} from "../db/queries";
import { badRequest, json, notFound, ok } from "../lib/json";

const PAGE_SIZE = 20;

function meta(c: {
  id: string;
  kind: string;
  slug: string;
  title: string;
  excerpt: string | null;
  category: string | null;
  learning_path: string | null;
  difficulty: string | null;
  personas_json: string | null;
  company: string | null;
  read_time: string | null;
  published_at: number | null;
}) {
  return {
    id: c.id,
    kind: c.kind,
    slug: c.slug,
    title: c.title,
    excerpt: c.excerpt,
    category: c.category,
    learningPath: c.learning_path,
    difficulty: c.difficulty,
    personas: c.personas_json ? safeArr(c.personas_json) : null,
    company: c.company,
    readTime: c.read_time,
    publishedAt: c.published_at,
  };
}

export function contentRoutes(route: RouteMaker): RouteDef[] {
  return [
    route("GET", "/api/content", async (_req, ctx) => {
      const kind = ctx.url.searchParams.get("kind") ?? undefined;
      const category = ctx.url.searchParams.get("category") ?? undefined;
      const page = Math.max(
        0,
        parseInt(ctx.url.searchParams.get("page") ?? "0", 10) || 0
      );
      const rows = await listContent(ctx.env.DB, {
        kind,
        category,
        limit: PAGE_SIZE,
        offset: page * PAGE_SIZE,
      });
      return ok({
        page,
        pageSize: PAGE_SIZE,
        items: rows.map(meta),
      });
    }),

    route("GET", "/api/search", async (_req, ctx) => {
      const q = (ctx.url.searchParams.get("q") ?? "").trim();
      if (q.length < 2) return badRequest("q must be at least 2 characters");
      // FTS5 MATCH: quote the term to tolerate punctuation, keep it a phrase/prefix.
      const safe = q.replace(/"/g, '""');
      const hits = await searchContent(ctx.env.DB, `"${safe}"*`);
      return ok({
        query: q,
        hits: hits.map((h) => ({
          contentId: h.content_id,
          kind: h.kind,
          slug: h.slug,
          title: h.title,
          snippet: h.snippet,
          rank: h.rank,
        })),
      });
    }),

    route("GET", "/api/content/:slug", async (_req, ctx) => {
      const row = await getContentBySlugAny(ctx.env.DB, ctx.params.slug!);
      if (!row) return notFound("no such content");
      const obj = await ctx.env.BODIES.get(row.body_r2_key);
      const body = obj ? await obj.text() : null;
      return json({ ok: true, item: { ...meta(row), body } });
    }),
  ];
}

function safeArr(s: string): unknown[] {
  try {
    const v = JSON.parse(s);
    return Array.isArray(v) ? v : [];
  } catch {
    return [];
  }
}
