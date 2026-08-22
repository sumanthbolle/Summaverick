/**
 * Worker-runtime shims for the handful of Node built-ins the original pipeline
 * used (`node:fs`, `node:path`, `node:crypto`). The app typechecks with only
 * `@cloudflare/workers-types` (no `@types/node`), and a Cloudflare Worker has no
 * local filesystem and cannot spawn subprocesses, so:
 *
 *   - The `path` helpers are pure string implementations (correct in a Worker).
 *   - The `fs` helpers report "nothing on disk": `existsSync` is always false and
 *     the readers throw. This makes the filesystem-backed retrieval layers
 *     (Layer 1 SDK `explain` and the local-repository provider) degrade to
 *     "no local Fluent project found" — which is the truthful state inside a
 *     Worker — rather than fabricating evidence. The provider CODE, its
 *     interfaces, and the trace it emits are preserved unchanged.
 *   - `hashHex` replaces `crypto.createHash("sha256")` for the non-security
 *     content-hash used in doc caching/dedup (a fast FNV-1a; collision-resistance
 *     is not required there).
 */

// ---- path (pure) ----------------------------------------------------------
export function join(...parts: string[]): string {
  return parts
    .filter((p) => p.length > 0)
    .join("/")
    .replace(/\/{2,}/g, "/");
}

export function dirname(p: string): string {
  const idx = p.replace(/\/+$/, "").lastIndexOf("/");
  if (idx <= 0) return idx === 0 ? "/" : ".";
  return p.slice(0, idx);
}

export function resolve(p: string): string {
  return p.startsWith("/") ? p : `/${p}`;
}

export function relative(from: string, to: string): string {
  return to.startsWith(from) ? to.slice(from.length).replace(/^\/+/, "") : to;
}

// ---- fs (no real filesystem in a Worker) ----------------------------------
export function existsSync(_path: string): boolean {
  return false;
}

export async function readFile(path: string, _enc?: string): Promise<string> {
  throw new Error(`No filesystem in Worker runtime: cannot read ${path}`);
}

export interface DirEnt {
  name: string;
  isDirectory(): boolean;
}

export async function readdir(
  _path: string,
  _opts?: { withFileTypes: true }
): Promise<DirEnt[]> {
  return [];
}

// ---- crypto (non-security content hash) ------------------------------------
export function hashHex(input: string): string {
  // FNV-1a 32-bit, expanded to 16 hex chars via a second pass. Used only for
  // cache keys / dedup, never for a security decision.
  let h1 = 0x811c9dc5;
  let h2 = 0x811c9dc5 ^ 0x5bd1e995;
  for (let i = 0; i < input.length; i++) {
    const c = input.charCodeAt(i);
    h1 = Math.imul(h1 ^ c, 0x01000193) >>> 0;
    h2 = Math.imul(h2 ^ (c + 1), 0x01000193) >>> 0;
  }
  return (
    h1.toString(16).padStart(8, "0") + h2.toString(16).padStart(8, "0")
  ).slice(0, 16);
}
