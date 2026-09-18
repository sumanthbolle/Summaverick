/**
 * Answer checks.
 *
 * These used to collapse into one "verified" badge, which was misleading: a
 * citation count says nothing about whether the sources are relevant, whether
 * a written answer follows from them, or whether the question was answered.
 * Four separate signals are reported instead, and each one only claims what it
 * actually tested:
 *
 *   sourcesFound   did retrieval release any evidence at all
 *   relevance      does the retrieved text match the question's terms
 *   groundedness   is every sentence of a written answer traceable to a source
 *                  (term overlap, not entailment) — not applicable when no
 *                  model wrote an answer
 *   completeness   never assessed automatically; stated as such
 */
import type { ServiceNowEvidence, EvidenceClaimLink } from "./schemas/evidence";
import type { ServiceNowResearchAnswer } from "./schemas/research-answer";
import { INSUFFICIENT_EVIDENCE_MESSAGE } from "./schemas/research-answer";
import type { ServiceNowDomainConfig } from "./config";
import { scanForPromptInjection } from "./security/prompt-injection";
import { nowIso, type TraceSink } from "./core/types";

/** Sentence-level verdict for a written answer, shown in the detailed trace. */
export interface ClaimVerdict {
  text: string;
  verdict: "supported" | "unsupported";
  evidenceId?: string;
}

export type RelevanceVerdict = "strong" | "weak" | "none";

export interface AnswerChecks {
  sourcesFound: { count: number; ok: boolean };
  relevance: {
    verdict: RelevanceVerdict;
    topScore: number;
    meanScore: number;
  };
  groundedness: {
    status: "checked" | "not_applicable";
    sentences: number;
    grounded: number;
    unsupported: string[];
    method: string;
  };
  completeness: { status: "not_assessed"; note: string };
}

export interface VerificationResult {
  /** Retrieval released usable, relevant evidence. Not a claim about answer quality. */
  ok: boolean;
  answer: ServiceNowResearchAnswer;
  issues: string[];
  citationCount: number;
  promptInjectionDetected: boolean;
  checks: AnswerChecks;
}

/** A source is relevant enough to release at or above this match score. */
const RELEVANCE_STRONG = 0.5;
const RELEVANCE_WEAK = 0.3;

const GROUNDEDNESS_METHOD =
  "term overlap between each sentence and the retrieved passages; it does not test entailment";

const COMPLETENESS_NOTE =
  "not assessed automatically — no check decides whether the question was fully answered";

export function verifyServiceNowAnswer(options: {
  draft: ServiceNowResearchAnswer;
  evidence: ServiceNowEvidence[];
  config: ServiceNowDomainConfig;
  trace?: TraceSink;
}): VerificationResult {
  const issues: string[] = [];
  const evidenceIds = new Set(options.evidence.map((e) => e.id));

  // Claim links only survive when they point at evidence that was released.
  const linked: EvidenceClaimLink[] = [];
  for (const link of options.draft.evidence) {
    if (evidenceIds.has(link.evidenceId)) linked.push(link);
  }

  let promptInjectionDetected = false;
  for (const item of options.evidence) {
    if (scanForPromptInjection(item.content).suspicious) {
      promptInjectionDetected = true;
      issues.push(
        "Retrieved evidence contained prompt-injection patterns (kept as data, never instructions)."
      );
    }
    if (item.containsSensitiveData) {
      options.draft.warnings.push("Some instance fields were redacted.");
    }
  }

  const relevance = scoreRelevance(options.evidence);
  const minimum = options.config.citations.required
    ? options.config.citations.minimumEvidenceCount
    : 0;

  if (options.evidence.length < minimum) {
    issues.push(
      `Retrieval released ${options.evidence.length} source(s); the citation gate requires ${minimum}.`
    );
  }
  if (options.evidence.length > 0 && relevance.verdict === "none") {
    issues.push(
      "The retrieved sources did not match the question closely enough to answer from."
    );
  }

  const checks: AnswerChecks = {
    sourcesFound: {
      count: options.evidence.length,
      ok: options.evidence.length >= Math.max(1, minimum),
    },
    relevance,
    groundedness: {
      status: "not_applicable",
      sentences: 0,
      grounded: 0,
      unsupported: [],
      method: GROUNDEDNESS_METHOD,
    },
    completeness: { status: "not_assessed", note: COMPLETENESS_NOTE },
  };

  const usable = checks.sourcesFound.ok && relevance.verdict !== "none";

  let answer: ServiceNowResearchAnswer = usable
    ? {
        ...options.draft,
        evidence: linked,
        warnings: [...new Set(options.draft.warnings)],
      }
    : {
        summary: INSUFFICIENT_EVIDENCE_MESSAGE,
        directAnswer: INSUFFICIENT_EVIDENCE_MESSAGE,
        warnings: [...new Set([...options.draft.warnings, ...issues])],
        assumptions: options.draft.assumptions,
        evidence: [],
        confidence: "low",
        releaseFamily: options.draft.releaseFamily,
        sdkVersion: options.draft.sdkVersion,
        requiresInstanceValidation: options.draft.requiresInstanceValidation,
      };

  if (usable && relevance.verdict === "weak") {
    answer = { ...answer, confidence: "low" };
    answer.warnings = [
      ...new Set([
        ...answer.warnings,
        "The closest sources only partly match the question.",
      ]),
    ];
  }

  options.trace?.emit({
    name: "servicenow.answer.checked",
    timestamp: nowIso(),
    attributes: {
      sources: options.evidence.length,
      relevance: relevance.verdict,
      topScore: relevance.topScore,
      usable,
    },
  });

  return {
    ok: usable,
    answer,
    issues,
    citationCount: usable ? options.evidence.length : 0,
    promptInjectionDetected,
    checks,
  };
}

function scoreRelevance(evidence: ServiceNowEvidence[]): AnswerChecks["relevance"] {
  if (!evidence.length) {
    return { verdict: "none", topScore: 0, meanScore: 0 };
  }
  const scores = evidence.map((e) => e.retrievalRelevance ?? e.relevanceScore);
  const topScore = round(Math.max(...scores));
  const meanScore = round(scores.reduce((a, b) => a + b, 0) / scores.length);
  const verdict: RelevanceVerdict =
    topScore >= RELEVANCE_STRONG ? "strong" : topScore >= RELEVANCE_WEAK ? "weak" : "none";
  return { verdict, topScore, meanScore };
}

export interface GroundednessResult {
  groundedness: AnswerChecks["groundedness"];
  claims: ClaimVerdict[];
}

/**
 * Check a written answer sentence by sentence against the released passages.
 *
 * This is term overlap, deliberately described as such everywhere it surfaces.
 * It catches an answer that wandered away from its sources; it cannot confirm
 * that a grounded sentence is correct.
 */
export function assessGroundedness(
  text: string,
  evidence: ServiceNowEvidence[]
): GroundednessResult {
  const sentences = splitSentences(text);
  const claims: ClaimVerdict[] = [];
  const unsupported: string[] = [];

  const haystacks = evidence.map((e) => ({
    id: e.id,
    text: `${e.title} ${e.snippet ?? e.content}`.toLowerCase(),
  }));

  for (const sentence of sentences) {
    const words = contentWords(sentence);
    if (words.length < 3) continue;

    let bestId: string | undefined;
    let bestShare = 0;
    for (const hay of haystacks) {
      const share = words.filter((w) => hay.text.includes(w)).length / words.length;
      if (share > bestShare) {
        bestShare = share;
        bestId = hay.id;
      }
    }

    // Half the content words of a sentence appearing in one passage is the
    // threshold for calling that sentence traceable to it.
    if (bestShare >= 0.5 && bestId) {
      claims.push({ text: sentence, verdict: "supported", evidenceId: bestId });
    } else {
      claims.push({ text: sentence, verdict: "unsupported" });
      unsupported.push(sentence);
    }
  }

  return {
    groundedness: {
      status: "checked",
      sentences: claims.length,
      grounded: claims.length - unsupported.length,
      unsupported,
      method: GROUNDEDNESS_METHOD,
    },
    claims,
  };
}

function splitSentences(text: string): string[] {
  return text
    .replace(/\s+/g, " ")
    .split(/(?<=[.!?])\s+(?=[A-Z0-9])/)
    .map((s) => s.trim())
    .filter((s) => s.length > 24);
}

const SENTENCE_STOPWORDS = new Set([
  "a", "an", "and", "are", "as", "at", "be", "but", "by", "can", "for", "from",
  "has", "have", "how", "in", "into", "is", "it", "its", "not", "of", "on",
  "or", "that", "the", "their", "then", "these", "they", "this", "to", "use",
  "used", "using", "was", "were", "when", "which", "will", "with", "you",
  "your",
]);

function contentWords(sentence: string): string[] {
  return [
    ...new Set(
      sentence
        .toLowerCase()
        .split(/[^a-z0-9_]+/)
        .filter((w) => w.length > 2 && !SENTENCE_STOPWORDS.has(w))
    ),
  ];
}

function round(n: number): number {
  return Math.round(n * 1000) / 1000;
}
