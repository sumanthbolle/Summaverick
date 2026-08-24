/**
 * UPSC pipeline off git (T10).
 *
 * The GitHub Actions workflows keep orchestrating; they stop committing JSON to
 * the old repo and instead POST to these authenticated routes. Storage moves to
 * D1 (+ R2 for retention archives).
 *
 * Preserved invariants (from api/README.md), enforced HERE server-side:
 *  - source_id / sourceUrl / content_hash are copied from the STORED source
 *    record; model/payload output can never replace them.
 *  - a fact is source-backed ONLY when its locator is exactly `officialSummary`
 *    AND the fact text occurs in that summary.
 *  - unsupported facts force the note to `draft` and open a needs-review row.
 *  - a content_hash that no longer matches the source invalidates the note and
 *    forces `draft`.
 */
import type { Env, RouteDef, RouteMaker } from "../types";
import {
  closePublishRun,
  countPublishedNotes,
  getUpscSource,
  insertUpscNote,
  insertUpscReview,
  listPublishedNotes,
  openPublishRun,
  upsertUpscSource,
} from "../db/queries";
import { timingSafeEqual } from "../lib/crypto";
import {
  badRequest,
  error,
  json,
  newId,
  nowMs,
  ok,
  readJson,
} from "../lib/json";

/** Bearer check shared by all publisher routes. */
function requirePublishToken(req: Request, env: Env): Response | null {
  if (!env.UPSC_PUBLISH_TOKEN) {
    return error(503, "unavailable", "publish token not configured");
  }
  const header = req.headers.get("authorization") ?? "";
  const token = header.startsWith("Bearer ") ? header.slice(7) : "";
  if (!token || !timingSafeEqual(token, env.UPSC_PUBLISH_TOKEN)) {
    return error(401, "unauthorized", "invalid publish token");
  }
  return null;
}

function hostOf(url: string): string {
  try {
    return new URL(url).hostname;
  } catch {
    return "";
  }
}

function normalize(s: string): string {
  return s.toLowerCase().replace(/\s+/g, " ").trim();
}

interface SourceRecord {
  id: string;
  url: string;
  host?: string;
  title?: string;
  contentHash: string;
  officialSummary?: string;
  sourceVerified?: boolean;
  health?: string;
}

interface Fact {
  text: string;
  locator?: string;
}

interface NoteRecord {
  id?: string;
  sourceId: string;
  contentHash: string;
  status?: string;
  paper?: string;
  anchor?: string;
  syllabus?: unknown;
  score?: number;
  band?: string;
  facts?: Fact[];
  payload?: unknown;
}

export function upscRoutes(route: RouteMaker): RouteDef[] {
  return [
    // ---- upsert normalized source records (bearer) ----
    route("POST", "/api/upsc/sources", async (req, ctx) => {
      const blocked = requirePublishToken(req, ctx.env);
      if (blocked) return blocked;

      const body = await readJson<SourceRecord | SourceRecord[]>(req);
      if (!body) return badRequest("missing source record(s)");
      const records = Array.isArray(body) ? body : [body];
      const now = nowMs();
      let upserted = 0;
      for (const r of records) {
        if (!r.id || !r.url || !r.contentHash) {
          return badRequest("each source needs id, url, contentHash");
        }
        await upsertUpscSource(ctx.env.DB, {
          id: r.id,
          url: r.url,
          host: r.host || hostOf(r.url),
          title: r.title ?? null,
          content_hash: r.contentHash,
          official_summary: r.officialSummary ?? null,
          source_verified: r.sourceVerified ? 1 : 0,
          fetched_at: now,
          health: r.health ?? null,
        });
        upserted++;
      }
      return ok({ upserted });
    }),

    // ---- write an enriched note (bearer) ----
    route("POST", "/api/upsc/notes", async (req, ctx) => {
      const blocked = requirePublishToken(req, ctx.env);
      if (blocked) return blocked;

      const body = await readJson<NoteRecord>(req);
      if (!body?.sourceId || !body?.contentHash) {
        return badRequest("note needs sourceId and contentHash");
      }
      const source = await getUpscSource(ctx.env.DB, body.sourceId);
      if (!source) return error(422, "unknown_source", "no such source");

      const now = nowMs();
      const noteId = body.id ?? newId("note");

      // Invariant: the note's source identity is COPIED from the stored source.
      // Anything in the payload claiming otherwise is ignored.
      const sourceId = source.id;
      const sourceContentHash = source.content_hash;

      // content_hash mismatch invalidates the note -> forced draft + review.
      const hashMismatch = body.contentHash !== sourceContentHash;

      // Evidence gate: a fact is source-backed only when locator ===
      // 'officialSummary' AND its text occurs in that summary.
      const summary = normalize(source.official_summary ?? "");
      const facts = Array.isArray(body.facts) ? body.facts : [];
      const flagged = facts.filter((f) => {
        const backed =
          f.locator === "officialSummary" &&
          summary.length > 0 &&
          summary.includes(normalize(f.text ?? ""));
        return !backed;
      });

      const invalid = hashMismatch || flagged.length > 0;
      let status = body.status === "published" ? "published" : "draft";
      if (invalid) status = flagged.length > 0 ? "needs-review" : "draft";
      const publishedAt = status === "published" ? now : null;

      await insertUpscNote(ctx.env.DB, {
        id: noteId,
        source_id: sourceId,
        content_hash: sourceContentHash, // copied, not from model output
        status,
        paper: body.paper ?? null,
        anchor: body.anchor ?? null,
        syllabus_json: body.syllabus ? JSON.stringify(body.syllabus) : null,
        score: typeof body.score === "number" ? body.score : null,
        band: body.band ?? null,
        payload_json: JSON.stringify(body.payload ?? {}),
        published_at: publishedAt,
        created_at: now,
      });

      if (invalid) {
        await insertUpscReview(ctx.env.DB, {
          id: newId("rev"),
          note_id: noteId,
          reason: hashMismatch
            ? "content_hash mismatch with source"
            : "unsupported facts (locator != officialSummary)",
          flagged_json: JSON.stringify({
            hashMismatch,
            flaggedFacts: flagged.map((f) => f.text),
          }),
          created_at: now,
          resolved_at: null,
          resolution: null,
        });
      }

      return json({
        ok: true,
        note: { id: noteId, status, flagged: flagged.length, hashMismatch },
      });
    }),

    // ---- open/close a publish run (bearer) ----
    route("POST", "/api/upsc/runs", async (req, ctx) => {
      const blocked = requirePublishToken(req, ctx.env);
      if (blocked) return blocked;

      const body = await readJson<{
        id?: string;
        kind?: string;
        action?: "open" | "close";
        ok?: boolean;
        stats?: unknown;
      }>(req);
      if (!body?.action) return badRequest("action open|close required");
      const now = nowMs();

      if (body.action === "open") {
        const id = body.id ?? newId("run");
        await openPublishRun(ctx.env.DB, {
          id,
          kind: body.kind ?? "daily",
          started_at: now,
          finished_at: null,
          ok: null,
          stats_json: null,
        });
        return ok({ runId: id });
      }
      if (!body.id) return badRequest("close requires run id");
      await closePublishRun(
        ctx.env.DB,
        body.id,
        now,
        body.ok ? 1 : 0,
        JSON.stringify(body.stats ?? {})
      );
      return ok({ runId: body.id, closed: true });
    }),

    // ---- public feed of published notes ----
    route("GET", "/api/upsc/feed", async (_req, ctx) => {
      const scope = ctx.url.searchParams.get("scope") ?? "daily";
      try {
        const notes = await listPublishedNotes(ctx.env.DB, 200);
        return json({
          ok: true,
          scope,
          count: notes.length,
          notes: notes.map((n) => ({
            id: n.id,
            paper: n.paper,
            anchor: n.anchor,
            band: n.band,
            score: n.score,
            publishedAt: n.published_at,
            payload: safeParse(n.payload_json),
          })),
        });
      } catch (err) {
        console.error("upsc feed", err);
        return json(
          {
            ok: false,
            error: "unavailable",
            message: "UPSC feed is still being set up.",
            scope,
            count: 0,
            notes: [],
          },
          { status: 503 }
        );
      }
    }),

    // ---- lightweight publish-health summary ----
    route("GET", "/api/upsc/stats", async (_req, ctx) => {
      const published = await countPublishedNotes(ctx.env.DB);
      return ok({ published });
    }),
  ];
}

function safeParse(s: string): unknown {
  try {
    return JSON.parse(s);
  } catch {
    return null;
  }
}
