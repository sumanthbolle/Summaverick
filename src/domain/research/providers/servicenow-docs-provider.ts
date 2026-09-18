/**
 * Layer 2 — ServiceNow product documentation provider. This is the layer that
 * fully runs in the Worker: it fetches ServiceNowDocs over HTTPS via the
 * injected `fetchImpl`.
 *
 * Retrieval is two hops, because the repository's shape requires it. The
 * `llms.txt` index lists one `index.md` per publication and nothing else, so a
 * one-hop search can only ever return a table of contents. Hop 1 picks the
 * publications worth opening (module mapping plus title match); hop 2 reads
 * those publication indexes, scores their topic entries against the question,
 * and fetches the topic pages themselves. Front matter is parsed as metadata
 * and the answer surface only ever sees the matching passage.
 */
import { createId, nowIso, type TraceSink } from "../core/types";
import { hashHex } from "../runtime/node-shims";
import type { ServiceNowDomainConfig } from "../config";
import type { ServiceNowEvidence } from "../schemas/evidence";
import { wrapUntrustedEvidence } from "../schemas/evidence";
import { sanitizeEvidenceContent } from "../security/prompt-injection";
import {
  buildDocsQueryTerms,
  computeTermWeights,
  extractPassage,
  isTableOfContents,
  parseFrontMatter,
  parseTopicEntries,
  phraseHits,
  plainSnippet,
  termCoverage,
  unescapeMarkdown,
  weightedTermCoverage,
  type DocsTopicEntry,
} from "../retrieval/docs-text";
import type {
  DocumentationRefreshResult,
  ServiceNowDocsIndex,
  ServiceNowDocsIndexEntry,
  ServiceNowDocsSearchInput,
  ServiceNowDocument,
  ServiceNowDocumentReference,
} from "../types";

export interface ServiceNowDocsProvider {
  loadIndex(releaseFamily: string): Promise<ServiceNowDocsIndex>;
  search(input: ServiceNowDocsSearchInput): Promise<ServiceNowEvidence[]>;
  fetchDocument(reference: ServiceNowDocumentReference): Promise<ServiceNowDocument>;
  refresh(releaseFamily: string): Promise<DocumentationRefreshResult>;
}

/**
 * Module → publication slugs, checked against the publication list actually
 * published in `llms.txt` for the current release family.
 */
const MODULE_TO_PUBLICATION: Record<string, string[]> = {
  itsm: ["it-service-management"],
  cmdb: ["servicenow-platform", "it-operations-management", "platform-administration"],
  itom: ["it-operations-management", "cloud-observability"],
  irm: ["governance-risk-compliance"],
  grc: ["governance-risk-compliance"],
  spm: ["it-business-management"],
  csm: ["customer-service-management", "customer-relationship-management"],
  hrsd: ["employee-service-management"],
  secops: ["security-management", "platform-security"],
  app_engine: ["application-development", "hyperautomation-low-code"],
  flow_designer: ["build-workflows"],
  integrationhub: ["integrate-applications"],
  ui_builder: ["platform-user-interface"],
  fluent_sdk: ["application-development", "api-reference"],
  api_reference: ["api-reference", "servicenow-platform"],
  access_control: ["platform-security", "servicenow-platform", "application-development"],
  platform: [
    "servicenow-platform",
    "application-development",
    "platform-administration",
    "platform-security",
  ],
};

/** How many publication indexes one question may open. */
const MAX_PUBLICATIONS = 4;
/** Hard ceiling on topic-page fetches, so one question cannot fan out. */
const MAX_TOPIC_FETCHES = 8;
/**
 * Minimum weighted match a topic entry needs before it is worth fetching, and
 * the share of the best candidate's score a weaker candidate must still reach.
 * The relative floor keeps the top cluster for a question that matches well
 * and drops the long tail behind one that barely matches at all.
 */
const MIN_TOPIC_SCORE = 0.18;
const RELATIVE_TOPIC_FLOOR = 0.55;

export class HttpServiceNowDocsProvider implements ServiceNowDocsProvider {
  private readonly indexCache = new Map<string, ServiceNowDocsIndex>();
  private readonly documentCache = new Map<string, ServiceNowDocument>();
  private readonly topicIndexCache = new Map<string, DocsTopicEntry[]>();
  private readonly refreshedAt = new Map<string, number>();

  constructor(
    private readonly config: ServiceNowDomainConfig,
    private readonly fetchImpl: typeof fetch = fetch,
    private readonly trace?: TraceSink
  ) {}

  async loadIndex(releaseFamily: string): Promise<ServiceNowDocsIndex> {
    const cached = this.indexCache.get(releaseFamily);
    const refreshed = this.refreshedAt.get(releaseFamily) ?? 0;
    const maxAgeMs = this.config.documentation.refreshIntervalHours * 3600_000;
    if (cached && Date.now() - refreshed < maxAgeMs) {
      return cached;
    }
    return this.refreshAndCache(releaseFamily);
  }

  async refresh(releaseFamily: string): Promise<DocumentationRefreshResult> {
    const index = await this.refreshAndCache(releaseFamily);
    return {
      releaseFamily,
      indexedCount: index.entries.length,
      refreshedAt: index.fetchedAt,
    };
  }

  async search(input: ServiceNowDocsSearchInput): Promise<ServiceNowEvidence[]> {
    const releaseFamily =
      input.releaseFamily || this.config.documentation.releaseFamily;
    const index = await this.loadIndex(releaseFamily);
    const { terms, phrases } = buildDocsQueryTerms(input.query);
    const limit = Math.max(1, input.limit);

    const publications = this.selectPublications(index, input.modules ?? [], terms, phrases);
    this.trace?.emit({
      name: "servicenow.docs.publications.selected",
      timestamp: nowIso(),
      attributes: {
        releaseFamily,
        terms: terms.join(","),
        publications: publications.map((p) => p.slug).join(","),
      },
    });

    if (!publications.length) return [];

    const candidates = await this.selectTopics(publications, releaseFamily, terms, phrases);
    this.trace?.emit({
      name: "servicenow.docs.topics.selected",
      timestamp: nowIso(),
      attributes: {
        candidates: candidates.length,
        best: candidates[0] ? `${candidates[0].entry.title} (${candidates[0].score})` : "none",
      },
    });

    const evidence: ServiceNowEvidence[] = [];
    let fetches = 0;
    for (const candidate of candidates) {
      if (evidence.length >= limit || fetches >= MAX_TOPIC_FETCHES) break;
      fetches += 1;
      const item = await this.topicEvidence(candidate, releaseFamily, terms, phrases);
      if (item) evidence.push(item);
    }

    this.trace?.emit({
      name: "servicenow.docs.searched",
      timestamp: nowIso(),
      attributes: {
        releaseFamily,
        query: input.query.slice(0, 120),
        fetched: fetches,
        hits: evidence.length,
      },
    });

    return evidence;
  }

  /** Hop 1 — which publication indexes are worth opening for this question. */
  private selectPublications(
    index: ServiceNowDocsIndex,
    modules: string[],
    terms: string[],
    phrases: string[]
  ): { entry: ServiceNowDocsIndexEntry; slug: string; score: number }[] {
    // Every detected module gets its primary publication before any module
    // gets its second. Walking module by module instead spent the publication
    // budget on one module's long tail — a GlideRecord question would open
    // three platform publications and never reach the API reference.
    const mapped = new Map<string, number>();
    const lists = modules.map((module) => MODULE_TO_PUBLICATION[module] ?? []);
    const depth = Math.max(0, ...lists.map((list) => list.length));
    for (let round = 0; round < depth; round += 1) {
      for (const list of lists) {
        const slug = list[round];
        if (slug && !mapped.has(slug)) mapped.set(slug, mapped.size);
      }
    }

    return index.entries
      .map((entry) => {
        const slug = publicationSlug(entry.url);
        const haystack = `${entry.title} ${slug.replace(/-/g, " ")}`;
        const rank = mapped.get(slug);
        const score =
          (rank === undefined ? 0 : Math.max(0.5, 0.8 - rank * 0.03)) +
          termCoverage(haystack, terms) * 0.6 +
          phraseHits(haystack, phrases) * 0.2;
        return { entry, slug, score };
      })
      .filter((candidate) => candidate.score > 0)
      .sort((a, b) => b.score - a.score)
      .slice(0, MAX_PUBLICATIONS);
  }

  /** Hop 2 — score the topic entries inside the selected publications. */
  private async selectTopics(
    publications: { entry: ServiceNowDocsIndexEntry; slug: string }[],
    releaseFamily: string,
    terms: string[],
    phrases: string[]
  ): Promise<TopicCandidate[]> {
    const candidates: TopicCandidate[] = [];

    for (const publication of publications) {
      let entries: DocsTopicEntry[];
      try {
        entries = await this.loadTopicIndex(publication.entry.url, releaseFamily);
      } catch {
        continue;
      }

      // Term weights come from this publication's own index, so the question's
      // distinctive words decide the ranking instead of the words that appear
      // on every page of the publication.
      const weights = computeTermWeights(
        entries.map((entry) => `${entry.title} ${entry.description}`),
        terms
      );

      for (const entry of entries) {
        const title = entry.title;
        const haystack = `${title} ${entry.description}`;
        const score =
          weightedTermCoverage(title, terms, weights) * 0.6 +
          weightedTermCoverage(haystack, terms, weights) * 0.3 +
          Math.min(2, phraseHits(haystack, phrases)) * 0.1;
        if (score < MIN_TOPIC_SCORE) continue;
        candidates.push({
          entry,
          publication: publication.slug,
          publicationTitle: publication.entry.title,
          score: round(score),
          weights,
        });
      }
    }

    const ordered = candidates.sort((a, b) => b.score - a.score);
    const floor = (ordered[0]?.score ?? 0) * RELATIVE_TOPIC_FLOOR;
    const seen = new Set<string>();
    return ordered
      .filter((candidate) => candidate.score >= floor)
      .filter((candidate) => {
        if (seen.has(candidate.entry.url)) return false;
        seen.add(candidate.entry.url);
        return true;
      });
  }

  /** Fetch one topic page and reduce it to the passage that matches. */
  private async topicEvidence(
    candidate: TopicCandidate,
    releaseFamily: string,
    terms: string[],
    phrases: string[]
  ): Promise<ServiceNowEvidence | null> {
    let doc: ServiceNowDocument;
    try {
      doc = await this.fetchDocument({
        releaseFamily,
        pathOrUrl: candidate.entry.url,
        title: candidate.entry.title,
      });
    } catch {
      return null;
    }

    // A navigation page cannot answer a question; it only points at the pages
    // that can, and those are already in the candidate list.
    if (isTableOfContents(doc.meta, doc.body)) return null;

    const passage = extractPassage(doc.body, terms, phrases);
    const text = passage.text || candidate.entry.description;
    if (!text) return null;

    const heading = passage.heading && passage.heading !== doc.title
      ? `${doc.title} › ${passage.heading}`
      : doc.title;
    // The passage carries most of the weight: it is what a visitor reads, and
    // weighting it by term distinctiveness is what separates a page about the
    // question from a page that merely shares the question's common words.
    const relevance = clamp01(
      candidate.score * 0.3 +
        weightedTermCoverage(`${heading} ${text}`, terms, candidate.weights) * 0.6 +
        Math.min(1, phraseHits(text, phrases) * 0.5) * 0.1
    );

    return {
      id: createId("docs"),
      sourceType: "product_documentation",
      title: heading,
      content: wrapUntrustedEvidence(sanitizeEvidenceContent(text)),
      snippet: plainSnippet(text),
      sourceReference: doc.path,
      canonicalUrl: doc.canonicalUrl,
      releaseFamily,
      module: candidate.publication,
      publicationTitle: candidate.publicationTitle,
      retrievedAt: nowIso(),
      authorityScore: 0.92,
      relevanceScore: relevance,
      retrievalRelevance: relevance,
      freshnessScore: 0.85,
      taskScoped: false,
      containsSensitiveData: false,
    };
  }

  private async loadTopicIndex(
    indexUrl: string,
    releaseFamily: string
  ): Promise<DocsTopicEntry[]> {
    const cached = this.topicIndexCache.get(indexUrl);
    if (cached) return cached;
    const doc = await this.fetchDocument({ releaseFamily, pathOrUrl: indexUrl });
    const entries = parseTopicEntries(doc.body);
    this.topicIndexCache.set(indexUrl, entries);
    return entries;
  }

  async fetchDocument(
    reference: ServiceNowDocumentReference
  ): Promise<ServiceNowDocument> {
    const cacheKey = `${reference.releaseFamily}|${reference.pathOrUrl}`;
    const cached = this.documentCache.get(cacheKey);
    if (cached) return cached;

    const url = reference.pathOrUrl.startsWith("http")
      ? reference.pathOrUrl
      : `https://raw.githubusercontent.com/ServiceNow/ServiceNowDocs/${reference.releaseFamily}/${reference.pathOrUrl}`;

    const res = await this.fetchImpl(url);
    if (!res.ok) {
      throw new Error(`Failed to fetch ServiceNowDocs document: HTTP ${res.status}`);
    }
    const content = await res.text();
    const { meta, body } = parseFrontMatter(content);
    const title =
      meta.title ||
      body.match(/^#\s+(.+)$/m)?.[1] ||
      reference.title ||
      url.split("/").pop() ||
      "ServiceNow document";

    const doc: ServiceNowDocument = {
      title: unescapeMarkdown(title.trim()),
      content,
      body,
      meta,
      canonicalUrl: meta.canonical_url || url,
      releaseFamily: reference.releaseFamily,
      path: url,
      contentHash: hashHex(content),
      updatedAt: meta.last_updated,
    };
    this.documentCache.set(cacheKey, doc);
    return doc;
  }

  private async refreshAndCache(
    releaseFamily: string
  ): Promise<ServiceNowDocsIndex> {
    const indexUrl =
      releaseFamily === this.config.documentation.releaseFamily
        ? this.config.documentation.indexUrl
        : `https://raw.githubusercontent.com/ServiceNow/ServiceNowDocs/${releaseFamily}/llms.txt`;

    const res = await this.fetchImpl(indexUrl);
    if (!res.ok) {
      throw new Error(`Failed to load ServiceNowDocs llms.txt: HTTP ${res.status}`);
    }
    const text = await res.text();
    const entries = parseLlmsIndex(text);
    const index: ServiceNowDocsIndex = {
      releaseFamily,
      fetchedAt: nowIso(),
      entries,
    };
    this.indexCache.set(releaseFamily, index);
    this.refreshedAt.set(releaseFamily, Date.now());
    return index;
  }
}

interface TopicCandidate {
  entry: DocsTopicEntry;
  publication: string;
  publicationTitle: string;
  score: number;
  /** Term weights from the publication this candidate came from. */
  weights: Map<string, number>;
}

export function parseLlmsIndex(text: string): ServiceNowDocsIndexEntry[] {
  const entries: ServiceNowDocsIndexEntry[] = [];
  const linkRe = /\[([^\]]+)\]\((https?:\/\/[^)]+)\)/g;
  let match: RegExpExecArray | null;
  while ((match = linkRe.exec(text)) !== null) {
    const title = match[1]!.trim();
    const url = match[2]!.trim();
    // Only the publication list under "## Documents" is retrievable; the
    // preamble links point at the human docs site and the GitHub repo.
    if (!url.includes("/markdown/")) continue;
    const publication = publicationSlug(url);
    entries.push({
      title,
      url,
      publication,
      module: publication,
    });
  }
  return entries;
}

function publicationSlug(url: string): string {
  return (
    url.match(/\/markdown\/([^/]+)\//)?.[1] ||
    url.split("/").slice(-2, -1)[0] ||
    ""
  );
}

function clamp01(n: number): number {
  return Math.max(0, Math.min(1, n));
}

function round(n: number): number {
  return Math.round(n * 1000) / 1000;
}
