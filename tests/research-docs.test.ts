import { describe, expect, it } from "vitest";
import {
  HttpServiceNowDocsProvider,
  extractSnippet,
  isTableOfContents,
  parseDocIndexEntries,
  rankPages,
  readableBody,
  selectPublications,
  tokenize,
  unescapeMarkdown,
} from "../src/domain/research/providers/servicenow-docs-provider";
import { defaultServiceNowDomainConfig } from "../src/domain/research/config";
import { classifyServiceNowIntent, isServiceNowDomainQuery } from "../src/domain/research/retrieval/query-classifier";
import { researchAnswerMode } from "../src/domain/research/pipeline";

/*
 * The reproduction from the 18 September review: the CMDB sample question came
 * back as raw YAML front matter from four publication table-of-contents pages,
 * because llms.txt lists only publication indexes and the keyword score was
 * dominated by stop words. These fixtures mirror the real files.
 */
const LLMS_TXT = `# ServiceNow Product Documentation

## Documents

- [API implementation and reference](https://docs.test/markdown/api-reference/index.md)
- [IT Operations Management](https://docs.test/markdown/it-operations-management/index.md)
- [Now Platform](https://docs.test/markdown/servicenow-platform/index.md)
- [Build workflows](https://docs.test/markdown/build-workflows/index.md)
`;

const PLATFORM_INDEX = `---
title: Australia Now Platform
doc_type: toc
---

# Australia Now Platform

- [Configuration Management Database \\(CMDB\\)](https://docs.test/markdown/servicenow-platform/cmdb.md) -- Learn about the CMDB.
  - [CI relationships in the CMDB](https://docs.test/markdown/servicenow-platform/ci-relationships.md) -- How configuration items relate to one another.
  - [Table administration](https://docs.test/markdown/servicenow-platform/tables.md) -- Administer tables and columns.
  - [Assessments](https://docs.test/markdown/servicenow-platform/assessments.md) -- Evaluate and score records from any table.
  - [Access control list rules](https://docs.test/markdown/servicenow-platform/acl-rules.md) -- How ACL rules are evaluated when a user opens a record.
`;

const CI_RELATIONSHIPS_DOC = `---
title: CI relationships in the CMDB
locale: en-US
canonical_url: "https://www.servicenow.com/docs/r/platform/c\\_CIRelationships.html"
doc_type: concept
---

# CI relationships in the CMDB

The CMDB tracks the configuration items in your system and the relationships between them. A relationship record joins a parent CI to a child CI and gives the relationship a type, so you can see what a service depends on.

Relationship types are defined in the CI Relationship table.
`;

function stubFetch(routes: Record<string, string>): typeof fetch {
  return (async (input: RequestInfo | URL) => {
    const url = String(input);
    const body = routes[url];
    if (body == null) {
      return new Response("not found", { status: 404 });
    }
    return new Response(body, { status: 200 });
  }) as typeof fetch;
}

describe("ServiceNow docs retrieval", () => {
  it("reaches page-level documents instead of publication indexes", async () => {
    const provider = new HttpServiceNowDocsProvider(
      defaultServiceNowDomainConfig({
        documentation: {
          enabled: true,
          repository: "https://docs.test",
          releaseFamily: "australia",
          indexUrl: "https://docs.test/llms.txt",
          refreshIntervalHours: 24,
        },
      }),
      stubFetch({
        "https://docs.test/llms.txt": LLMS_TXT,
        "https://docs.test/markdown/servicenow-platform/index.md": PLATFORM_INDEX,
        "https://docs.test/markdown/servicenow-platform/ci-relationships.md": CI_RELATIONSHIPS_DOC,
      })
    );

    const evidence = await provider.search({
      query: "What is the CMDB and how do CI relationships work?",
      modules: ["cmdb"],
      limit: 4,
    });

    expect(evidence).toHaveLength(1);
    const [hit] = evidence;
    expect(hit!.title).toBe("CI relationships in the CMDB");
    // No front matter, no leading heading, no markdown escapes.
    expect(hit!.content).not.toContain("doc_type:");
    expect(hit!.content).not.toContain("locale: en-US");
    expect(hit!.snippet).toMatch(/^The CMDB tracks the configuration items/);
    expect(hit!.canonicalUrl).toBe(
      "https://www.servicenow.com/docs/r/platform/c_CIRelationships.html"
    );
  });

  it("returns nothing rather than quoting a table of contents", async () => {
    const provider = new HttpServiceNowDocsProvider(
      defaultServiceNowDomainConfig({
        documentation: {
          enabled: true,
          repository: "https://docs.test",
          releaseFamily: "australia",
          indexUrl: "https://docs.test/llms.txt",
          refreshIntervalHours: 24,
        },
      }),
      stubFetch({
        "https://docs.test/llms.txt": LLMS_TXT,
        "https://docs.test/markdown/servicenow-platform/index.md": PLATFORM_INDEX,
      })
    );

    const evidence = await provider.search({
      query: "What is the CMDB and how do CI relationships work?",
      modules: ["cmdb"],
      limit: 4,
    });
    expect(evidence).toEqual([]);
  });

  it("drops index links and keeps page entries with their descriptions", () => {
    const entries = parseDocIndexEntries(PLATFORM_INDEX, "servicenow-platform");
    expect(entries.map((e) => e.title)).toEqual([
      "Configuration Management Database (CMDB)",
      "CI relationships in the CMDB",
      "Table administration",
      "Assessments",
      "Access control list rules",
    ]);
    expect(entries[1]!.description).toBe("How configuration items relate to one another.");
    expect(entries.every((e) => !e.url.endsWith("/index.md"))).toBe(true);
  });

  it("ranks on the question's distinctive word, not its common ones", () => {
    const pages = parseDocIndexEntries(PLATFORM_INDEX, "servicenow-platform");
    const ranked = rankPages(pages, tokenize("How do ACLs evaluate on a table?"));
    expect(ranked[0]!.entry.title).toBe("Access control list rules");
    // "Assessments" evaluates records on any table but has nothing to do with
    // ACLs, so shared common words must not make it a match.
    expect(ranked.map((r) => r.entry.title)).not.toContain("Assessments");
  });

  it("stems plurals and keeps short ServiceNow terms", () => {
    expect(tokenize("How do ACLs evaluate on a table?")).toEqual([
      "acl",
      "evaluate",
      "table",
    ]);
    expect(tokenize("What is the CMDB and how do CI relationships work?")).toEqual([
      "cmdb",
      "ci",
      "relationship",
    ]);
  });

  it("picks publications from the classifier modules", () => {
    const entries = [
      { title: "Now Platform", url: "https://docs.test/markdown/servicenow-platform/index.md", publication: "servicenow-platform" },
      { title: "Build workflows", url: "https://docs.test/markdown/build-workflows/index.md", publication: "build-workflows" },
    ];
    const picked = selectPublications(entries, tokenize("CI relationships"), ["cmdb"]);
    expect(picked.map((p) => p.publication)).toContain("servicenow-platform");
  });

  it("recognises a table of contents by front matter and by shape", () => {
    expect(isTableOfContents(PLATFORM_INDEX)).toBe(true);
    expect(isTableOfContents(CI_RELATIONSHIPS_DOC)).toBe(false);
  });

  it("cleans a document down to readable prose", () => {
    const body = readableBody(CI_RELATIONSHIPS_DOC);
    expect(body).not.toContain("---");
    expect(body).not.toContain("# CI relationships in the CMDB");
    expect(extractSnippet(body, ["relationship"])).toContain("relationships between them");
  });

  it("removes markdown escaping from titles and links", () => {
    expect(unescapeMarkdown("CMDB Identification \\(IRE\\)")).toBe("CMDB Identification (IRE)");
    expect(unescapeMarkdown("c\\_CIRelationships.html")).toBe("c_CIRelationships.html");
  });
});

describe("ServiceNow domain scope", () => {
  it("keeps every advertised sample question inside the domain", () => {
    const samples = [
      "How do I use GlideRecord to query the incident table?",
      "How do I define a Business Rule with the Fluent SDK?",
      "What is the CMDB and how do CI relationships work?",
      "How do ACLs evaluate on a table?",
    ];
    for (const q of samples) {
      expect(isServiceNowDomainQuery(q), q).toBe(true);
    }
  });

  it("still turns away a question that is not about ServiceNow", () => {
    expect(isServiceNowDomainQuery("How do I bake sourdough bread?")).toBe(false);
  });

  it("plans product documentation for a Fluent SDK question", () => {
    // The SDK explain layer needs a checked-out Fluent project, which a Worker
    // never has, so an SDK question must still be able to reach the docs.
    const cls = classifyServiceNowIntent("How do I define a Business Rule with the Fluent SDK?");
    expect(cls.intent).toBe("fluent_sdk");
    expect(cls.requiresProductDocs).toBe(true);
  });
});

describe("research answer mode", () => {
  it("is knowable before a question is asked", () => {
    expect(researchAnswerMode({ answerModelConfigured: false })).toBe("source_results");
    expect(researchAnswerMode({ answerModelConfigured: true })).toBe("model_answer");
  });
});
