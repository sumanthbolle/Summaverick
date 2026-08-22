/**
 * CLI backup companion to the in-Worker nightly cron (src/lib/backup.ts).
 *
 *   node scripts/export-backup.ts [--local|--remote]
 *
 * Uses `wrangler d1 export` to produce a full .sql dump and uploads it to R2 at
 * backups/YYYY-MM-DD.sql. The Cron Trigger does the same thing from inside the
 * Worker; this script is for manual/CI backups where wrangler is available.
 *
 * Why this exists NOW (before real writes): from Phase 3 onward D1 is the only
 * copy of the UPSC archive — git is no longer the backup — so a working dump
 * path must exist before anything writes real data.
 */
import { execFileSync } from "node:child_process";
import { mkdtempSync, rmSync, statSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { parseTarget } from "./_util.ts";

const BUCKET = "summaverick-content";

function main(): void {
  const target = parseTarget(process.argv.slice(2));
  const day = new Date().toISOString().slice(0, 10);
  const dir = mkdtempSync(join(tmpdir(), "sv-backup-"));
  const dump = join(dir, `${day}.sql`);
  try {
    console.log(`exporting D1 (${target.flag}) -> ${dump}`);
    execFileSync(
      "node_modules/.bin/wrangler",
      ["d1", "export", "summaverick", target.flag, `--output=${dump}`],
      { stdio: ["ignore", "inherit", "inherit"] }
    );
    const bytes = statSync(dump).size;
    if (bytes === 0) throw new Error("dump is empty");
    console.log(`dump is ${bytes} bytes; uploading to R2 backups/${day}.sql`);
    execFileSync(
      "node_modules/.bin/wrangler",
      [
        "r2",
        "object",
        "put",
        `${BUCKET}/backups/${day}.sql`,
        `--file=${dump}`,
        "--content-type=application/sql",
        target.flag,
      ],
      { stdio: ["ignore", "inherit", "inherit"] }
    );
    console.log("backup uploaded.");
  } finally {
    rmSync(dir, { recursive: true, force: true });
  }
}

main();
