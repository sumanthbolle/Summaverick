/**
 * Contact messages from the homepage form.
 *
 * The form previously confirmed success in the browser without sending
 * anything. It now posts here, and the wording the visitor sees follows this
 * response: stored means stored, a 4xx/5xx means not sent, and a request that
 * never completes is reported as unconfirmed rather than as success.
 *
 * Delivery has two parts, reported separately:
 *   stored    the row is in D1 — always true for a 201
 *   notified  a mail provider accepted an email; false when none is configured
 *
 * The hidden `company_url` field is a honeypot. A filled one answers normally
 * and stores nothing, so a bot learns nothing from the response.
 */
import type { Ctx, Env, RouteDef, RouteMaker } from "../types";
import { badRequest, json, newId, nowMs, readJson } from "../lib/json";
import { clientKey, rateLimit } from "../lib/ratelimit";
import {
  findLeadByIdempotencyKey,
  insertLead,
  markLeadNotified,
} from "../db/queries";

const MAX = { name: 120, email: 200, organisation: 160, intent: 60, message: 4000 };
const BURST = { limit: 3, window: 600 }; // 3 per 10 minutes
const DAILY = { limit: 12, window: 86400 };

const INTENTS = new Set([
  "servicenow_application",
  "system_integration",
  "ai_tool",
  "not_sure",
]);

interface ContactBody {
  name?: unknown;
  email?: unknown;
  organisation?: unknown;
  intent?: unknown;
  message?: unknown;
  company_url?: unknown;
  idempotency_key?: unknown;
}

const text = (value: unknown, max: number): string =>
  typeof value === "string" ? value.trim().slice(0, max) : "";

/** Deliberately permissive: shape only, so a valid address is never rejected. */
function looksLikeEmail(value: string): boolean {
  return /^[^\s@]+@[^\s@.]+\.[^\s@]+$/.test(value);
}

async function notifyTeam(
  env: Env,
  lead: { id: string; name: string; email: string; organisation: string; intent: string; message: string }
): Promise<boolean> {
  if (!env.RESEND_API_KEY) return false;
  const lines = [
    `Name: ${lead.name || "(not given)"}`,
    `Email: ${lead.email}`,
    `Organisation: ${lead.organisation || "(not given)"}`,
    `Needs help with: ${lead.intent || "(not given)"}`,
    "",
    lead.message || "(no message)",
  ];
  const res = await fetch("https://api.resend.com/emails", {
    method: "POST",
    headers: {
      authorization: `Bearer ${env.RESEND_API_KEY}`,
      "content-type": "application/json",
    },
    body: JSON.stringify({
      from: env.MAGIC_LINK_FROM,
      to: env.MAGIC_LINK_FROM,
      reply_to: lead.email,
      subject: `Website enquiry — ${lead.intent || "general"}`,
      text: lines.join("\n"),
    }),
  });
  return res.ok;
}

export function contactRoutes(route: RouteMaker): RouteDef[] {
  return [
    route("POST", "/api/contact", async (req, ctx: Ctx) => {
      const body = await readJson<ContactBody>(req);
      if (!body) return badRequest("send a JSON body");

      // Honeypot: answer as though it worked, store nothing.
      if (text(body.company_url, 200)) {
        return json({ ok: true, stored: false, notified: false }, { status: 201 });
      }

      const email = text(body.email, MAX.email);
      const message = text(body.message, MAX.message);
      const fieldErrors: Record<string, string> = {};
      if (!email) fieldErrors.email = "Enter an email address so we can reply.";
      else if (!looksLikeEmail(email)) {
        fieldErrors.email = "That email address does not look complete.";
      }
      if (!message) {
        fieldErrors.message = "Tell us a little about what you need.";
      }
      if (Object.keys(fieldErrors).length) {
        return json(
          { ok: false, error: "invalid", fieldErrors },
          { status: 422 }
        );
      }

      // A retry of a message that is already stored is answered from the row
      // itself, before the rate limiter. Otherwise a visitor retrying an
      // unconfirmed submission would be told to try again when the message had
      // in fact been received.
      const idempotencyKey = text(body.idempotency_key, 80) || null;
      if (idempotencyKey) {
        const seen = await findLeadByIdempotencyKey(ctx.env.DB, idempotencyKey);
        if (seen) {
          return json(
            {
              ok: true,
              id: seen.id,
              stored: true,
              notified: seen.notified === 1,
              duplicate: true,
            },
            { status: 201 }
          );
        }
      }

      const key = clientKey(req, ctx.session.deviceId);
      const burst = await rateLimit(ctx.env, `lead:b:${key}`, BURST.limit, BURST.window);
      const daily = await rateLimit(ctx.env, `lead:d:${key}`, DAILY.limit, DAILY.window);
      if (!burst.allowed || !daily.allowed) {
        const resetAt = !daily.allowed ? daily.resetAt : burst.resetAt;
        const secs = Math.max(1, Math.ceil((resetAt - nowMs()) / 1000));
        return json(
          {
            ok: false,
            error: "rate_limited",
            message: `That is a few messages in a short time. Try again in ${secs}s.`,
          },
          { status: 429, headers: { "retry-after": String(secs) } }
        );
      }

      const lead = {
        id: newId("lead"),
        idempotency_key: idempotencyKey,
        name: text(body.name, MAX.name) || null,
        email,
        organisation: text(body.organisation, MAX.organisation) || null,
        intent: INTENTS.has(text(body.intent, MAX.intent))
          ? text(body.intent, MAX.intent)
          : null,
        message,
        notified: 0,
        created_at: nowMs(),
      };

      const stored = await insertLead(ctx.env.DB, lead);

      let notified = false;
      if (!stored.duplicate) {
        try {
          notified = await notifyTeam(ctx.env, {
            id: stored.id,
            name: lead.name ?? "",
            email: lead.email,
            organisation: lead.organisation ?? "",
            intent: lead.intent ?? "",
            message: lead.message,
          });
        } catch (e) {
          console.error("lead notification failed (message is stored)", e);
        }
        if (notified) {
          ctx.exec.waitUntil(markLeadNotified(ctx.env.DB, stored.id).catch(() => {}));
        }
      }

      return json(
        { ok: true, id: stored.id, stored: true, notified, duplicate: stored.duplicate },
        { status: 201 }
      );
    }),
  ];
}
