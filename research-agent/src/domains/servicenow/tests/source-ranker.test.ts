import { describe, expect, it } from "vitest";
import { rankEvidence } from "../retrieval/source-ranker.js";
import { deduplicateEvidence } from "../retrieval/evidence-deduplicator.js";
import type { ServiceNowEvidence } from "../schemas/evidence.js";

function ev(
  partial: Partial<ServiceNowEvidence> &
    Pick<ServiceNowEvidence, "id" | "sourceType" | "title">
): ServiceNowEvidence {
  return {
    content: partial.content ?? "content",
    sourceReference: partial.sourceReference ?? partial.id,
    retrievedAt: new Date().toISOString(),
    authorityScore: partial.authorityScore ?? 1,
    relevanceScore: partial.relevanceScore ?? 0.5,
    freshnessScore: partial.freshnessScore ?? 0.5,
    taskScoped: false,
    containsSensitiveData: false,
    ...partial,
  };
}

describe("source ranker and deduplicator", () => {
  it("deduplicates identical evidence", () => {
    const a = ev({
      id: "a",
      sourceType: "product_documentation",
      title: "T",
      content: "same",
      sourceReference: "ref",
    });
    const b = ev({
      id: "b",
      sourceType: "product_documentation",
      title: "T",
      content: "same",
      sourceReference: "ref",
      relevanceScore: 0.9,
    });
    const out = deduplicateEvidence([a, b]);
    expect(out).toHaveLength(1);
    expect(out[0]?.id).toBe("b");
  });

  it("ranks SDK evidence higher for fluent intents", () => {
    const ranked = rankEvidence(
      [
        ev({
          id: "docs",
          sourceType: "product_documentation",
          title: "docs",
          authorityScore: 1,
          relevanceScore: 0.8,
        }),
        ev({
          id: "sdk",
          sourceType: "sdk_explain",
          title: "sdk",
          authorityScore: 1,
          relevanceScore: 0.8,
        }),
      ],
      "fluent_sdk"
    );
    expect(ranked[0]?.sourceType).toBe("sdk_explain");
  });

  it("ranks live instance highest for instance schema intents", () => {
    const ranked = rankEvidence(
      [
        ev({
          id: "docs",
          sourceType: "product_documentation",
          title: "docs",
          authorityScore: 1,
          relevanceScore: 0.9,
        }),
        ev({
          id: "live",
          sourceType: "live_instance",
          title: "live",
          authorityScore: 1,
          relevanceScore: 0.9,
        }),
      ],
      "instance_schema"
    );
    expect(ranked[0]?.sourceType).toBe("live_instance");
  });
});
