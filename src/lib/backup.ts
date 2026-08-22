/**
 * In-Worker nightly backup (T4). Wrangler is not available inside a scheduled
 * handler, so we dump every table to a single .sql file of INSERT statements and
 * write it to R2 under backups/YYYY-MM-DD.sql. 30-day retention by key date.
 *
 * From Phase 3 onward D1 is the only copy of the UPSC archive — git is no longer
 * the backup — so this must run before any real writes land.
 *
 * A companion CLI (scripts/export-backup.ts) uses `wrangler d1 export` for
 * manual/CI dumps; this runtime path is what the Cron Trigger uses.
 */
import type { Env } from "../types";
import { nowMs } from "./json";

/** Every table we back up. Order chosen so a restore satisfies FKs. */
const TABLES = [
  "users",
  "credentials",
  "sessions",
  "magic_links",
  "quiz_categories",
  "questions",
  "attempts",
  "responses",
  "events",
  "content",
  "progress",
  "upsc_sources",
  "upsc_notes",
  "upsc_review",
  "publish_runs",
  "metals_daily",
] as const;

function sqlLiteral(v: unknown): string {
  if (v === null || v === undefined) return "NULL";
  if (typeof v === "number") return Number.isFinite(v) ? String(v) : "NULL";
  if (typeof v === "bigint") return v.toString();
  if (v instanceof ArrayBuffer) {
    const bytes = new Uint8Array(v);
    let hex = "";
    for (const b of bytes) hex += b.toString(16).padStart(2, "0");
    return `X'${hex}'`;
  }
  return `'${String(v).replace(/'/g, "''")}'`;
}

async function dumpTable(env: Env, table: string): Promise<string> {
  const r = await env.DB.prepare(`SELECT * FROM ${table}`).all<
    Record<string, unknown>
  >();
  const rows = r.results ?? [];
  if (rows.length === 0) return `-- ${table}: 0 rows\n`;
  const cols = Object.keys(rows[0]!);
  const lines = [`-- ${table}: ${rows.length} rows`];
  for (const row of rows) {
    const vals = cols.map((c) => sqlLiteral(row[c])).join(", ");
    lines.push(
      `INSERT INTO ${table} (${cols.join(", ")}) VALUES (${vals});`
    );
  }
  return lines.join("\n") + "\n";
}

export async function runBackup(env: Env): Promise<{ key: string; bytes: number }> {
  const now = nowMs();
  const day = new Date(now).toISOString().slice(0, 10);
  const key = `backups/${day}.sql`;

  const parts: string[] = [
    `-- summaverick D1 backup ${new Date(now).toISOString()}`,
    `PRAGMA foreign_keys=OFF;`,
    `BEGIN TRANSACTION;`,
  ];
  for (const t of TABLES) parts.push(await dumpTable(env, t));
  parts.push(`COMMIT;`);
  const body = parts.join("\n");

  await env.BODIES.put(key, body, {
    httpMetadata: { contentType: "application/sql" },
  });

  await pruneOldBackups(env, now);
  console.log(`backup written ${key} (${body.length} bytes)`);
  return { key, bytes: body.length };
}

/** Delete backups older than 30 days based on the YYYY-MM-DD in the key. */
async function pruneOldBackups(env: Env, now: number): Promise<void> {
  const cutoff = now - 30 * 24 * 60 * 60 * 1000;
  const listed = await env.BODIES.list({ prefix: "backups/" });
  for (const obj of listed.objects) {
    const m = obj.key.match(/backups\/(\d{4}-\d{2}-\d{2})\.sql$/);
    if (!m) continue;
    const t = Date.parse(m[1]! + "T00:00:00Z");
    if (Number.isFinite(t) && t < cutoff) {
      await env.BODIES.delete(obj.key);
      console.log(`pruned old backup ${obj.key}`);
    }
  }
}
