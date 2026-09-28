import { createHash } from "node:crypto";
import { createId, nowIso, type TraceSink } from "../../../core/types.js";
import type { ServiceNowDomainConfig } from "../config.js";
import type { ServiceNowEvidence } from "../schemas/evidence.js";
import { wrapUntrustedEvidence } from "../schemas/evidence.js";
import { sanitizeEvidenceContent } from "../security/prompt-injection.js";
import type {
  DocumentationRefreshResult,
  ServiceNowDocsIndex,
  ServiceNowDocsIndexEntry,
  ServiceNowDocsSearchInput,
  ServiceNowDocument,
  ServiceNowDocumentReference,
} from "../types.js";

export interface ServiceNowDocsProvider {
  loadIndex(releaseFamily: string): Promise<ServiceNowDocsIndex>;
  search(input: ServiceNowDocsSearchInput): Promise<ServiceNowEvidence[]>;
  fetchDocument(reference: ServiceNowDocumentReference): Promise<ServiceNowDocument>;
  refresh(releaseFamily: string): Promise<DocumentationRefreshResult>;
}

const MODULE_TO_PUBLICATION: Record<string, string[]> = {
  itsm: ["it-service-management"],
  cmdb: ["it-operations-management", "now-platform"],
  itom: ["it-operations-management"],
  irm: ["governance-risk-compliance"],
  grc: ["governance-risk-compliance"],
  spm: ["it-business-management"],
  csm: ["customer-service-management"],
  hrsd: ["employee-service-management"],
  secops: ["security-management"],
  app_engine: ["application-development", "hyperautomation-low-code"],
  flow_designer: ["build-workflows"],
  integrationhub: ["integrate-applications"],
  ui_builder: ["platform-user-interface"],
  platform: ["platform-administration", "now-platform", "servicenow-platform"],
};

export class HttpServiceNowDocsProvider implements ServiceNowDocsProvider {
  private readonly indexCache = new Map<string, ServiceNowDocsIndex>();
  private readonly documentCache = new Map<string, ServiceNowDocument>();
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
    const tokens = tokenize(input.query);
    const modulePubs = (input.modules ?? [])
      .flatMap((m) => MODULE_TO_PUBLICATION[m] ?? [])
      .map((p) => p.toLowerCase());

    const scored = index.entries
      .map((entry) => {
        const hay = `${entry.title} ${entry.publication ?? ""} ${entry.url}`.toLowerCase();
        const keywordScore = scoreKeyword(hay, tokens);
        const moduleBoost =
          modulePubs.length &&
          modulePubs.some((p) => (entry.publication || "").includes(p) || hay.includes(p))
            ? 0.15
            : 0;
        const headingBoost = tokens.some((t) => entry.title.toLowerCase().includes(t))
          ? 0.1
          : 0;
        return {
          entry,
          score: keywordScore + moduleBoost + headingBoost,
        };
      })
      .filter((s) => s.score > 0)
      .sort((a, b) => b.score - a.score)
      .slice(0, Math.max(1, input.limit));

    this.trace?.emit({
      name: "servicenow.docs.searched",
      timestamp: nowIso(),
      attributes: {
        releaseFamily,
        query: input.query.slice(0, 120),
        hits: scored.length,
      },
    });

    const evidence: ServiceNowEvidence[] = [];
    for (const hit of scored.slice(0, Math.min(5, input.limit))) {
      try {
        const doc = await this.fetchDocument({
          releaseFamily,
          pathOrUrl: hit.entry.url,
          title: hit.entry.title,
        });
        evidence.push({
          id: createId("docs"),
          sourceType: "product_documentation",
          title: doc.title,
          content: wrapUntrustedEvidence(sanitizeEvidenceContent(doc.content.slice(0, 8000))),
          sourceReference: doc.path,
          canonicalUrl: doc.canonicalUrl,
          releaseFamily,
          module: hit.entry.module || hit.entry.publication,
          retrievedAt: nowIso(),
          authorityScore: 0.92,
          relevanceScore: Math.min(1, hit.score),
          freshnessScore: 0.85,
          taskScoped: false,
          containsSensitiveData: false,
        });
      } catch {
        evidence.push({
          id: createId("docs"),
          sourceType: "product_documentation",
          title: hit.entry.title,
          content: wrapUntrustedEvidence(
            sanitizeEvidenceContent(
              `Index hit only (body fetch failed): ${hit.entry.title}\n${hit.entry.url}`
            )
          ),
          sourceReference: hit.entry.url,
          canonicalUrl: hit.entry.url,
          releaseFamily,
          module: hit.entry.publication,
          retrievedAt: nowIso(),
          authorityScore: 0.7,
          relevanceScore: Math.min(1, hit.score * 0.8),
          freshnessScore: 0.7,
          taskScoped: false,
          containsSensitiveData: false,
        });
      }
    }
    return evidence;
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
    const title =
      reference.title ||
      content.match(/^title:\s*["']?(.+?)["']?\s*$/m)?.[1] ||
      content.match(/^#\s+(.+)$/m)?.[1] ||
      url.split("/").pop() ||
      "ServiceNow document";
    const canonicalUrl =
      content.match(/^canonical_url:\s*["']?(.+?)["']?\s*$/m)?.[1] || url;
    const updatedAt = content.match(/^last_updated:\s*["']?(.+?)["']?\s*$/m)?.[1];
    const contentHash = createHash("sha256").update(content).digest("hex").slice(0, 16);

    const doc: ServiceNowDocument = {
      title: title.trim(),
      content,
      canonicalUrl,
      releaseFamily: reference.releaseFamily,
      path: url,
      contentHash,
      updatedAt,
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

export function parseLlmsIndex(text: string): ServiceNowDocsIndexEntry[] {
  const entries: ServiceNowDocsIndexEntry[] = [];
  const linkRe = /\[([^\]]+)\]\((https?:\/\/[^)]+)\)/g;
  let match: RegExpExecArray | null;
  while ((match = linkRe.exec(text)) !== null) {
    const title = match[1]!.trim();
    const url = match[2]!.trim();
    const publication =
      url.match(/\/markdown\/([^/]+)\//)?.[1] ||
      url.split("/").slice(-2, -1)[0];
    entries.push({
      title,
      url,
      publication,
      module: publication,
    });
  }
  return entries;
}

function tokenize(query: string): string[] {
  return query
    .toLowerCase()
    .split(/[^a-z0-9_]+/)
    .filter((t) => t.length > 2);
}

function scoreKeyword(haystack: string, tokens: string[]): number {
  if (!tokens.length) return 0;
  let hits = 0;
  for (const t of tokens) {
    if (haystack.includes(t)) hits += 1;
  }
  return hits / tokens.length;
}
