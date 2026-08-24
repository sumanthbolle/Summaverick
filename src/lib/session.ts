/**
 * Session + device resolution. Every visitor — anonymous or authenticated — has
 * a session row. Anonymous sessions carry a device_id and NULL user_id; when the
 * visitor later authenticates we run a claim step (see queries.claimDeviceForUser)
 * so their prior attempts follow them.
 */
import type { Env, SessionInfo } from "../types";
import {
  createSession,
  extendSession,
  getSession,
  getUserById,
  touchUser,
} from "../db/queries";
import { newId, nowMs } from "./json";
import { fingerprint } from "./crypto";

const SESSION_COOKIE = "sv_sid";
const DEVICE_COOKIE = "sv_did";
const SESSION_TTL_MS = 30 * 24 * 60 * 60 * 1000; // 30 days

function parseCookies(header: string | null): Record<string, string> {
  const out: Record<string, string> = {};
  if (!header) return out;
  for (const part of header.split(";")) {
    const idx = part.indexOf("=");
    if (idx === -1) continue;
    const k = part.slice(0, idx).trim();
    const v = part.slice(idx + 1).trim();
    if (k) out[k] = decodeURIComponent(v);
  }
  return out;
}

/**
 * Resolve (or create) the session for a request. Never throws; on any DB miss
 * it mints a fresh anonymous session. Returns the SessionInfo plus the Set-Cookie
 * headers that must be attached to the response when `isNew` is true.
 */
function ephemeral(
  cookies: Record<string, string>
): { info: SessionInfo; setCookies: string[] } {
  const now = nowMs();
  return {
    info: {
      sessionId: cookies[SESSION_COOKIE] ?? newId("sess"),
      deviceId: cookies[DEVICE_COOKIE] ?? newId("dev"),
      userId: null,
      isNew: false,
      expiresAt: now + SESSION_TTL_MS,
    },
    setCookies: [],
  };
}

export async function resolveSession(
  req: Request,
  env: Env
): Promise<{ info: SessionInfo; setCookies: string[] }> {
  const cookies = parseCookies(req.headers.get("cookie"));
  try {
    return await resolveSessionInner(req, env, cookies);
  } catch (err) {
    // D1 may not be migrated yet; never take the whole API down with it.
    console.error("session resolve failed", err);
    return ephemeral(cookies);
  }
}

async function resolveSessionInner(
  req: Request,
  env: Env,
  cookies: Record<string, string>
): Promise<{ info: SessionInfo; setCookies: string[] }> {
  const now = nowMs();
  const uaHash = await fingerprint(
    req.headers.get("user-agent") ?? "unknown",
    env.SESSION_SECRET ?? "sv"
  );

  const existingSid = cookies[SESSION_COOKIE];
  if (existingSid) {
    const row = await getSession(env.DB, existingSid);
    if (row && row.expires_at > now) {
      // Sliding expiry: extend lazily when past the halfway mark.
      const halfway = row.created_at + SESSION_TTL_MS / 2;
      let expiresAt = row.expires_at;
      if (now > halfway) {
        expiresAt = now + SESSION_TTL_MS;
        await extendSession(env.DB, row.id, expiresAt);
      }
      if (row.user_id) await touchUser(env.DB, row.user_id, now);
      return {
        info: {
          sessionId: row.id,
          deviceId: row.device_id,
          userId: row.user_id,
          isNew: false,
          expiresAt,
        },
        setCookies: [],
      };
    }
  }

  // Mint a fresh anonymous session, reusing the device id if we have one.
  const deviceId = cookies[DEVICE_COOKIE] ?? newId("dev");
  const sessionId = newId("sess");
  const expiresAt = now + SESSION_TTL_MS;
  await createSession(env.DB, {
    id: sessionId,
    user_id: null,
    device_id: deviceId,
    created_at: now,
    expires_at: expiresAt,
    ua_hash: uaHash,
  });

  return {
    info: { sessionId, deviceId, userId: null, isNew: true, expiresAt },
    setCookies: [
      cookie(SESSION_COOKIE, sessionId, expiresAt),
      cookie(DEVICE_COOKIE, deviceId, now + 400 * 24 * 60 * 60 * 1000),
    ],
  };
}

/** Build a hardened cookie string. */
export function cookie(name: string, value: string, expiresAtMs: number): string {
  const maxAge = Math.max(0, Math.floor((expiresAtMs - nowMs()) / 1000));
  return [
    `${name}=${encodeURIComponent(value)}`,
    "Path=/",
    "HttpOnly",
    "Secure",
    "SameSite=Lax",
    `Max-Age=${maxAge}`,
  ].join("; ");
}

/** Cookie string that clears a cookie. */
export function clearCookie(name: string): string {
  return `${name}=; Path=/; HttpOnly; Secure; SameSite=Lax; Max-Age=0`;
}

export { SESSION_COOKIE, DEVICE_COOKIE, SESSION_TTL_MS };

/** Convenience: load the full user row for an authenticated session. */
export async function currentUser(env: Env, info: SessionInfo) {
  if (!info.userId) return null;
  return getUserById(env.DB, info.userId);
}
