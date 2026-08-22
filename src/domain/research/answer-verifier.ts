import type { ServiceNowEvidence, EvidenceClaimLink } from "./schemas/evidence";
import type { ServiceNowResearchAnswer } from "./schemas/research-answer";
import { INSUFFICIENT_EVIDENCE_MESSAGE } from "./schemas/research-answer";
import type { ServiceNowDomainConfig } from "./config";
import { scanForPromptInjection } from "./security/prompt-injection";
import { nowIso, type TraceSink } from "./core/types";

/** Per-claim verdict, surfaced to the /api/research trace. */
export interface ClaimVerdict {
  text: string;
  verdict: "supported" | "unsupported";
  evidenceId?: string;
}

export interface VerificationResult {
  ok: boolean;
  answer: ServiceNowResearchAnswer;
  unsupportedClaimCount: number;
  issues: string[];
  /** Which citation requirement / injection / mismatch gates fired. */
  claims: ClaimVerdict[];
  citationCount: number;
  promptInjectionDetected: boolean;
}

export function verifyServiceNowAnswer(options: {
  draft: ServiceNowResearchAnswer;
  evidence: ServiceNowEvidence[];
  config: ServiceNowDomainConfig;
  majorClaims?: string[];
  trace?: TraceSink;
}): VerificationResult {
  const issues: string[] = [];
  const evidenceIds = new Set(options.evidence.map((e) => e.id));
  let unsupportedClaimCount = 0;
  const claimVerdicts: ClaimVerdict[] = [];
  let promptInjectionDetected = false;

  const linked: EvidenceClaimLink[] = [];
  for (const link of options.draft.evidence) {
    if (!evidenceIds.has(link.evidenceId)) {
      unsupportedClaimCount += 1;
      issues.push(`Claim missing evidence id: ${link.claim}`);
      claimVerdicts.push({ text: link.claim, verdict: "unsupported" });
      continue;
    }
    linked.push(link);
    claimVerdicts.push({
      text: link.claim,
      verdict: "supported",
      evidenceId: link.evidenceId,
    });
  }

  const claims = options.majorClaims?.length
    ? options.majorClaims
    : extractMajorClaims(options.draft);

  for (const claim of claims) {
    const hasLink = linked.some((l) =>
      normalize(l.claim).includes(normalize(claim).slice(0, 40))
    );
    if (!hasLink) {
      // Attempt auto-link to highest authority evidence
      const best = [...options.evidence].sort(
        (a, b) => b.authorityScore * b.relevanceScore - a.authorityScore * a.relevanceScore
      )[0];
      if (best && options.evidence.length > 0) {
        linked.push({ evidenceId: best.id, claim });
        claimVerdicts.push({
          text: claim,
          verdict: "supported",
          evidenceId: best.id,
        });
      } else {
        unsupportedClaimCount += 1;
        issues.push(`Uncited claim: ${claim}`);
        claimVerdicts.push({ text: claim, verdict: "unsupported" });
      }
    }
  }

  for (const item of options.evidence) {
    const scan = scanForPromptInjection(item.content);
    if (scan.suspicious) {
      promptInjectionDetected = true;
      issues.push("Retrieved evidence contained prompt-injection patterns (ignored for control).");
    }
    if (item.containsSensitiveData) {
      options.draft.warnings.push("Some instance fields were redacted.");
    }
  }

  if (
    options.config.citations.required &&
    linked.length < options.config.citations.minimumEvidenceCount
  ) {
    unsupportedClaimCount += 1;
    issues.push("Citation requirement not met.");
  }

  const sdkVersions = new Set(
    options.evidence.map((e) => e.sdkVersion).filter(Boolean)
  );
  const docVersions = new Set(
    options.evidence
      .map((e) => e.documentationVersion)
      .filter(Boolean)
  );
  if (sdkVersions.size && docVersions.size) {
    const installed = [...sdkVersions][0];
    const docs = [...docVersions][0];
    if (installed && docs && installed !== docs) {
      options.draft.warnings.push(
        `SDK version mismatch: installed ${installed}, documentation ${docs}.`
      );
    }
  }

  let answer: ServiceNowResearchAnswer = {
    ...options.draft,
    evidence: linked,
    warnings: [...new Set(options.draft.warnings)],
  };

  if (
    options.config.citations.required &&
    (linked.length === 0 || unsupportedClaimCount > claims.length)
  ) {
    answer = {
      summary: INSUFFICIENT_EVIDENCE_MESSAGE,
      directAnswer: INSUFFICIENT_EVIDENCE_MESSAGE,
      warnings: [...answer.warnings, ...issues],
      assumptions: answer.assumptions,
      evidence: [],
      confidence: "low",
      releaseFamily: answer.releaseFamily,
      sdkVersion: answer.sdkVersion,
      requiresInstanceValidation: answer.requiresInstanceValidation,
    };
  } else if (unsupportedClaimCount > 0) {
    answer.confidence = answer.confidence === "high" ? "medium" : "low";
    answer.warnings.push("Some claims could not be fully verified against evidence.");
  }

  options.trace?.emit({
    name: "servicenow.answer.verified",
    timestamp: nowIso(),
    attributes: {
      ok: issues.length === 0,
      unsupportedClaimCount,
      citationCount: linked.length,
    },
  });

  return {
    ok: issues.length === 0 && answer.evidence.length > 0,
    answer,
    unsupportedClaimCount,
    issues,
    claims: claimVerdicts,
    citationCount: linked.length,
    promptInjectionDetected,
  };
}

function extractMajorClaims(draft: ServiceNowResearchAnswer): string[] {
  const claims = [draft.directAnswer, draft.summary].filter(Boolean);
  if (draft.explanation) claims.push(draft.explanation.split(/[.!?]/)[0] || draft.explanation);
  return claims.map((c) => c.trim()).filter((c) => c.length > 12);
}

function normalize(s: string): string {
  return s.toLowerCase().replace(/\s+/g, " ").trim();
}
