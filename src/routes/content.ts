/**
 * Content API (T8): published essays. Interview rows may still be in D1 but
 * are no longer part of the site, so only `post` rows are served.
 *   GET /api/content?category=&page=   feed (metadata only)
 *   GET /api/content/:slug             single item + body from R2
 */
import type { RouteDef, RouteMaker } from "../types";
import {
  getContentBySlugAny,
  listContent,
} from "../db/queries";
import { json, notFound, ok } from "../lib/json";

const PUBLISHED_KIND = "post";

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
      const category = ctx.url.searchParams.get("category") ?? undefined;
      const page = Math.max(
        0,
        parseInt(ctx.url.searchParams.get("page") ?? "0", 10) || 0
      );
      try {
        const rows = await listContent(ctx.env.DB, {
          kind: PUBLISHED_KIND,
          category,
          limit: PAGE_SIZE,
          offset: page * PAGE_SIZE,
        });
        return ok({
          page,
          pageSize: PAGE_SIZE,
          items: rows.map(meta),
        });
      } catch (err) {
        console.error("content feed", err);
        return json(
          {
            ok: false,
            error: "unavailable",
            message: "Library is still being set up.",
            page,
            pageSize: PAGE_SIZE,
            items: [],
          },
          { status: 503 }
        );
      }
    }),

    route("GET", "/api/content/:slug", async (_req, ctx) => {
      const row = await getContentBySlugAny(ctx.env.DB, ctx.params.slug!);
      if (!row || row.kind !== PUBLISHED_KIND) return notFound("no such content");
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
