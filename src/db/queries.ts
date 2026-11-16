/**
 * The ONE place raw SQL is allowed. Route handlers and lib code import named
 * functions from here and never write SQL themselves. Keeping every statement
 * in one file is what makes schema changes (Phase 3+) survivable.
 *
 * All time values are epoch-ms integers. Callers pass numbers, get numbers.
 */
import type {
  ContentRow,
  CredentialRow,
  LeadRow,
  MagicLinkRow,
  ProgressRow,
  PublishRunRow,
  SessionRow,
  UpscNoteRow,
  UpscReviewRow,
  UpscSourceRow,
  UserRow,
} from "./schema";

// ---------------------------------------------------------------------------
// Sessions & identity
// ---------------------------------------------------------------------------

export async function createSession(
  db: D1Database,
  s: SessionRow
): Promise<void> {
  await db
    .prepare(
      `INSERT INTO sessions (id, user_id, device_id, created_at, expires_at, ua_hash)
       VALUES (?, ?, ?, ?, ?, ?)`
    )
    .bind(s.id, s.user_id, s.device_id, s.created_at, s.expires_at, s.ua_hash)
    .run();
}

export async function getSession(
  db: D1Database,
  id: string
): Promise<SessionRow | null> {
  return db
    .prepare(`SELECT * FROM sessions WHERE id = ?`)
    .bind(id)
    .first<SessionRow>();
}

export async function extendSession(
  db: D1Database,
  id: string,
  expiresAt: number
): Promise<void> {
  await db
    .prepare(`UPDATE sessions SET expires_at = ? WHERE id = ?`)
    .bind(expiresAt, id)
    .run();
}

export async function attachUserToSession(
  db: D1Database,
  sessionId: string,
  userId: string
): Promise<void> {
  await db
    .prepare(`UPDATE sessions SET user_id = ? WHERE id = ?`)
    .bind(userId, sessionId)
    .run();
}

export async function deleteSession(db: D1Database, id: string): Promise<void> {
  await db.prepare(`DELETE FROM sessions WHERE id = ?`).bind(id).run();
}

export async function deleteExpiredSessions(
  db: D1Database,
  now: number
): Promise<number> {
  const r = await db
    .prepare(`DELETE FROM sessions WHERE expires_at < ?`)
    .bind(now)
    .run();
  return r.meta.changes ?? 0;
}

/** Migrate anonymous attempts + events from a device to a newly-known user. */
export async function claimDeviceForUser(
  db: D1Database,
  deviceId: string,
  userId: string
): Promise<void> {
  await db.batch([
    db
      .prepare(
        `UPDATE attempts SET user_id = ? WHERE device_id = ? AND user_id IS NULL`
      )
      .bind(userId, deviceId),
    db
      .prepare(
        `UPDATE events SET user_id = ? WHERE device_id = ? AND user_id IS NULL`
      )
      .bind(userId, deviceId),
  ]);
  // responses are linked to attempts via attempt_id, so they follow automatically.
}

export async function getUserById(
  db: D1Database,
  id: string
): Promise<UserRow | null> {
  return db.prepare(`SELECT * FROM users WHERE id = ?`).bind(id).first<UserRow>();
}

export async function getUserByEmail(
  db: D1Database,
  email: string
): Promise<UserRow | null> {
  return db
    .prepare(`SELECT * FROM users WHERE email = ?`)
    .bind(email)
    .first<UserRow>();
}

export async function createUser(db: D1Database, u: UserRow): Promise<void> {
  await db
    .prepare(
      `INSERT INTO users (id, email, display_name, created_at, last_seen_at)
       VALUES (?, ?, ?, ?, ?)`
    )
    .bind(u.id, u.email, u.display_name, u.created_at, u.last_seen_at)
    .run();
}

export async function touchUser(
  db: D1Database,
  id: string,
  now: number
): Promise<void> {
  await db
    .prepare(`UPDATE users SET last_seen_at = ? WHERE id = ?`)
    .bind(now, id)
    .run();
}

// ---------------------------------------------------------------------------
// WebAuthn credentials
// ---------------------------------------------------------------------------

export async function insertCredential(
  db: D1Database,
  c: CredentialRow
): Promise<void> {
  await db
    .prepare(
      `INSERT INTO credentials (id, user_id, public_key, sign_count, transports, created_at)
       VALUES (?, ?, ?, ?, ?, ?)`
    )
    .bind(c.id, c.user_id, c.public_key, c.sign_count, c.transports, c.created_at)
    .run();
}

export async function getCredentialById(
  db: D1Database,
  id: string
): Promise<CredentialRow | null> {
  return db
    .prepare(`SELECT * FROM credentials WHERE id = ?`)
    .bind(id)
    .first<CredentialRow>();
}

export async function getCredentialsByUser(
  db: D1Database,
  userId: string
): Promise<CredentialRow[]> {
  const r = await db
    .prepare(`SELECT * FROM credentials WHERE user_id = ?`)
    .bind(userId)
    .all<CredentialRow>();
  return r.results ?? [];
}

export async function updateSignCount(
  db: D1Database,
  id: string,
  signCount: number
): Promise<void> {
  await db
    .prepare(`UPDATE credentials SET sign_count = ? WHERE id = ?`)
    .bind(signCount, id)
    .run();
}

// ---------------------------------------------------------------------------
// Magic links
// ---------------------------------------------------------------------------

export async function insertMagicLink(
  db: D1Database,
  m: MagicLinkRow
): Promise<void> {
  await db
    .prepare(
      `INSERT INTO magic_links (token_hash, email, expires_at, consumed_at)
       VALUES (?, ?, ?, ?)`
    )
    .bind(m.token_hash, m.email, m.expires_at, m.consumed_at)
    .run();
}

export async function getMagicLink(
  db: D1Database,
  tokenHash: string
): Promise<MagicLinkRow | null> {
  return db
    .prepare(`SELECT * FROM magic_links WHERE token_hash = ?`)
    .bind(tokenHash)
    .first<MagicLinkRow>();
}

/** Atomically consume a link: marks consumed only if not already consumed. */
export async function consumeMagicLink(
  db: D1Database,
  tokenHash: string,
  now: number
): Promise<boolean> {
  const r = await db
    .prepare(
      `UPDATE magic_links SET consumed_at = ?
       WHERE token_hash = ? AND consumed_at IS NULL AND expires_at > ?`
    )
    .bind(now, tokenHash, now)
    .run();
  return (r.meta.changes ?? 0) > 0;
}

// ---------------------------------------------------------------------------
// Events (telemetry) — batched writes only
// ---------------------------------------------------------------------------

export async function insertEvents(
  db: D1Database,
  events: Array<{
    id: string;
    user_id: string | null;
    device_id: string | null;
    name: string;
    props_json: string | null;
    ts: number;
  }>
): Promise<void> {
  if (events.length === 0) return;
  const stmt = db.prepare(
    `INSERT INTO events (id, user_id, device_id, name, props_json, ts)
     VALUES (?, ?, ?, ?, ?, ?)`
  );
  await db.batch(
    events.map((e) =>
      stmt.bind(e.id, e.user_id, e.device_id, e.name, e.props_json, e.ts)
    )
  );
}

// ---------------------------------------------------------------------------
// Content + FTS + progress
// ---------------------------------------------------------------------------

export async function listContent(
  db: D1Database,
  opts: { kind?: string; category?: string; limit: number; offset: number }
): Promise<ContentRow[]> {
  let sql = `SELECT * FROM content WHERE status = 'published'`;
  const binds: unknown[] = [];
  if (opts.kind) {
    sql += ` AND kind = ?`;
    binds.push(opts.kind);
  }
  if (opts.category) {
    sql += ` AND category = ?`;
    binds.push(opts.category);
  }
  sql += ` ORDER BY published_at DESC LIMIT ? OFFSET ?`;
  binds.push(opts.limit, opts.offset);
  const r = await db
    .prepare(sql)
    .bind(...binds)
    .all<ContentRow>();
  return r.results ?? [];
}

export async function getContentBySlug(
  db: D1Database,
  kind: string,
  slug: string
): Promise<ContentRow | null> {
  return db
    .prepare(`SELECT * FROM content WHERE kind = ? AND slug = ?`)
    .bind(kind, slug)
    .first<ContentRow>();
}

export async function getContentBySlugAny(
  db: D1Database,
  slug: string
): Promise<ContentRow | null> {
  return db
    .prepare(`SELECT * FROM content WHERE slug = ? LIMIT 1`)
    .bind(slug)
    .first<ContentRow>();
}

export async function upsertProgress(
  db: D1Database,
  p: ProgressRow
): Promise<void> {
  await db
    .prepare(
      `INSERT INTO progress (user_id, content_id, state, pct, updated_at)
       VALUES (?, ?, ?, ?, ?)
       ON CONFLICT(user_id, content_id)
       DO UPDATE SET state = excluded.state, pct = excluded.pct, updated_at = excluded.updated_at`
    )
    .bind(p.user_id, p.content_id, p.state, p.pct, p.updated_at)
    .run();
}

// ---------------------------------------------------------------------------
// UPSC pipeline
// ---------------------------------------------------------------------------

export async function upsertUpscSource(
  db: D1Database,
  s: UpscSourceRow
): Promise<void> {
  await db
    .prepare(
      `INSERT INTO upsc_sources
         (id, url, host, title, content_hash, official_summary, source_verified, fetched_at, health)
       VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)
       ON CONFLICT(id) DO UPDATE SET
         url = excluded.url, host = excluded.host, title = excluded.title,
         content_hash = excluded.content_hash, official_summary = excluded.official_summary,
         source_verified = excluded.source_verified, fetched_at = excluded.fetched_at,
         health = excluded.health`
    )
    .bind(
      s.id,
      s.url,
      s.host,
      s.title,
      s.content_hash,
      s.official_summary,
      s.source_verified,
      s.fetched_at,
      s.health
    )
    .run();
}

export async function getUpscSource(
  db: D1Database,
  id: string
): Promise<UpscSourceRow | null> {
  return db
    .prepare(`SELECT * FROM upsc_sources WHERE id = ?`)
    .bind(id)
    .first<UpscSourceRow>();
}

export async function insertUpscNote(
  db: D1Database,
  n: UpscNoteRow
): Promise<void> {
  await db
    .prepare(
      `INSERT INTO upsc_notes
         (id, source_id, content_hash, status, paper, anchor, syllabus_json, score, band, payload_json, published_at, created_at)
       VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
       ON CONFLICT(id) DO UPDATE SET
         source_id = excluded.source_id, content_hash = excluded.content_hash,
         status = excluded.status, paper = excluded.paper, anchor = excluded.anchor,
         syllabus_json = excluded.syllabus_json, score = excluded.score,
         band = excluded.band, payload_json = excluded.payload_json,
         published_at = excluded.published_at`
    )
    .bind(
      n.id,
      n.source_id,
      n.content_hash,
      n.status,
      n.paper,
      n.anchor,
      n.syllabus_json,
      n.score,
      n.band,
      n.payload_json,
      n.published_at,
      n.created_at
    )
    .run();
}

export async function listPublishedNotes(
  db: D1Database,
  limit = 100
): Promise<UpscNoteRow[]> {
  const r = await db
    .prepare(
      `SELECT * FROM upsc_notes WHERE status = 'published'
        ORDER BY published_at DESC LIMIT ?`
    )
    .bind(limit)
    .all<UpscNoteRow>();
  return r.results ?? [];
}

export async function countPublishedNotes(db: D1Database): Promise<number> {
  const r = await db
    .prepare(`SELECT COUNT(*) AS n FROM upsc_notes WHERE status = 'published'`)
    .first<{ n: number }>();
  return r?.n ?? 0;
}

export async function insertUpscReview(
  db: D1Database,
  r: UpscReviewRow
): Promise<void> {
  await db
    .prepare(
      `INSERT INTO upsc_review (id, note_id, reason, flagged_json, created_at, resolved_at, resolution)
       VALUES (?, ?, ?, ?, ?, ?, ?)`
    )
    .bind(
      r.id,
      r.note_id,
      r.reason,
      r.flagged_json,
      r.created_at,
      r.resolved_at,
      r.resolution
    )
    .run();
}

export async function listOpenReviews(
  db: D1Database,
  limit = 100
): Promise<UpscReviewRow[]> {
  const r = await db
    .prepare(
      `SELECT * FROM upsc_review WHERE resolved_at IS NULL
        ORDER BY created_at DESC LIMIT ?`
    )
    .bind(limit)
    .all<UpscReviewRow>();
  return r.results ?? [];
}

export async function resolveReview(
  db: D1Database,
  id: string,
  now: number,
  resolution: string
): Promise<boolean> {
  const r = await db
    .prepare(
      `UPDATE upsc_review SET resolved_at = ?, resolution = ?
        WHERE id = ? AND resolved_at IS NULL`
    )
    .bind(now, resolution, id)
    .run();
  return (r.meta.changes ?? 0) > 0;
}

export async function openPublishRun(
  db: D1Database,
  r: PublishRunRow
): Promise<void> {
  await db
    .prepare(
      `INSERT INTO publish_runs (id, kind, started_at, finished_at, ok, stats_json)
       VALUES (?, ?, ?, ?, ?, ?)`
    )
    .bind(r.id, r.kind, r.started_at, r.finished_at, r.ok, r.stats_json)
    .run();
}

export async function closePublishRun(
  db: D1Database,
  id: string,
  finishedAt: number,
  ok: number,
  statsJson: string
): Promise<void> {
  await db
    .prepare(
      `UPDATE publish_runs SET finished_at = ?, ok = ?, stats_json = ? WHERE id = ?`
    )
    .bind(finishedAt, ok, statsJson, id)
    .run();
}

/** Notes published before `cutoff` — candidates for archival + deletion. */
export async function notesOlderThan(
  db: D1Database,
  cutoff: number,
  limit = 500
): Promise<UpscNoteRow[]> {
  const r = await db
    .prepare(
      `SELECT * FROM upsc_notes
        WHERE status = 'published' AND published_at IS NOT NULL AND published_at < ?
        ORDER BY published_at ASC LIMIT ?`
    )
    .bind(cutoff, limit)
    .all<UpscNoteRow>();
  return r.results ?? [];
}

export async function deleteNotesByIds(
  db: D1Database,
  ids: string[]
): Promise<number> {
  if (ids.length === 0) return 0;
  const placeholders = ids.map(() => "?").join(",");
  const r = await db
    .prepare(`DELETE FROM upsc_notes WHERE id IN (${placeholders})`)
    .bind(...ids)
    .run();
  return r.meta.changes ?? 0;
}

// ---------------------------------------------------------------------------
// Contact messages
// ---------------------------------------------------------------------------

/**
 * Store one contact message. Returns the id that now holds this submission: the
 * existing one when the idempotency key has been seen before, so a retry after
 * an uncertain response does not create a second copy.
 */
export async function findLeadByIdempotencyKey(
  db: D1Database,
  key: string
): Promise<{ id: string; notified: number } | null> {
  return await db
    .prepare(`SELECT id, notified FROM leads WHERE idempotency_key = ?`)
    .bind(key)
    .first<{ id: string; notified: number }>();
}

export async function insertLead(
  db: D1Database,
  lead: LeadRow
): Promise<{ id: string; duplicate: boolean }> {
  if (lead.idempotency_key) {
    const existing = await findLeadByIdempotencyKey(db, lead.idempotency_key);
    if (existing) return { id: existing.id, duplicate: true };
  }
  await db
    .prepare(
      `INSERT INTO leads
         (id, idempotency_key, name, email, organisation, intent, message, notified, created_at)
       VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)`
    )
    .bind(
      lead.id,
      lead.idempotency_key,
      lead.name,
      lead.email,
      lead.organisation,
      lead.intent,
      lead.message,
      lead.notified,
      lead.created_at
    )
    .run();
  return { id: lead.id, duplicate: false };
}

export async function markLeadNotified(
  db: D1Database,
  id: string
): Promise<void> {
  await db.prepare(`UPDATE leads SET notified = 1 WHERE id = ?`).bind(id).run();
}

// ---------------------------------------------------------------------------
// Health
// ---------------------------------------------------------------------------

export async function pingDb(db: D1Database): Promise<boolean> {
  const r = await db.prepare(`SELECT 1 AS ok`).first<{ ok: number }>();
  return r?.ok === 1;
}
