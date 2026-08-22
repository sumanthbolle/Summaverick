/**
 * UPSC retention sweep (T10). Archives published notes older than 180 days to R2
 * as JSONL, then deletes the D1 rows. This is the fix for data/upsc/history/
 * growing unboundedly (1.1 MB today, no eviction).
 */
import type { Env } from "../types";
import { deleteNotesByIds, notesOlderThan } from "../db/queries";
import { nowMs } from "./json";

const RETENTION_DAYS = 180;

export async function runUpscRetention(
  env: Env
): Promise<{ archived: number; deleted: number }> {
  const now = nowMs();
  const cutoff = now - RETENTION_DAYS * 24 * 60 * 60 * 1000;

  let archived = 0;
  let deleted = 0;

  // Work in bounded batches to stay within subrequest/time limits.
  for (let i = 0; i < 20; i++) {
    const batch = await notesOlderThan(env.DB, cutoff, 500);
    if (batch.length === 0) break;

    const day = new Date(now).toISOString().slice(0, 10);
    const key = `upsc-archive/${day}/batch-${i}.jsonl`;
    const jsonl = batch.map((n) => JSON.stringify(n)).join("\n") + "\n";
    await env.BODIES.put(key, jsonl, {
      httpMetadata: { contentType: "application/x-ndjson" },
    });
    archived += batch.length;

    deleted += await deleteNotesByIds(
      env.DB,
      batch.map((n) => n.id)
    );

    if (batch.length < 500) break;
  }

  console.log(`upsc retention: archived ${archived}, deleted ${deleted}`);
  return { archived, deleted };
}
