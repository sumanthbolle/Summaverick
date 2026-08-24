/**
 * Deterministic customer-advocacy demo used by the Worker SSE endpoint.
 *
 * Mirrors the Python MVP in summaverick/: intake → policy → draft → mock
 * support-bot negotiation → refund. Offline and key-free, so the live site
 * can show the full agentic loop without FastAPI / NIM.
 */

export type AdvocateScenario = "cooperative" | "stubborn";

export interface DemoEvent {
  event: string;
  data: Record<string, unknown>;
  /** Milliseconds to wait *before* emitting this event (for the live stream). */
  delayMs: number;
}

export const DEMO_CASE = {
  platform: "swiggy",
  issueType: "missing_item",
  orderId: "SWG9F2K31",
  amount: 229,
  currency: "INR",
  item: "paneer roll",
  desiredOutcome: "a full refund of ₹229",
  policyWindowHours: 24,
} as const;

const DRAFTS = {
  polite:
    "Hello Swiggy support — I received order SWG9F2K31 but the paneer roll was missing. I paid ₹229 and I'm requesting a full refund to the original payment method, per your missing-item policy.",
  firm: "I need this missing-item claim on order SWG9F2K31 resolved. Swiggy policy lists missing items as eligible for a refund within 24 hours. Please process the ₹229 refund now.",
  legal:
    "This is a formal complaint regarding order SWG9F2K31 (missing item, ₹229). Under your published refund policy and applicable consumer-protection rules I am entitled to a refund. Please confirm processing or escalate to a human representative.",
} as const;

export function parseScenario(raw: string | null | undefined): AdvocateScenario {
  return raw === "stubborn" ? "stubborn" : "cooperative";
}

/** Scripted SSE events for one demo run. Pure — no IO. */
export function buildDemoEvents(scenario: AdvocateScenario): DemoEvent[] {
  const events: DemoEvent[] = [
    { event: "stage", data: { stage: "Intake" }, delayMs: 120 },
    {
      event: "intake",
      data: {
        platform: DEMO_CASE.platform,
        issueType: DEMO_CASE.issueType,
        orderId: DEMO_CASE.orderId,
        amount: DEMO_CASE.amount,
        summary: `Swiggy order #${DEMO_CASE.orderId} — ${DEMO_CASE.item} not delivered. Amount ₹${DEMO_CASE.amount} paid via UPI.`,
      },
      delayMs: 280,
    },
    { event: "stage", data: { stage: "Policy" }, delayMs: 220 },
    {
      event: "policy",
      data: {
        eligible: true,
        reason: DEMO_CASE.issueType,
        notes: `Swiggy refunds missing items within ${DEMO_CASE.policyWindowHours} hours to the original payment method.`,
        proof: ["order_confirmation"],
      },
      delayMs: 320,
    },
    { event: "stage", data: { stage: "Drafting" }, delayMs: 220 },
    {
      event: "draft",
      data: { tone: "polite", message: DRAFTS.polite },
      delayMs: 360,
    },
    { event: "stage", data: { stage: "Engaging" }, delayMs: 180 },
    { event: "channel_opened", data: {}, delayMs: 140 },
    {
      event: "bot_replied",
      data: {
        message: "Hi! Welcome to Swiggy support. How can I help you today?",
        intent: "greeting",
      },
      delayMs: 260,
    },
    { event: "agent_sent", data: { message: DRAFTS.polite }, delayMs: 420 },
    {
      event: "bot_replied",
      data: {
        message: "I can help with that. Could you share your order ID, please?",
        intent: "ask_order_id",
      },
      delayMs: 380,
    },
    {
      event: "agent_sent",
      data: { message: `My order ID is ${DEMO_CASE.orderId}.` },
      delayMs: 320,
    },
  ];

  if (scenario === "stubborn") {
    events.push(
      {
        event: "bot_replied",
        data: {
          message:
            "I'm sorry, but based on our records this doesn't appear eligible for a refund. Is there anything else?",
          intent: "deny",
        },
        delayMs: 420,
      },
      { event: "stage", data: { stage: "Escalating" }, delayMs: 160 },
      { event: "escalating", data: { tone: "firm" }, delayMs: 80 },
      { event: "agent_sent", data: { message: DRAFTS.firm }, delayMs: 480 },
      {
        event: "bot_replied",
        data: {
          message:
            "I'm sorry, but based on our records this doesn't appear eligible for a refund. Is there anything else?",
          intent: "deny",
        },
        delayMs: 420,
      },
      { event: "escalating", data: { tone: "legal" }, delayMs: 120 },
      { event: "agent_sent", data: { message: DRAFTS.legal }, delayMs: 520 }
    );
  }

  events.push(
    {
      event: "bot_replied",
      data: {
        message: `Thank you. I've located order ${DEMO_CASE.orderId}. Please give me a moment while I check this — processing your request.`,
        intent: "processing",
      },
      delayMs: 480,
    },
    {
      event: "bot_replied",
      data: {
        message: `Good news — I've approved a refund of ₹${DEMO_CASE.amount} to your original payment method. It will reflect in 24-48 hours.`,
        intent: "offer_refund",
        refund_amount: DEMO_CASE.amount,
      },
      delayMs: 640,
    },
    { event: "stage", data: { stage: "Resolved" }, delayMs: 180 },
    {
      event: "resolved",
      data: { refund_amount: DEMO_CASE.amount, intervention: false },
      delayMs: 80,
    }
  );

  return events;
}
