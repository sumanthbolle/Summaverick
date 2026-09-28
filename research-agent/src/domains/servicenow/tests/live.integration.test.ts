/**
 * Live instance integration tests — skipped unless SERVICENOW_LIVE_INTEGRATION=1.
 * These require a configured Fluent project and authenticated SDK profile.
 */
import { describe, expect, it } from "vitest";

const live = process.env.SERVICENOW_LIVE_INTEGRATION === "1";

describe.runIf(live)("live ServiceNow instance integration", () => {
  it("placeholder — enable with SERVICENOW_LIVE_INTEGRATION=1", async () => {
    expect(process.env.SERVICENOW_INSTANCE_URL).toBeTruthy();
  });
});

describe.runIf(!live)("live integration gate", () => {
  it("is skipped by default", () => {
    expect(live).toBe(false);
  });
});
