/**
 * Retrieval and answer-check behaviour for the public research agent.
 *
 * These cases pin the failures that made the demo untrustworthy:
 *   - only publication tables of contents could ever be retrieved, because
 *     `llms.txt` links nothing else;
 *   - raw YAML front matter reached the answer surface;
 *   - pages matching only the corpus's most common words ranked first;
 *   - a citation count was reported as "verified".
 */
import { describe, expect, it } from "vitest";
import {
  buildDocsQueryTerms,
  computeTermWeights,
  extractPassage,
  isTableOfContents,
  parseFrontMatter,
  parseTopicEntries,
  termCoverage,
  unescapeMarkdown,
  weightedTermCoverage,
} from "../src/domain/research/retrieval/docs-text";
import {
  HttpServiceNowDocsProvider,
  parseLlmsIndex,
} from "../src/domain/research/providers/servicenow-docs-provider";
import { defaultServiceNowDomainConfig } from "../src/domain/research/config";
import {
  assessGroundedness,
  verifyServiceNowAnswer,
} from "../src/domain/research/answer-verifier";
import { isServiceNowDomainQuery } from "../src/domain/research/retrieval/query-classifier";
import { runResearchPipeline } from "../src/domain/research/pipeline";
import type { ServiceNowEvidence } from "../src/domain/research/schemas/evidence";

const RELEASE = "australia";
const BASE = `https://raw.githubusercontent.com/ServiceNow/ServiceNowDocs/${RELEASE}/markdown`;

const LLMS_TXT = `# ServiceNow Product Documentation

For human-readable docs visit https://www.servicenow.com/docs

## Documents

- [Extend ServiceNow AI Platform capabilities](${BASE}/servicenow-platform/index.md)
- [IT Service Management](${BASE}/it-service-management/index.md)
`;

const PLATFORM_INDEX = `---
title: Australia Extend capabilities
doc_type: toc
---

# Australia Extend capabilities

- [Configuration Management Database (CMDB)](${BASE}/servicenow-platform/cmdb/c_ITILConfigurationManagement.md) -- Use the CMDB application to build logical representations of assets.
  - [CI relationships in the CMDB](${BASE}/servicenow-platform/cmdb/c_CIRelationships.md) -- The CMDB helps you track the relationships between configuration items \\(CIs\\).
  - [Configure a data table](${BASE}/servicenow-platform/tables/configure-table.md) -- Configure a table to capture the data your application needs.
  - [Configure the table dictionary](${BASE}/servicenow-platform/tables/dictionary.md) -- Configure dictionary entries for a table.
`;

const ITSM_INDEX = `---
title: Australia IT Service Management
doc_type: toc
---

- [Define assignment rules](${BASE}/it-service-management/assignment-rules.md) -- Configure a table of assignment rules for an incident.
`;

const CI_RELATIONSHIPS_DOC = `---
title: CI relationships in the CMDB
locale: en-US
release: australia
bundle: platform
doc_type: concept
canonical_url: https://www.servicenow.com/docs/cmdb/ci-relationships
---

# CI relationships in the CMDB

The CMDB, in contrast to a static asset list, tracks the relationships between
configuration items \\(CIs\\). A relationship record links a parent CI to a child
CI with a relationship type.

## Dependent and non-dependent relationships

Dependent relationships, such as Runs on, are used by the Identification and
Reconciliation Engine to identify dependent CIs.
`;

const CONFIGURE_TABLE_DOC = `---
title: Configure a data table
doc_type: task
---

# Configure a data table

Configure a table to capture the data your application needs. Add fields and
choose a label for the table.
`;

/** Serves the fixture corpus; any other URL is a 404, as a real miss would be. */
function fixtureFetch(): typeof fetch {
  const files: Record<string, string> = {
    [`https://raw.githubusercontent.com/ServiceNow/ServiceNowDocs/${RELEASE}/llms.txt`]: LLMS_TXT,
    [`${BASE}/servicenow-platform/index.md`]: PLATFORM_INDEX,
    [`${BASE}/it-service-management/index.md`]: ITSM_INDEX,
    [`${BASE}/servicenow-platform/cmdb/c_CIRelationships.md`]: CI_RELATIONSHIPS_DOC,
    [`${BASE}/servicenow-platform/cmdb/c_ITILConfigurationManagement.md`]: `---\ntitle: Configuration Management Database (CMDB)\n---\n\nThe CMDB stores configuration items and the relationships between them.`,
    [`${BASE}/servicenow-platform/tables/configure-table.md`]: CONFIGURE_TABLE_DOC,
    [`${BASE}/servicenow-platform/tables/dictionary.md`]: `---\ntitle: Configure the table dictionary\n---\n\nConfigure dictionary entries for a table.`,
    [`${BASE}/it-service-management/assignment-rules.md`]: `---\ntitle: Define assignment rules\n---\n\nConfigure a table of assignment rules for an incident.`,
  };

  return (async (input: RequestInfo | URL) => {
    const url = String(input);
    const body = files[url];
    return body
      ? new Response(body, { status: 200 })
      : new Response("not found", { status: 404 });
  }) as typeof fetch;
}

function provider() {
  return new HttpServiceNowDocsProvider(
    defaultServiceNowDomainConfig(),
    fixtureFetch()
  );
}

describe("documentation text handling", () => {
  it("keeps YAML front matter out of the body", () => {
    const { meta, body } = parseFrontMatter(CI_RELATIONSHIPS_DOC);

    expect(meta.title).toBe("CI relationships in the CMDB");
    expect(meta.doc_type).toBe("concept");
    expect(meta.canonical_url).toBe("https://www.servicenow.com/docs/cmdb/ci-relationships");
    expect(body.startsWith("# CI relationships in the CMDB")).toBe(true);
    expect(body).not.toContain("locale:");
    expect(body).not.toContain("---");
  });

  it("recognises a navigation page so it is never offered as an answer", () => {
    const toc = parseFrontMatter(PLATFORM_INDEX);
    const concept = parseFrontMatter(CI_RELATIONSHIPS_DOC);

    expect(isTableOfContents(toc.meta, toc.body)).toBe(true);
    expect(isTableOfContents(concept.meta, concept.body)).toBe(false);
  });

  it("reads the topic entries and descriptions out of a publication index", () => {
    const entries = parseTopicEntries(PLATFORM_INDEX);

    expect(entries).toHaveLength(4);
    const relationships = entries.find((e) => e.title.startsWith("CI relationships"));
    expect(relationships?.url).toContain("c_CIRelationships.md");
    expect(relationships?.description).toContain("(CIs)");
    expect(relationships?.depth).toBe(1);
  });

  it("scores whole words, not substrings", () => {
    // Substring matching let "work" hit "build-workflows", which is how a
    // question about CI relationships retrieved the Build workflows publication.
    expect(termCoverage("Build workflows and integrations", ["work"])).toBe(0);
    expect(termCoverage("Work with the CMDB", ["work"])).toBe(1);
    expect(termCoverage("Build workflows and integrations", ["relationship"])).toBe(0);
    // A trailing plural is still the same word.
    expect(termCoverage("CI relationships in the CMDB", ["relationship"])).toBe(1);
  });

  it("drops the question's filler so distinctive terms decide the match", () => {
    const { terms, phrases } = buildDocsQueryTerms(
      "What is the CMDB and how do CI relationships work?"
    );

    expect(terms).toContain("cmdb");
    expect(terms).toContain("ci");
    expect(terms).toContain("relationships");
    expect(terms).not.toContain("what");
    expect(terms).not.toContain("work");
    expect(phrases).toContain("ci relationships");
  });

  it("weights a term by how rare it is in the corpus being searched", () => {
    const corpus = [
      "Configure a data table",
      "Configure the table dictionary",
      "Configure a table field",
      "CI relationships in the CMDB",
    ];
    const weights = computeTermWeights(corpus, ["configure", "cmdb"]);

    expect(weights.get("cmdb")!).toBeGreaterThan(weights.get("configure")!);
    // A page sharing only the corpus's commonest word is a weak match.
    expect(
      weightedTermCoverage("Configure a data table", ["configure", "cmdb"], weights)
    ).toBeLessThan(0.4);
  });

  it("extracts the matching section instead of the first N bytes", () => {
    const { body } = parseFrontMatter(CI_RELATIONSHIPS_DOC);
    const passage = extractPassage(body, ["dependent", "reconciliation"], []);

    expect(passage.heading).toBe("Dependent and non-dependent relationships");
    expect(passage.text).toContain("Reconciliation Engine");
    expect(passage.text).not.toContain("doc_type");
  });

  it("undoes the documentation's punctuation escaping", () => {
    expect(unescapeMarkdown("GlideRecord - next\\(\\)")).toBe("GlideRecord - next()");
    expect(unescapeMarkdown("encoded\\_query")).toBe("encoded_query");
  });

  it("only indexes the publication list, not the preamble links", () => {
    const entries = parseLlmsIndex(LLMS_TXT);

    expect(entries).toHaveLength(2);
    expect(entries.every((e) => e.url.includes("/markdown/"))).toBe(true);
  });
});

describe("product documentation retrieval", () => {
  it("reaches a topic page rather than a publication table of contents", async () => {
    const evidence = await provider().search({
      query: "What is the CMDB and how do CI relationships work?",
      modules: ["cmdb"],
      releaseFamily: RELEASE,
      limit: 4,
    });

    expect(evidence.length).toBeGreaterThan(0);
    const top = evidence[0]!;
    expect(top.title).toContain("CI relationships");
    expect(top.sourceReference).toContain("c_CIRelationships.md");
    expect(top.canonicalUrl).toBe("https://www.servicenow.com/docs/cmdb/ci-relationships");
    // No index page, and no front matter anywhere near the answer surface.
    expect(evidence.some((e) => e.sourceReference.endsWith("/index.md"))).toBe(false);
    expect(top.content).not.toContain("doc_type");
    expect(top.snippet).toBeTruthy();
    expect(top.snippet).not.toContain("locale:");
    expect(top.retrievalRelevance ?? 0).toBeGreaterThan(0.5);
  });

  it("returns nothing when the question only shares common words", async () => {
    const evidence = await provider().search({
      query: "How do I configure the flux capacitor table in ServiceNow?",
      modules: ["platform"],
      releaseFamily: RELEASE,
      limit: 4,
    });

    const best = Math.max(0, ...evidence.map((e) => e.retrievalRelevance ?? 0));
    expect(best).toBeLessThan(0.3);
  });
});

describe("domain scope", () => {
  it("routes an ACL question about a table into the domain", () => {
    expect(isServiceNowDomainQuery("How do ACLs evaluate on a table?")).toBe(true);
  });

  it("still turns away a question with no platform signal", () => {
    expect(isServiceNowDomainQuery("What is the best pizza in Naples?")).toBe(false);
    expect(isServiceNowDomainQuery("How do I set an ACL on my NFS mount?")).toBe(false);
  });
});

function evidenceFixture(overrides: Partial<ServiceNowEvidence> = {}): ServiceNowEvidence {
  return {
    id: "docs_1",
    sourceType: "product_documentation",
    title: "CI relationships in the CMDB",
    content: "The CMDB tracks relationships between configuration items.",
    snippet: "The CMDB tracks relationships between configuration items.",
    sourceReference: `${BASE}/servicenow-platform/cmdb/c_CIRelationships.md`,
    retrievedAt: new Date().toISOString(),
    authorityScore: 0.92,
    relevanceScore: 0.8,
    retrievalRelevance: 0.8,
    freshnessScore: 0.85,
    taskScoped: false,
    containsSensitiveData: false,
    ...overrides,
  };
}

function draftFixture() {
  return {
    summary: "1 passage matched.",
    directAnswer: "1 passage matched.",
    warnings: [] as string[],
    assumptions: [] as string[],
    evidence: [{ evidenceId: "docs_1", claim: "CI relationships in the CMDB" }],
    confidence: "medium" as const,
    requiresInstanceValidation: false,
  };
}

describe("answer checks", () => {
  it("reports source availability and relevance separately", () => {
    const result = verifyServiceNowAnswer({
      draft: draftFixture(),
      evidence: [evidenceFixture()],
      config: defaultServiceNowDomainConfig(),
    });

    expect(result.checks.sourcesFound).toEqual({ count: 1, ok: true });
    expect(result.checks.relevance.verdict).toBe("strong");
  });

  it("does not call a written answer verified when no answer was written", () => {
    const result = verifyServiceNowAnswer({
      draft: draftFixture(),
      evidence: [evidenceFixture()],
      config: defaultServiceNowDomainConfig(),
    });

    expect(result.checks.groundedness.status).toBe("not_applicable");
    expect(result.checks.completeness.status).toBe("not_assessed");
  });

  it("withholds the answer when the sources are not relevant enough", () => {
    const result = verifyServiceNowAnswer({
      draft: draftFixture(),
      evidence: [evidenceFixture({ retrievalRelevance: 0.12, relevanceScore: 0.12 })],
      config: defaultServiceNowDomainConfig(),
    });

    expect(result.ok).toBe(false);
    expect(result.citationCount).toBe(0);
    expect(result.answer.directAnswer).toMatch(/could not confirm/i);
  });

  it("flags a written sentence that wandered away from its sources", () => {
    const { groundedness, claims } = assessGroundedness(
      "The CMDB tracks relationships between configuration items. Summaverick guarantees a 40% reduction in outages for every customer.",
      [evidenceFixture()]
    );

    expect(groundedness.status).toBe("checked");
    expect(groundedness.sentences).toBe(2);
    expect(groundedness.grounded).toBe(1);
    expect(groundedness.unsupported[0]).toContain("guarantees");
    expect(claims.filter((c) => c.verdict === "supported")).toHaveLength(1);
  });
});

describe("pipeline answer modes", () => {
  const config = defaultServiceNowDomainConfig({
    sdk: { enabled: false, minimumExplainVersion: "4.6.0", minimumQueryVersion: "4.8.0" },
  });

  it("labels a run with no model as source passages, not as an answer", async () => {
    const result = await runResearchPipeline({
      query: "What is the CMDB and how do CI relationships work?",
      configOverride: config,
      fetchImpl: fixtureFetch(),
    });

    expect(result.mode).toBe("source_passages");
    expect(result.trace.llm.reason).toBe("not_configured");
    expect(result.sources.length).toBeGreaterThan(0);
    expect(result.sources[0]!.snippet).not.toContain("doc_type");
    expect(result.trace.checks?.groundedness.status).toBe("not_applicable");
    // The old draft claimed "here is the evidence-backed answer for: <question>".
    expect(result.answer).not.toMatch(/evidence-backed answer/i);
  });

  it("labels an out-of-scope question instead of retrieving anything", async () => {
    const result = await runResearchPipeline({
      query: "What is the best pizza in Naples?",
      configOverride: config,
      fetchImpl: fixtureFetch(),
    });

    expect(result.mode).toBe("out_of_scope");
    expect(result.sources).toHaveLength(0);
    expect(result.trace.layers).toHaveLength(0);
  });

  it("reports insufficient evidence rather than stitching weak matches", async () => {
    const result = await runResearchPipeline({
      query: "How do I configure the flux capacitor table in ServiceNow?",
      configOverride: config,
      fetchImpl: fixtureFetch(),
    });

    expect(result.mode).toBe("insufficient_evidence");
    expect(result.sources).toHaveLength(0);
    expect(
      result.trace.evidenceGates.find((g) => g.gate === "insufficient_evidence_fallback")
        ?.fired
    ).toBe(true);
  });

  it("degrades to an unavailable state when a layer cannot be reached", async () => {
    const failing = (async () => new Response("boom", { status: 503 })) as typeof fetch;
    const result = await runResearchPipeline({
      query: "What is the CMDB and how do CI relationships work?",
      configOverride: config,
      fetchImpl: failing,
    });

    expect(result.mode).toBe("sources_unavailable");
    expect(result.sources).toHaveLength(0);
    expect(result.trace.layerErrors[0]?.source).toBe("product_docs");
    expect(result.answer).toMatch(/could not be reached/i);
  });
});
