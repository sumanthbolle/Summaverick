/**
 * Auth (T5): passkeys (WebAuthn) as primary, email magic-link as fallback.
 * No password hashes are ever stored.
 *
 * Anonymous users are first-class: everyone already has a session + device_id
 * (see lib/session). On successful auth we run the CLAIM step so the device's
 * prior attempts follow the now-known user.
 */
import type { Env, RouteDef, RouteMaker, SessionInfo } from "../types";
import {
  attachUserToSession,
  claimDeviceForUser,
  consumeMagicLink,
  createUser,
  getCredentialById,
  getCredentialsByUser,
  getMagicLink,
  getUserByEmail,
  getUserById,
  insertCredential,
  insertMagicLink,
  updateSignCount,
} from "../db/queries";
import {
  authOptions,
  regOptions,
  verifyAuthentication,
  verifyRegistration,
} from "../lib/webauthn";
import { randomToken, sha256Hex } from "../lib/crypto";
import { clearCookie } from "../lib/session";
import { rateLimit, clientKey } from "../lib/ratelimit";
import {
  badRequest,
  error,
  json,
  newId,
  nowMs,
  ok,
  readJson,
  unauthorized,
} from "../lib/json";

const MAGIC_TTL_MS = 15 * 60 * 1000;

async function completeLogin(
  env: Env,
  session: SessionInfo,
  userId: string
): Promise<void> {
  await attachUserToSession(env.DB, session.sessionId, userId);
  await claimDeviceForUser(env.DB, session.deviceId, userId);
}

async function sendMagicEmail(
  env: Env,
  email: string,
  link: string
): Promise<void> {
  if (!env.RESEND_API_KEY) {
    // Local dev / no email provider: surface the link in logs instead.
    console.log(`[magic-link] ${email} -> ${link}`);
    return;
  }
  await fetch("https://api.resend.com/emails", {
    method: "POST",
    headers: {
      authorization: `Bearer ${env.RESEND_API_KEY}`,
      "content-type": "application/json",
    },
    body: JSON.stringify({
      from: env.MAGIC_LINK_FROM,
      to: email,
      subject: "Your summaverick sign-in link",
      html: `<p>Click to sign in:</p><p><a href="${link}">${link}</a></p><p>This link expires in 15 minutes.</p>`,
    }),
  });
}

function publicUser(u: {
  id: string;
  email: string | null;
  display_name: string | null;
}) {
  return { id: u.id, email: u.email, displayName: u.display_name };
}

export function authRoutes(route: RouteMaker): RouteDef[] {
  return [
    // ---- WebAuthn registration ----
    route("POST", "/api/auth/register/options", async (req, ctx) => {
      const body = await readJson<{ email?: string; displayName?: string }>(req);
      const email = body?.email?.trim().toLowerCase() || undefined;
      if (email && (await getUserByEmail(ctx.env.DB, email))) {
        return error(409, "email_taken", "email already registered — sign in");
      }
      const userId = newId("usr");
      const userName = email ?? body?.displayName ?? `user-${userId.slice(-6)}`;
      // Stash email alongside the challenge so verify can create the user.
      await ctx.env.CONFIG.put(
        `wa:regmeta:${ctx.session.deviceId}`,
        JSON.stringify({ email: email ?? null }),
        { expirationTtl: 300 }
      );
      const options = await regOptions(ctx.env, ctx.session.deviceId, {
        userId,
        userName,
        displayName: body?.displayName,
      });
      return json({ ok: true, options });
    }),

    route("POST", "/api/auth/register/verify", async (req, ctx) => {
      const body = await readJson<unknown>(req);
      if (!body) return badRequest("missing attestation response");
      const verified = await verifyRegistration(
        ctx.env,
        ctx.session.deviceId,
        body
      );
      if (!verified) return badRequest("registration failed");

      const metaRaw = await ctx.env.CONFIG.get(
        `wa:regmeta:${ctx.session.deviceId}`
      );
      await ctx.env.CONFIG.delete(`wa:regmeta:${ctx.session.deviceId}`);
      const email = metaRaw ? (JSON.parse(metaRaw).email as string | null) : null;

      const now = nowMs();
      await createUser(ctx.env.DB, {
        id: verified.userId,
        email,
        display_name: verified.displayName,
        created_at: now,
        last_seen_at: now,
      });
      await insertCredential(ctx.env.DB, {
        id: verified.credentialId,
        user_id: verified.userId,
        public_key: toArrayBuffer(verified.publicKey),
        sign_count: verified.counter,
        transports: verified.transports
          ? JSON.stringify(verified.transports)
          : null,
        created_at: now,
      });
      await completeLogin(ctx.env, ctx.session, verified.userId);
      return ok({ user: publicUser({ id: verified.userId, email, display_name: verified.displayName }) });
    }),

    // ---- WebAuthn login ----
    route("POST", "/api/auth/login/options", async (req, ctx) => {
      const body = await readJson<{ email?: string }>(req);
      let allow: Array<{ id: string; transports?: string[] }> = [];
      const email = body?.email?.trim().toLowerCase();
      if (email) {
        const user = await getUserByEmail(ctx.env.DB, email);
        if (user) {
          const creds = await getCredentialsByUser(ctx.env.DB, user.id);
          allow = creds.map((c) => ({
            id: c.id,
            transports: c.transports ? JSON.parse(c.transports) : undefined,
          }));
        }
      }
      const options = await authOptions(ctx.env, ctx.session.deviceId, allow);
      return json({ ok: true, options });
    }),

    route("POST", "/api/auth/login/verify", async (req, ctx) => {
      const body = await readJson<{ id?: string }>(req);
      if (!body?.id) return badRequest("missing assertion response");
      const cred = await getCredentialById(ctx.env.DB, body.id);
      if (!cred) return badRequest("unknown credential");

      const result = await verifyAuthentication(
        ctx.env,
        ctx.session.deviceId,
        body,
        {
          id: cred.id,
          publicKey: new Uint8Array(cred.public_key),
          counter: cred.sign_count,
        }
      );
      if (!result.verified) return unauthorized("assertion failed");

      await updateSignCount(ctx.env.DB, cred.id, result.newCounter);
      await completeLogin(ctx.env, ctx.session, cred.user_id);
      const user = await getUserById(ctx.env.DB, cred.user_id);
      return ok({ user: user ? publicUser(user) : null });
    }),

    // ---- Magic link ----
    route("POST", "/api/auth/magic", async (req, ctx) => {
      const rl = await rateLimit(
        ctx.env,
        `magic:${clientKey(req, ctx.session.deviceId)}`,
        5,
        3600
      );
      if (!rl.allowed) return error(429, "rate_limited", "too many requests");

      const body = await readJson<{ email?: string }>(req);
      const email = body?.email?.trim().toLowerCase();
      if (!email || !email.includes("@")) return badRequest("valid email required");

      const token = randomToken(32);
      const tokenHash = await sha256Hex(token);
      const now = nowMs();
      await insertMagicLink(ctx.env.DB, {
        token_hash: tokenHash,
        email,
        expires_at: now + MAGIC_TTL_MS,
        consumed_at: null,
      });
      const link = `${ctx.env.PUBLIC_ORIGIN}/api/auth/callback?token=${token}`;
      await sendMagicEmail(ctx.env, email, link);
      // Never reveal whether the email exists.
      return ok({ sent: true });
    }),

    route("GET", "/api/auth/callback", async (_req, ctx) => {
      const token = ctx.url.searchParams.get("token") ?? "";
      if (!token) return badRequest("missing token");
      const tokenHash = await sha256Hex(token);
      const now = nowMs();
      const consumed = await consumeMagicLink(ctx.env.DB, tokenHash, now);
      if (!consumed) return badRequest("invalid or expired link");
      const link = await getMagicLink(ctx.env.DB, tokenHash);
      if (!link) return badRequest("invalid link");

      let user = await getUserByEmail(ctx.env.DB, link.email);
      if (!user) {
        const id = newId("usr");
        await createUser(ctx.env.DB, {
          id,
          email: link.email,
          display_name: link.email.split("@")[0] ?? null,
          created_at: now,
          last_seen_at: now,
        });
        user = await getUserById(ctx.env.DB, id);
      }
      if (user) await completeLogin(ctx.env, ctx.session, user.id);
      return new Response(null, { status: 302, headers: { location: "/" } });
    }),

    // ---- session ----
    route("GET", "/api/me", async (_req, ctx) => {
      if (!ctx.session.userId) return unauthorized("not signed in");
      const user = await getUserById(ctx.env.DB, ctx.session.userId);
      if (!user) return unauthorized("not signed in");
      return ok({ user: publicUser(user), deviceId: ctx.session.deviceId });
    }),

    route("POST", "/api/auth/logout", async (_req, ctx) => {
      const res = ok({ loggedOut: true });
      res.headers.append("set-cookie", clearCookie("sv_sid"));
      return res;
    }),
  ];
}

function toArrayBuffer(u: Uint8Array): ArrayBuffer {
  return u.buffer.slice(u.byteOffset, u.byteOffset + u.byteLength) as ArrayBuffer;
}
