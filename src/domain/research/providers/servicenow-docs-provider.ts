/**
 * Layer 2 — ServiceNow product documentation provider. Ported from research-agent.
 * This is the layer that FULLY runs in the Worker: it fetches the ServiceNowDocs
 * `llms.txt` index and individual docs over HTTPS via the injected `fetchImpl`.
 * Only change from the original port: `crypto.createHash("sha256")` for the
 * content hash is replaced by the non-security `hashHex` shim (cache/dedup key).
 *
 * Retrieval runs in two stages, because `llms.txt` lists one entry per
 * PUBLICATION (`markdown/<publication>/index.md`, `doc_type: toc`) and no
 * page-level documents at all:
 *
 *   1. pick the publications a question belongs to (module map + keywords);
 *   2. read those publications' index.md link lists, score the page-level
 *      entries, and fetch the pages themselves as evidence.
 *
 * A table-of-contents page is never returned as evidence. When no page clears
 * the relevance floor the provider returns nothing, so the caller can say it
 * found no specific documentation instead of quoting an index.
 */
import { createId, nowIso, type TraceSink } from "../core/types";
import { hashHex } from "../runtime/node-shims";
import type { ServiceNowDomainConfig } from "../config";
import type { ServiceNowEvidence } from "../schemas/evidence";
import { wrapUntrustedEvidence } from "../schemas/evidence";
import { sanitizeEvidenceContent } from "../security/prompt-injection";
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

/** One page-level document listed inside a publication's index.md. */
export interface DocsPageEntry {
  title: string;
  url: string;
  description: string;
  publication?: string;
}

const MODULE_TO_PUBLICATION: Record<string, string[]> = {
  itsm: ["it-service-management"],
  cmdb: ["servicenow-platform", "now-platform", "it-operations-management"],
  itom: ["it-operations-management"],
  irm: ["governance-risk-compliance"],
  grc: ["governance-risk-compliance"],
  spm: ["it-business-management", "strategic-portfolio-management"],
  csm: ["customer-service-management"],
  hrsd: ["employee-service-management"],
  secops: ["security-management"],
  app_engine: ["application-development", "hyperautomation-low-code"],
  flow_designer: ["build-workflows"],
  integrationhub: ["integrate-applications"],
  ui_builder: ["platform-user-interface"],
  platform: ["platform-administration", "now-platform", "servicenow-platform"],
  scripting: ["api-reference", "now-platform", "platform-administration"],
  fluent_sdk: ["application-development", "hyperautomation-low-code"],
  platform_security: ["platform-security", "now-platform", "servicenow-platform"],
};

/**
 * Words that appear in almost every documentation title or URL. Left in, they
 * made a question like "what is the CMDB and how do CI relationships work"
 * match every publication landing page, which is how a CMDB question came back
 * with the API reference index.
 */
const STOP_WORDS = new Set([
  "about", "after", "and", "any", "are", "been", "before", "between", "but",
  "can", "could", "did", "does", "doing", "explain", "find", "for", "from",
  "get", "give", "has", "have", "how", "into", "its", "let", "like", "make",
  "many", "may", "much", "must", "need", "not", "now", "one", "only", "our",
  "out", "over", "per", "servicenow", "should", "show", "some", "such", "than",
  "that", "the", "their", "them", "then", "there", "these", "they", "this",
  "tell", "use", "used", "uses", "using", "very", "want", "was", "way", "well",
  "were", "what", "when", "where", "which", "while", "who", "why", "will",
  "with", "within", "would", "you", "your", "work", "works", "working", "does",
]);

/**
 * Short words that carry real meaning in ServiceNow questions, so the
 * three-character floor does not drop them ("CI relationships" is a topic).
 */
const SHORT_TERMS = new Set(["ci", "ui", "ux", "va", "mid", "acl", "sla", "api", "rest", "cmdb"]);

/** A page must match at least this share of the question's content words. */
const RELEVANCE_FLOOR = 0.34;

export class HttpServiceNowDocsProvider implements ServiceNowDocsProvider {
  private readonly indexCache = new Map<string, ServiceNowDocsIndex>();
  private readonly documentCache = new Map<string, ServiceNowDocument>();
  private readonly pageIndexCache = new Map<string, DocsPageEntry[]>();
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
    // Expansions widen the net but must not outvote the question itself.
    const relatedTokens = tokenize((input.expansions ?? []).join(" ")).filter(
      (t) => !tokens.includes(t)
    );
    const limit = Math.max(1, Math.min(4, input.limit));

    const publications = selectPublications(
      index.entries,
      [...tokens, ...relatedTokens],
      input.modules ?? []
    );
    const pages = await this.collectPageCandidates(releaseFamily, publications);
    const scored = rankPages(pages, tokens, relatedTokens).slice(0, limit);

    this.trace?.emit({
      name: "servicenow.docs.searched",
      timestamp: nowIso(),
      attributes: {
        releaseFamily,
        query: input.query.slice(0, 120),
        publications: publications.map((p) => p.publication ?? p.title).join(","),
        pageCandidates: pages.length,
        hits: scored.length,
      },
    });

    const evidence: ServiceNowEvidence[] = [];
    for (const hit of scored) {
      try {
        const doc = await this.fetchDocument({
          releaseFamily,
          pathOrUrl: hit.entry.url,
          title: hit.entry.title,
        });
        // A link list is navigation, not an answer to anything.
        if (isTableOfContents(doc.content)) continue;
        const body = readableBody(doc.content);
        if (!body) continue;
        evidence.push({
          id: createId("docs"),
          sourceType: "product_documentation",
          title: doc.title,
          content: wrapUntrustedEvidence(sanitizeEvidenceContent(body.slice(0, 8000))),
          snippet: extractSnippet(body, tokens),
          sourceReference: doc.path,
          canonicalUrl: doc.canonicalUrl,
          releaseFamily,
          module: hit.entry.publication,
          retrievedAt: nowIso(),
          authorityScore: 0.92,
          relevanceScore: hit.score,
          freshnessScore: 0.85,
          taskScoped: false,
          containsSensitiveData: false,
        });
      } catch {
        // A page that will not load is a retrieval miss, not evidence. The
        // earlier port turned the failure into an "index hit only" document,
        // which is what surfaced raw index text in the answer.
        this.trace?.emit({
          name: "servicenow.docs.fetch_failed",
          timestamp: nowIso(),
          attributes: { url: hit.entry.url },
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
    const title = unescapeMarkdown(
      content.match(/^title:\s*["']?(.+?)["']?\s*$/m)?.[1] ||
        content.match(/^#\s+(.+)$/m)?.[1] ||
        reference.title ||
        url.split("/").pop() ||
        "ServiceNow document"
    );
    const canonicalUrl = unescapeMarkdown(
      content.match(/^canonical_url:\s*["']?(.+?)["']?\s*$/m)?.[1] || url
    );
    const updatedAt = content.match(/^last_updated:\s*["']?(.+?)["']?\s*$/m)?.[1];
    const contentHash = hashHex(content);

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

  /** Read the chosen publications' index.md files and pool their page entries. */
  private async collectPageCandidates(
    releaseFamily: string,
    publications: ServiceNowDocsIndexEntry[]
  ): Promise<DocsPageEntry[]> {
    const pooled: DocsPageEntry[] = [];
    for (const publication of publications) {
      const cacheKey = `${releaseFamily}|${publication.url}`;
      const cached = this.pageIndexCache.get(cacheKey);
      if (cached) {
        pooled.push(...cached);
        continue;
      }
      try {
        const res = await this.fetchImpl(publication.url);
        if (!res.ok) continue;
        const entries = parseDocIndexEntries(
          await res.text(),
          publication.publication ?? publication.module
        );
        this.pageIndexCache.set(cacheKey, entries);
        pooled.push(...entries);
      } catch {
        this.trace?.emit({
          name: "servicenow.docs.index_failed",
          timestamp: nowIso(),
          attributes: { url: publication.url },
        });
      }
    }
    return pooled;
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

/**
 * Page-level entries from a publication index: `- [Title](url) -- description`,
 * at any indentation depth. Index/TOC links are dropped, so the result only
 * contains documents that can carry an explanation.
 */
export function parseDocIndexEntries(
  text: string,
  publication?: string
): DocsPageEntry[] {
  const entries: DocsPageEntry[] = [];
  const seen = new Set<string>();
  const lineRe =
    /^[ \t]*[-*]\s+\[([^\]]+)\]\((https?:\/\/[^)\s]+)\)(?:\s*--\s*(.*))?$/gm;
  let match: RegExpExecArray | null;
  while ((match = lineRe.exec(text)) !== null) {
    const url = match[2]!.trim();
    if (/\/index\.md$/i.test(url) || seen.has(url)) continue;
    seen.add(url);
    entries.push({
      title: unescapeMarkdown(match[1]!.trim()),
      url,
      description: unescapeMarkdown((match[3] ?? "").trim()),
      publication: publication ?? url.match(/\/markdown\/([^/]+)\//)?.[1],
    });
  }
  return entries;
}

/**
 * Content words from a question: stop words and short fragments removed, and
 * plurals reduced to a stem so "ACLs" matches an "ACL rules" page. Matching is
 * substring-based, so the stem covers both forms.
 */
export function tokenize(query: string): string[] {
  const raw = query
    .toLowerCase()
    .split(/[^a-z0-9_]+/)
    .filter((t) => (t.length > 2 || SHORT_TERMS.has(t)) && !STOP_WORDS.has(t))
    .map(stem);
  return [...new Set(raw)];
}

function stem(token: string): string {
  if (token.length > 4 && token.endsWith("ies")) return `${token.slice(0, -3)}y`;
  if (token.length > 4 && /(ses|xes|ches|shes)$/.test(token)) return token.slice(0, -2);
  if (token.length > 3 && token.endsWith("s") && !token.endsWith("ss")) {
    return token.slice(0, -1);
  }
  return token;
}

/** Markdown escaping from the docs source, removed for display and matching. */
export function unescapeMarkdown(text: string): string {
  return text.replace(/\\([\\`*_{}[\]()#+\-.!])/g, "$1");
}

/**
 * Publications worth reading for this question: everything the classifier's
 * modules map to, plus any publication whose name matches a content word.
 */
export function selectPublications(
  entries: ServiceNowDocsIndexEntry[],
  tokens: string[],
  modules: string[],
  maxPublications = 3
): ServiceNowDocsIndexEntry[] {
  const wanted = new Set(
    modules.flatMap((m) => MODULE_TO_PUBLICATION[m] ?? []).map((p) => p.toLowerCase())
  );

  const scored = entries.map((entry) => {
    const publication = (entry.publication ?? "").toLowerCase();
    const haystack = `${entry.title} ${publication}`.toLowerCase();
    const moduleMatch = [...wanted].some(
      (p) => publication.includes(p) || p.includes(publication)
    );
    const keywordScore = tokens.length
      ? tokens.filter((t) => haystack.includes(t)).length / tokens.length
      : 0;
    return { entry, score: (moduleMatch ? 1 : 0) + keywordScore };
  });

  return scored
    .filter((s) => s.score > 0)
    .sort((a, b) => b.score - a.score)
    .slice(0, maxPublications)
    .map((s) => s.entry);
}

/**
 * Score page entries on the question's content words, weighting each word by
 * how rare it is in the candidate pool. Without that weighting, an ACL question
 * matched every page that happened to mention "table" or "evaluate". Title
 * matches count for more than description matches, and a page must clear the
 * relevance floor to be quoted at all.
 */
export function rankPages(
  pages: DocsPageEntry[],
  tokens: string[],
  relatedTokens: string[] = []
): { entry: DocsPageEntry; score: number }[] {
  if (!tokens.length || !pages.length) return [];

  const fields = pages.map((entry) => ({
    title: `${entry.title} ${entry.url}`.toLowerCase(),
    description: entry.description.toLowerCase(),
  }));
  const frequencies = tokens.map(
    (token) =>
      fields.filter((f) => f.title.includes(token) || f.description.includes(token))
        .length
  );
  const weights = frequencies.map(
    (documentFrequency) => Math.log((pages.length + 1) / (documentFrequency + 1)) + 1
  );
  const totalWeight = weights.reduce((a, b) => a + b, 0);

  /**
   * The question's most distinctive word that the corpus actually contains —
   * usually its subject. A page that misses it is about something else, however
   * many common words it shares, so it is not offered as a match.
   */
  let keyToken: string | null = null;
  let keyWeight = -1;
  tokens.forEach((token, i) => {
    if (frequencies[i]! > 0 && weights[i]! > keyWeight) {
      keyWeight = weights[i]!;
      keyToken = token;
    }
  });

  const scored = pages.map((entry, i) => {
    const { title, description } = fields[i]!;
    let matched = 0;
    let titleMatched = 0;
    tokens.forEach((token, t) => {
      const weight = weights[t]!;
      const inTitle = title.includes(token);
      if (inTitle) titleMatched += weight;
      if (inTitle || description.includes(token)) matched += weight;
    });
    const coverage = matched / totalWeight;
    const titleWeight = (titleMatched / totalWeight) * 0.35;
    const related = relatedTokens.length
      ? relatedTokens.filter((t) => title.includes(t) || description.includes(t))
          .length / relatedTokens.length
      : 0;
    const hasKey =
      !keyToken || title.includes(keyToken) || description.includes(keyToken);
    return {
      entry,
      coverage,
      hasKey,
      score: Math.min(1, coverage * 0.7 + titleWeight + related * 0.12),
    };
  });

  return scored
    .filter((s) => s.hasKey && s.coverage >= RELEVANCE_FLOOR)
    .sort((a, b) => b.score - a.score)
    .map(({ entry, score }) => ({ entry, score }));
}

/** True for a ServiceNowDocs table-of-contents page (front matter or shape). */
export function isTableOfContents(content: string): boolean {
  const { meta, body } = splitFrontMatter(content);
  if (/^doc_type:\s*toc\s*$/m.test(meta)) return true;
  const lines = body.split("\n").filter((l) => l.trim().length);
  if (lines.length < 4) return false;
  const links = lines.filter((l) => /^[ \t]*[-*]\s+\[[^\]]+\]\(/.test(l)).length;
  return links / lines.length > 0.6;
}

/** Front matter as raw text plus the prose that follows it. */
export function splitFrontMatter(content: string): { meta: string; body: string } {
  const match = content.match(/^---\r?\n([\s\S]*?)\r?\n---\r?\n?/);
  if (!match) return { meta: "", body: content };
  return { meta: match[1] ?? "", body: content.slice(match[0].length) };
}

/**
 * The prose of a document: no YAML front matter, no navigation breadcrumbs,
 * and no leading title heading (the UI already shows the title).
 */
export function readableBody(content: string): string {
  const { body } = splitFrontMatter(content);
  return body
    .replace(/^#\s+.+$/m, "")
    .replace(/^\s*\[[^\]]+\]\([^)]*\)\s*(?:>|›)\s*.*$/gm, "")
    .replace(/\n{3,}/g, "\n\n")
    .trim();
}

/**
 * A readable excerpt centred on the first paragraph that mentions the question's
 * content words, falling back to the opening paragraph.
 */
export function extractSnippet(body: string, tokens: string[], maxLength = 320): string {
  const paragraphs = body
    .split(/\n{2,}/)
    .map((p) => p.replace(/\s+/g, " ").trim())
    .filter((p) => p.length > 60 && !/^[-*|#>]/.test(p));
  const pick = unescapeMarkdown(
    paragraphs.find((p) => {
      const lower = p.toLowerCase();
      return tokens.some((t) => lower.includes(t));
    }) ?? paragraphs[0] ?? ""
  );
  if (pick.length <= maxLength) return pick;
  const cut = pick.lastIndexOf(" ", maxLength);
  return `${pick.slice(0, cut > maxLength * 0.6 ? cut : maxLength).trimEnd()}…`;
}
