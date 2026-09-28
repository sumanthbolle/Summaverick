import { describe, expect, it } from "vitest";
import {
  HttpServiceNowDocsProvider,
  parseLlmsIndex,
} from "../providers/servicenow-docs-provider.js";
import { defaultServiceNowDomainConfig } from "../config.js";
import { filterByReleaseFamily } from "../retrieval/release-filter.js";
import type { ServiceNowEvidence } from "../schemas/evidence.js";

const SAMPLE_INDEX = `
# Docs
- [IT Service Management](https://raw.githubusercontent.com/ServiceNow/ServiceNowDocs/australia/markdown/it-service-management/index.md)
- [CMDB topic](https://raw.githubusercontent.com/ServiceNow/ServiceNowDocs/australia/markdown/it-operations-management/cmdb.md)
- [Zurich only](https://raw.githubusercontent.com/ServiceNow/ServiceNowDocs/zurich/markdown/it-service-management/index.md)
`;

describe("docs provider", () => {
  it("parses llms.txt index links", () => {
    const entries = parseLlmsIndex(SAMPLE_INDEX);
    expect(entries.length).toBeGreaterThanOrEqual(2);
    expect(entries[0]?.title).toContain("IT Service Management");
  });

  it("filters product-doc evidence by release family", () => {
    const evidence: ServiceNowEvidence[] = [
      {
        id: "1",
        sourceType: "product_documentation",
        title: "A",
        content: "x",
        sourceReference: "a",
        releaseFamily: "australia",
        retrievedAt: new Date().toISOString(),
        authorityScore: 1,
        relevanceScore: 1,
        freshnessScore: 1,
        taskScoped: false,
        containsSensitiveData: false,
      },
      {
        id: "2",
        sourceType: "product_documentation",
        title: "Z",
        content: "x",
        sourceReference: "z",
        releaseFamily: "zurich",
        retrievedAt: new Date().toISOString(),
        authorityScore: 1,
        relevanceScore: 1,
        freshnessScore: 1,
        taskScoped: false,
        containsSensitiveData: false,
      },
    ];
    const filtered = filterByReleaseFamily(evidence, "australia");
    expect(filtered).toHaveLength(1);
    expect(filtered[0]?.releaseFamily).toBe("australia");
  });

  it("searches with release partitioning via mocked fetch", async () => {
    const fetchImpl: typeof fetch = async (input) => {
      const url = String(input);
      if (url.includes("llms.txt")) {
        return new Response(SAMPLE_INDEX, { status: 200 });
      }
      return new Response(
        "---\ntitle: CMDB reconciliation\ncanonical_url: https://example\n---\n# CMDB reconciliation\nHow reconciliation works.",
        { status: 200 }
      );
    };
    const provider = new HttpServiceNowDocsProvider(
      defaultServiceNowDomainConfig(),
      fetchImpl
    );
    const hits = await provider.search({
      query: "CMDB reconciliation",
      releaseFamily: "australia",
      modules: ["cmdb"],
      limit: 3,
    });
    expect(hits.length).toBeGreaterThan(0);
    expect(hits[0]?.releaseFamily).toBe("australia");
    expect(hits[0]?.content).toContain("BEGIN_UNTRUSTED_SERVICENOW_EVIDENCE");
  });
});
