import { describe, expect, it } from "vitest";
import {
  buildDemoEvents,
  DEMO_CASE,
  parseScenario,
} from "../src/domain/advocate";

describe("advocate demo script", () => {
  it("parses scenario, defaulting to cooperative", () => {
    expect(parseScenario("stubborn")).toBe("stubborn");
    expect(parseScenario("cooperative")).toBe("cooperative");
    expect(parseScenario(null)).toBe("cooperative");
    expect(parseScenario("nope")).toBe("cooperative");
  });

  it("cooperative path resolves with a refund and never escalates", () => {
    const events = buildDemoEvents("cooperative");
    const names = events.map((e) => e.event);
    expect(names).toContain("intake");
    expect(names).toContain("policy");
    expect(names).toContain("draft");
    expect(names).toContain("channel_opened");
    expect(names).toContain("agent_sent");
    expect(names).toContain("bot_replied");
    expect(names).toContain("resolved");
    expect(names).not.toContain("escalating");
    const resolved = events.find((e) => e.event === "resolved");
    expect(resolved?.data.refund_amount).toBe(DEMO_CASE.amount);
  });

  it("stubborn path escalates tone then still resolves", () => {
    const events = buildDemoEvents("stubborn");
    const tones = events
      .filter((e) => e.event === "escalating")
      .map((e) => e.data.tone);
    expect(tones).toEqual(["firm", "legal"]);
    expect(events.some((e) => e.event === "resolved")).toBe(true);
    const denies = events.filter(
      (e) => e.event === "bot_replied" && e.data.intent === "deny"
    );
    expect(denies.length).toBe(2);
  });

  it("never invents PII in outbound agent messages", () => {
    for (const scenario of ["cooperative", "stubborn"] as const) {
      const texts = buildDemoEvents(scenario)
        .filter((e) => e.event === "agent_sent" || e.event === "draft")
        .map((e) => String(e.data.message));
      for (const t of texts) {
        expect(t).not.toMatch(/\b\d{10}\b/); // phone
        expect(t).not.toMatch(/@/);
        expect(t).not.toMatch(/UPI/i);
      }
    }
  });
});
