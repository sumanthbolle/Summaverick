/**
 * Thin wrapper over @simplewebauthn/server (works on Workers with nodejs_compat).
 * We do NOT hand-roll CBOR/COSE. Challenges are stashed in KV between the
 * options and verify calls, keyed by the caller's device id, with a short TTL.
 */
import {
  generateAuthenticationOptions,
  generateRegistrationOptions,
  verifyAuthenticationResponse,
  verifyRegistrationResponse,
} from "@simplewebauthn/server";
import type { Env } from "../types";

const CHALLENGE_TTL = 300; // seconds

function rpID(env: Env): string {
  return new URL(env.PUBLIC_ORIGIN).hostname;
}

/** Copy into a fresh ArrayBuffer-backed Uint8Array (satisfies lib's strict type). */
function u8(a: Uint8Array): Uint8Array<ArrayBuffer> {
  const b = new Uint8Array(a.byteLength);
  b.set(a);
  return b;
}

async function stashChallenge(
  env: Env,
  scope: string,
  deviceId: string,
  value: Record<string, unknown>
): Promise<void> {
  await env.CONFIG.put(`wa:${scope}:${deviceId}`, JSON.stringify(value), {
    expirationTtl: CHALLENGE_TTL,
  });
}
async function popChallenge(
  env: Env,
  scope: string,
  deviceId: string
): Promise<Record<string, unknown> | null> {
  const key = `wa:${scope}:${deviceId}`;
  const raw = await env.CONFIG.get(key);
  if (!raw) return null;
  await env.CONFIG.delete(key);
  try {
    return JSON.parse(raw) as Record<string, unknown>;
  } catch {
    return null;
  }
}

export async function regOptions(
  env: Env,
  deviceId: string,
  opts: { userId: string; userName: string; displayName?: string }
) {
  const options = await generateRegistrationOptions({
    rpName: "summaverick",
    rpID: rpID(env),
    userID: u8(new TextEncoder().encode(opts.userId)),
    userName: opts.userName,
    userDisplayName: opts.displayName ?? opts.userName,
    attestationType: "none",
    authenticatorSelection: {
      residentKey: "preferred",
      userVerification: "preferred",
    },
  });
  await stashChallenge(env, "reg", deviceId, {
    challenge: options.challenge,
    userId: opts.userId,
    userName: opts.userName,
    displayName: opts.displayName ?? opts.userName,
  });
  return options;
}

export interface VerifiedRegistration {
  userId: string;
  userName: string;
  displayName: string;
  credentialId: string;
  publicKey: Uint8Array;
  counter: number;
  transports: string[] | undefined;
}

export async function verifyRegistration(
  env: Env,
  deviceId: string,
  response: unknown
): Promise<VerifiedRegistration | null> {
  const stashed = await popChallenge(env, "reg", deviceId);
  if (!stashed) return null;
  const verification = await verifyRegistrationResponse({
    response: response as never,
    expectedChallenge: String(stashed.challenge),
    expectedOrigin: env.PUBLIC_ORIGIN,
    expectedRPID: rpID(env),
    requireUserVerification: false,
  });
  if (!verification.verified || !verification.registrationInfo) return null;
  const cred = verification.registrationInfo.credential;
  return {
    userId: String(stashed.userId),
    userName: String(stashed.userName),
    displayName: String(stashed.displayName),
    credentialId: cred.id,
    publicKey: cred.publicKey,
    counter: cred.counter,
    transports: cred.transports,
  };
}

export async function authOptions(
  env: Env,
  deviceId: string,
  allow: Array<{ id: string; transports?: string[] }>
) {
  const options = await generateAuthenticationOptions({
    rpID: rpID(env),
    userVerification: "preferred",
    allowCredentials: allow.map((c) => ({
      id: c.id,
      transports: c.transports as never,
    })),
  });
  await stashChallenge(env, "auth", deviceId, { challenge: options.challenge });
  return options;
}

export async function verifyAuthentication(
  env: Env,
  deviceId: string,
  response: unknown,
  credential: { id: string; publicKey: Uint8Array; counter: number }
): Promise<{ verified: boolean; newCounter: number }> {
  const stashed = await popChallenge(env, "auth", deviceId);
  if (!stashed) return { verified: false, newCounter: credential.counter };
  const verification = await verifyAuthenticationResponse({
    response: response as never,
    expectedChallenge: String(stashed.challenge),
    expectedOrigin: env.PUBLIC_ORIGIN,
    expectedRPID: rpID(env),
    credential: {
      id: credential.id,
      publicKey: u8(credential.publicKey),
      counter: credential.counter,
    },
    requireUserVerification: false,
  });
  return {
    verified: verification.verified,
    newCounter: verification.authenticationInfo?.newCounter ?? credential.counter,
  };
}
