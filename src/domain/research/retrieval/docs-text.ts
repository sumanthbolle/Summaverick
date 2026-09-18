/**
 * Text handling for the product-documentation layer.
 *
 * ServiceNowDocs publishes one `llms.txt` that links a single `index.md` per
 * publication, and each of those index pages is a nested link list pointing at
 * the real topic pages. A single hop therefore can never reach a page that
 * answers a question — it can only reach a table of contents. Everything here
 * supports the second hop: pick query terms worth matching on, score topic
 * entries from a publication index, then pull the passage of the topic page
 * that actually addresses the question.
 */

/** Words that carry no retrieval signal in any question. */
const STOPWORDS = new Set([
  "a", "about", "after", "all", "also", "an", "and", "any", "are", "as", "at",
  "be", "because", "been", "before", "between", "both", "but", "by", "can",
  "could", "did", "do", "does", "doing", "for", "from", "get", "gets", "had",
  "has", "have", "how", "i", "if", "in", "into", "is", "it", "its", "just",
  "me", "more", "most", "my", "no", "not", "of", "on", "one", "only", "or",
  "other", "our", "out", "over", "should", "so", "some", "such", "than",
  "that", "the", "their", "them", "then", "there", "these", "they", "this",
  "those", "to", "up", "was", "we", "were", "what", "when", "where", "which",
  "while", "who", "why", "will", "with", "would", "you", "your",
]);

/**
 * Words that appear in nearly every ServiceNow document, so matching on them
 * ranks table-of-contents pages above the topic that answers the question.
 * Dropping them is what stops "how do CI relationships work" from retrieving
 * every publication with "workflow" in its title.
 */
const GENERIC_DOC_WORDS = new Set([
  "servicenow", "now", "platform", "product", "documentation", "docs", "doc",
  "guide", "guides", "reference", "overview", "introduction", "explain",
  "explained", "understand", "work", "works", "working", "use", "used",
  "using", "usage", "need", "needs", "want", "know", "help", "example",
  "examples", "instance", "system", "application", "applications", "app",
  "apps", "feature", "features", "information", "data", "release", "version",
]);

/** Acronyms short enough to be dropped by a length filter but worth matching. */
const SHORT_TERMS = new Set([
  "ci", "cd", "kb", "ui", "ux", "va", "ml", "ai", "hr", "it", "sn",
]);

export interface DocsQueryTerms {
  /** Content-bearing single words. Coverage of these drives the score. */
  terms: string[];
  /** Multi-word phrases worth an exact-match bonus. */
  phrases: string[];
}

/**
 * Build the terms the documentation layer scores on, from the visitor's
 * question and nothing else.
 *
 * The domain query expansions are deliberately not folded in here. They are
 * keyed on single words, so "…query the incident table" pulled in "incident
 * lifecycle" and "assignment rules", and those extra terms then ranked the
 * assignment-rules pages above the GlideRecord API reference. Expansions still
 * serve the SDK layer; the documentation layer scores the question asked.
 */
export function buildDocsQueryTerms(query: string): DocsQueryTerms {
  const terms = tokenize(query);
  return { terms: [...new Set(terms)], phrases: adjacentPhrases(terms) };
}

/**
 * Pairs of content words that were adjacent in the question, e.g. "ci
 * relationships" or "business rule". An exact hit on one of these is stronger
 * evidence than the two words appearing separately.
 */
function adjacentPhrases(terms: string[]): string[] {
  const phrases: string[] = [];
  for (let i = 0; i < terms.length - 1; i += 1) {
    phrases.push(`${terms[i]} ${terms[i + 1]}`);
  }
  return phrases;
}

function tokenize(text: string): string[] {
  return text
    .toLowerCase()
    .split(/[^a-z0-9_]+/)
    .filter((token) => {
      if (!token) return false;
      if (STOPWORDS.has(token) || GENERIC_DOC_WORDS.has(token)) return false;
      return token.length > 2 || SHORT_TERMS.has(token);
    });
}

/** Fraction of `terms` present in `text` as whole words. 0 when no terms. */
export function termCoverage(text: string, terms: string[]): number {
  if (!terms.length) return 0;
  const hay = text.toLowerCase();
  let hits = 0;
  for (const term of terms) {
    if (containsWord(hay, term)) hits += 1;
  }
  return hits / terms.length;
}

/**
 * Coverage weighted by how distinctive each term is inside the corpus being
 * searched. Without this, a question like "configure the flux capacitor table"
 * scores well against every page containing "configure" and "table", because
 * plain coverage treats the corpus's most common words as evidence.
 */
export function weightedTermCoverage(
  text: string,
  terms: string[],
  weights: Map<string, number>
): number {
  if (!terms.length) return 0;
  const hay = text.toLowerCase();
  let matched = 0;
  let total = 0;
  for (const term of terms) {
    const weight = weights.get(term) ?? 1;
    total += weight;
    if (containsWord(hay, term)) matched += weight;
  }
  return total > 0 ? matched / total : 0;
}

/**
 * Inverse-document-frequency weight per term, over the corpus actually being
 * searched. A term in most documents carries little signal; a rare one carries
 * a lot.
 */
export function computeTermWeights(
  documents: string[],
  terms: string[]
): Map<string, number> {
  const weights = new Map<string, number>();
  if (!documents.length) {
    for (const term of terms) weights.set(term, 1);
    return weights;
  }

  const total = documents.length;
  const haystacks = documents.map((d) => d.toLowerCase());
  let max = 0;
  for (const term of terms) {
    let df = 0;
    for (const hay of haystacks) {
      if (containsWord(hay, term)) df += 1;
    }
    const idf = Math.log((total + 1) / (df + 1));
    weights.set(term, idf);
    if (idf > max) max = idf;
  }

  if (max > 0) {
    for (const [term, idf] of weights) weights.set(term, idf / max);
  } else {
    for (const term of terms) weights.set(term, 1);
  }
  return weights;
}

/** Undo the punctuation escaping ServiceNowDocs applies for its renderer. */
export function unescapeMarkdown(text: string): string {
  return text.replace(/\\([()[\]{}<>*_`#+\-.!|])/g, "$1");
}

/** How many of `phrases` appear verbatim in `text`. */
export function phraseHits(text: string, phrases: string[]): number {
  if (!phrases.length) return 0;
  const hay = text.toLowerCase();
  let hits = 0;
  for (const phrase of phrases) {
    if (hay.includes(phrase)) hits += 1;
  }
  return hits;
}

/**
 * Whole-word containment, tolerant of a trailing plural. Substring matching is
 * what let "work" score a hit on "build-workflows".
 */
function containsWord(haystack: string, term: string): boolean {
  for (const form of wordForms(term)) {
    if (matchesWord(haystack, form)) return true;
  }
  return false;
}

function wordForms(term: string): string[] {
  const forms = new Set([term]);
  const singular = term.endsWith("ies")
    ? `${term.slice(0, -3)}y`
    : term.replace(/(es|s)$/, "");
  if (singular.length > 2) {
    forms.add(singular);
    forms.add(`${singular}s`);
    forms.add(`${singular}es`);
  }
  return [...forms];
}

function matchesWord(haystack: string, term: string): boolean {
  let from = 0;
  for (;;) {
    const at = haystack.indexOf(term, from);
    if (at < 0) return false;
    const before = at === 0 ? "" : haystack[at - 1]!;
    const after = haystack[at + term.length] ?? "";
    if (!/[a-z0-9]/.test(before) && !/[a-z0-9]/.test(after)) return true;
    from = at + 1;
  }
}

export interface FrontMatter {
  meta: Record<string, string>;
  body: string;
}

/**
 * Split the YAML front matter off a ServiceNowDocs markdown file. The raw
 * front matter is metadata for the fetcher, never answer text.
 */
export function parseFrontMatter(markdown: string): FrontMatter {
  const match = /^---\r?\n([\s\S]*?)\r?\n---\r?\n?/.exec(markdown);
  if (!match) return { meta: {}, body: markdown.trim() };

  const meta: Record<string, string> = {};
  for (const line of match[1]!.split(/\r?\n/)) {
    const pair = /^([A-Za-z0-9_-]+):\s*(.*)$/.exec(line);
    if (!pair) continue;
    meta[pair[1]!.toLowerCase()] = pair[2]!.trim().replace(/^["']|["']$/g, "");
  }
  return { meta, body: markdown.slice(match[0].length).trim() };
}

/**
 * True when a document is a navigation page rather than an explanation. These
 * are useful for finding the next hop and useless as an answer.
 */
export function isTableOfContents(meta: Record<string, string>, body: string): boolean {
  if ((meta.doc_type || "").toLowerCase() === "toc") return true;
  const lines = body.split(/\r?\n/).filter((line) => line.trim());
  if (lines.length < 4) return false;
  const linkLines = lines.filter((line) => /^\s*[-*]\s*\[[^\]]+\]\(/.test(line));
  return linkLines.length / lines.length > 0.6;
}

export interface DocsTopicEntry {
  title: string;
  url: string;
  description: string;
  /** Nesting depth in the publication index; 0 is a top-level entry. */
  depth: number;
}

/**
 * Parse the topic entries out of a publication `index.md`. Lines look like
 * `  - [Title](url) -- description`, nested by indentation.
 */
export function parseTopicEntries(markdown: string): DocsTopicEntry[] {
  const entries: DocsTopicEntry[] = [];
  const seen = new Set<string>();
  const lineRe = /^(\s*)[-*]\s*\[([^\]]+)\]\((https?:\/\/[^)\s]+)\)\s*(?:--\s*(.*))?$/;

  for (const line of markdown.split(/\r?\n/)) {
    const match = lineRe.exec(line);
    if (!match) continue;
    const url = match[3]!.trim();
    const title = unescapeMarkdown(match[2]!.trim());
    const key = `${url}|${title}`;
    if (seen.has(key)) continue;
    seen.add(key);
    entries.push({
      title,
      url,
      description: unescapeMarkdown((match[4] ?? "").trim()),
      depth: Math.floor(match[1]!.replace(/\t/g, "  ").length / 2),
    });
  }
  return entries;
}

export interface DocsPassage {
  /** Readable prose, front matter and link plumbing removed. */
  text: string;
  /** The heading the passage sits under, when the page has headings. */
  heading: string | null;
  /** Term coverage of the extracted passage, 0–1. */
  coverage: number;
}

/**
 * Pull the section of a topic page that best matches the question, so the
 * answer surface shows the relevant passage instead of the first N bytes.
 */
export function extractPassage(
  body: string,
  terms: string[],
  phrases: string[] = [],
  maxChars = 1200
): DocsPassage {
  const sections = splitSections(body);
  if (!sections.length) {
    return { text: "", heading: null, coverage: 0 };
  }

  let best = sections[0]!;
  let bestScore = -1;
  for (const section of sections) {
    if (section.text.length < 40) continue;
    const score =
      termCoverage(`${section.heading ?? ""} ${section.text}`, terms) +
      0.2 * phraseHits(`${section.heading ?? ""} ${section.text}`, phrases);
    if (score > bestScore) {
      bestScore = score;
      best = section;
    }
  }

  const text = clip(best.text, maxChars);
  return {
    text,
    heading: best.heading,
    coverage: termCoverage(`${best.heading ?? ""} ${text}`, terms),
  };
}

interface DocsSection {
  heading: string | null;
  text: string;
}

function splitSections(body: string): DocsSection[] {
  const sections: DocsSection[] = [];
  let heading: string | null = null;
  let buffer: string[] = [];

  const flush = () => {
    const text = cleanProse(buffer.join("\n"));
    if (text) sections.push({ heading, text });
    buffer = [];
  };

  for (const line of body.split(/\r?\n/)) {
    const headingMatch = /^(#{1,6})\s+(.*)$/.exec(line);
    if (headingMatch) {
      flush();
      heading = unescapeMarkdown(
        headingMatch[2]!.replace(/\{#[^}]*\}/g, "").trim()
      );
      continue;
    }
    buffer.push(line);
  }
  flush();
  return sections;
}

/**
 * Turn documentation markdown into something readable in an answer panel:
 * links become their text, list entries keep their description, and anchors,
 * comments and image embeds go away.
 */
function cleanProse(markdown: string): string {
  return markdown
    .replace(/<!--[\s\S]*?-->/g, "")
    .replace(/^\s*[-*]\s*\[([^\]]+)\]\([^)\s]+\)\s*--\s*(.*)$/gm, "- $1 — $2")
    .replace(/^\s*[-*]\s*\[([^\]]+)\]\([^)\s]+\)\s*$/gm, "- $1")
    .replace(/!\[[^\]]*\]\([^)\s]*\)/g, "")
    .replace(/\[([^\]]+)\]\([^)\s]+\)/g, "$1")
    .replace(/\{#[^}]*\}/g, "")
    // ServiceNowDocs escapes punctuation for its own renderer; the escapes
    // read as noise once the text is plain prose.
    .replace(/\\([()[\]{}<>*_`#+\-.!|])/g, "$1")
    .replace(/[ \t]+$/gm, "")
    .replace(/\n{3,}/g, "\n\n")
    .trim();
}

/** Trim to a length without cutting a word or sentence in half. */
export function clip(text: string, maxChars: number): string {
  const trimmed = text.trim();
  if (trimmed.length <= maxChars) return trimmed;
  const window = trimmed.slice(0, maxChars);
  const sentenceEnd = Math.max(
    window.lastIndexOf(". "),
    window.lastIndexOf(".\n"),
    window.lastIndexOf("! "),
    window.lastIndexOf("? ")
  );
  if (sentenceEnd > maxChars * 0.5) return window.slice(0, sentenceEnd + 1).trim();
  const wordEnd = window.lastIndexOf(" ");
  return `${window.slice(0, wordEnd > 0 ? wordEnd : maxChars).trim()}…`;
}

/** A one- or two-sentence plain-text snippet for a source card. */
export function plainSnippet(text: string, maxChars = 320): string {
  return clip(
    text
      .replace(/^#+\s*/gm, "")
      .replace(/[*_`>]/g, "")
      .replace(/\s*\n\s*/g, " ")
      .trim(),
    maxChars
  );
}
