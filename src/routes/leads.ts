/**
 * Project enquiries from the homepage form.
 *
 *   POST /api/leads            store an enquiry, notify the owner if configured
 *   GET  /api/admin/leads      read them back (Cloudflare Access)
 *
 * The form previously confirmed "your message is on its way" from the browser
 * alone and sent it nowhere, which made the homepage's promise that a person
 * reads every message untrue. The row in D1 is now the record of receipt: the
 * email notification is a convenience on top of it, and the endpoint reports
 * whether it actually left so nothing claims delivery it cannot back up.
 */
import type { Ctx, Env, RouteDef, RouteMaker } from "../types";
import { badRequest, forbidden, json, newId, nowMs, ok, readJson } from "../lib/json";
import { clientKey, rateLimit } from "../lib/ratelimit";
import { insertLead, listLeads } from "../db/queries";

const INTENTS = ["servicenow_app", "integration", "ai_tool", "unsure"] as const;
type Intent = (typeof INTENTS)[number];

const MAX = { name: 120, email: 254, organisation: 160, message: 4000 };
const HOURLY = { limit: 5, window: 3600 };
const DAILY = { limit: 15, window: 86400 };

interface LeadBody {
  name?: unknown;
  email?: unknown;
  organisation?: unknown;
  intent?: unknown;
  message?: unknown;
  company_url?: unknown;
}

function str(value: unknown, max: number): string {
  return typeof value === "string" ? value.trim().slice(0, max) : "";
}

/**
 * Deliberately permissive: the address only has to be plausible enough to be
 * worth a reply. Rejecting an unusual but valid address costs a conversation.
 */
function plausibleEmail(value: string): boolean {
  return /^[^\s@]+@[^\s@.]+(\.[^\s@.]+)+$/.test(value);
}

/**
 * Notify the owner. Absent configuration is not an error the visitor should
 * see — the enquiry is already stored — but it is logged, because it means
 * nobody is being told about it.
 */
async function notifyOwner(
  env: Env,
  lead: { id: string; name: string; email: string; organisation: string; intent: Intent; message: string }
): Promise<boolean> {
  const to = env.LEAD_NOTIFY_TO;
  if (!to || !env.RESEND_API_KEY) {
    console.log(
      `[lead] ${lead.id} stored; no notification sent ` +
        `(${!to ? "LEAD_NOTIFY_TO" : "RESEND_API_KEY"} is not configured)`
    );
    return false;
  }
  const lines = [
    `Intent: ${lead.intent}`,
    `Name: ${lead.name || "(not given)"}`,
    `Email: ${lead.email}`,
    `Organisation: ${lead.organisation || "(not given)"}`,
    "",
    lead.message,
  ];
  try {
    const res = await fetch("https://api.resend.com/emails", {
      method: "POST",
      headers: {
        authorization: `Bearer ${env.RESEND_API_KEY}`,
        "content-type": "application/json",
      },
      body: JSON.stringify({
        from: env.LEAD_NOTIFY_FROM ?? env.MAGIC_LINK_FROM,
        to,
        reply_to: lead.email,
        subject: `Project enquiry — ${lead.intent}`,
        text: lines.join("\n"),
      }),
    });
    if (!res.ok) {
      console.error(`[lead] ${lead.id} notification failed: HTTP ${res.status}`);
      return false;
    }
    return true;
  } catch (e) {
    console.error(`[lead] ${lead.id} notification failed`, e);
    return false;
  }
}

export function leadRoutes(route: RouteMaker): RouteDef[] {
  return [
    route("POST", "/api/leads", async (req, ctx: Ctx) => {
      const body = await readJson<LeadBody>(req);
      if (!body) return badRequest("a JSON body is required");

      // Honeypot. A bot filled a field no visitor can see, so answer the way
      // a success looks and store nothing.
      if (str(body.company_url, 200)) {
        return ok({ reference: newId("lead") });
      }

      const email = str(body.email, MAX.email).toLowerCase();
      if (!email) return badRequest("an email address is required so we can reply");
      if (!plausibleEmail(email)) return badRequest("that email address does not look complete");

      const message = str(body.message, MAX.message);
      if (message.length < 10) {
        return badRequest("tell us a little about the work — a sentence is enough");
      }

      const rawIntent = str(body.intent, 40);
      const intent: Intent = (INTENTS as readonly string[]).includes(rawIntent)
        ? (rawIntent as Intent)
        : "unsure";

      const key = clientKey(req, ctx.session.deviceId);
      const hourly = await rateLimit(ctx.env, `lead:h:${key}`, HOURLY.limit, HOURLY.window);
      const daily = await rateLimit(ctx.env, `lead:d:${key}`, DAILY.limit, DAILY.window);
      if (!hourly.allowed || !daily.allowed) {
        const resetAt = hourly.allowed ? daily.resetAt : hourly.resetAt;
        const mins = Math.max(1, Math.ceil((resetAt - nowMs()) / 60000));
        return json(
          {
            ok: false,
            error: "rate_limited",
            message: `That is a lot of messages from one place. Try again in about ${mins} minute(s).`,
          },
          { status: 429, headers: { "retry-after": String(Math.max(60, mins * 60)) } }
        );
      }

      const lead = {
        id: newId("lead"),
        name: str(body.name, MAX.name),
        email,
        organisation: str(body.organisation, MAX.organisation),
        intent,
        message,
      };

      try {
        await insertLead(ctx.env.DB, {
          id: lead.id,
          device_id: ctx.session.deviceId,
          name: lead.name || null,
          email: lead.email,
          organisation: lead.organisation || null,
          intent: lead.intent,
          message: lead.message,
          notified: 0,
          user_agent: req.headers.get("user-agent")?.slice(0, 300) ?? null,
          created_at: nowMs(),
        });
      } catch (e) {
        console.error("[lead] could not be stored", e);
        return json(
          {
            ok: false,
            error: "not_stored",
            message: "We could not save that message. Nothing was lost on your side — try again.",
          },
          { status: 503 }
        );
      }

      // The visitor already has a stored enquiry; the notification is allowed
      // to finish after the response.
      ctx.exec.waitUntil(
        (async () => {
          const sent = await notifyOwner(ctx.env, lead);
          if (!sent) return;
          try {
            await ctx.env.DB.prepare(`UPDATE leads SET notified = 1 WHERE id = ?`)
              .bind(lead.id)
              .run();
          } catch (e) {
            console.error("[lead] notified flag not written (non-fatal)", e);
          }
        })()
      );

      return ok({ reference: lead.id });
    }),

    route("GET", "/api/admin/leads", async (req, ctx: Ctx) => {
      if (ctx.env.ENVIRONMENT !== "development") {
        const who = req.headers.get("cf-access-authenticated-user-email");
        if (!who) return forbidden("admin access required (Cloudflare Access)");
      }
      const leads = await listLeads(ctx.env.DB, 200);
      return json({
        ok: true,
        count: leads.length,
        leads: leads.map((l) => ({
          id: l.id,
          name: l.name,
          email: l.email,
          organisation: l.organisation,
          intent: l.intent,
          message: l.message,
          notified: l.notified === 1,
          createdAt: l.created_at,
        })),
      });
    }),
  ];
}
