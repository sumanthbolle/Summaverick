/**
 * Shared helpers for the seed/backup/verify Node scripts.
 *
 * Seeds are emitted as chunked SQL files (<= CHUNK statements each) and executed
 * with `wrangler d1 execute --file`. Each file runs atomically, giving the same
 * "bounded batch" property the brief asks of db.batch() while staying runnable
 * from CI/local with no deployed Worker. Idempotency comes from ON CONFLICT.
 */
import { execFileSync } from "node:child_process";
import { mkdtempSync, writeFileSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";

export const CHUNK = 100;

export interface Target {
  flag: "--local" | "--remote";
  db: string;
}

export function parseTarget(argv: string[]): Target {
  const remote = argv.includes("--remote");
  return { flag: remote ? "--remote" : "--local", db: "summaverick" };
}

/** SQL literal with single-quote doubling. Handles null/number/bool/string. */
export function lit(v: unknown): string {
  if (v === null || v === undefined) return "NULL";
  if (typeof v === "number") return Number.isFinite(v) ? String(v) : "NULL";
  if (typeof v === "boolean") return v ? "1" : "0";
  return `'${String(v).replace(/'/g, "''")}'`;
}

/** JSON column literal (or NULL). */
export function jsonLit(v: unknown): string {
  if (v === null || v === undefined) return "NULL";
  return lit(JSON.stringify(v));
}

/** Normalise an absent/empty optional to null, never ''. */
export function orNull(v: unknown): string | null {
  if (v === null || v === undefined) return null;
  const s = String(v).trim();
  return s === "" ? null : s;
}

export function chunk<T>(arr: T[], size = CHUNK): T[][] {
  const out: T[][] = [];
  for (let i = 0; i < arr.length; i += size) out.push(arr.slice(i, i + size));
  return out;
}

/** Execute a list of SQL statements as one wrangler d1 file invocation. */
export function execSql(target: Target, statements: string[]): void {
  const dir = mkdtempSync(join(tmpdir(), "sv-seed-"));
  const file = join(dir, "chunk.sql");
  try {
    writeFileSync(file, statements.join("\n") + "\n", "utf8");
    execFileSync(
      "node_modules/.bin/wrangler",
      ["d1", "execute", target.db, target.flag, `--file=${file}`, "-y"],
      { stdio: ["ignore", "ignore", "inherit"] }
    );
  } finally {
    rmSync(dir, { recursive: true, force: true });
  }
}

/** Run a read query and return parsed rows via wrangler --json. */
export function query(target: Target, sql: string): Array<Record<string, unknown>> {
  const out = execFileSync(
    "node_modules/.bin/wrangler",
    ["d1", "execute", target.db, target.flag, "--json", `--command=${sql}`],
    { encoding: "utf8" }
  );
  // wrangler prints a JSON array; find the first '[' to skip any banner text.
  const start = out.indexOf("[");
  const parsed = JSON.parse(out.slice(start)) as Array<{
    results?: Array<Record<string, unknown>>;
  }>;
  return parsed[0]?.results ?? [];
}

export function log(msg: string): void {
  process.stdout.write(msg + "\n");
}
