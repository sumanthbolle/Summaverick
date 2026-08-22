/** Hashing + token helpers shared by auth, magic links and UPSC ingest. */

const enc = new TextEncoder();

function toHex(buf: ArrayBuffer): string {
  const bytes = new Uint8Array(buf);
  let out = "";
  for (const b of bytes) out += b.toString(16).padStart(2, "0");
  return out;
}

/** SHA-256 hex digest of a string. Used to store magic-link tokens hashed. */
export async function sha256Hex(input: string): Promise<string> {
  const digest = await crypto.subtle.digest("SHA-256", enc.encode(input));
  return toHex(digest);
}

/** Stable, non-reversible hash of a value (e.g. UA string) with a namespace. */
export async function fingerprint(value: string, salt = ""): Promise<string> {
  return (await sha256Hex(`${salt}:${value}`)).slice(0, 32);
}

/** Cryptographically-random URL-safe token (base64url, no padding). */
export function randomToken(bytes = 32): string {
  const buf = new Uint8Array(bytes);
  crypto.getRandomValues(buf);
  let bin = "";
  for (const b of buf) bin += String.fromCharCode(b);
  return btoa(bin).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/, "");
}

/** Constant-time-ish string comparison for tokens/secrets. */
export function timingSafeEqual(a: string, b: string): boolean {
  if (a.length !== b.length) return false;
  let diff = 0;
  for (let i = 0; i < a.length; i++) diff |= a.charCodeAt(i) ^ b.charCodeAt(i);
  return diff === 0;
}
