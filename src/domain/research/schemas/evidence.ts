export type ServiceNowSourceType =
  | "sdk_explain"
  | "sdk_api_reference"
  | "sdk_example"
  | "product_documentation"
  | "local_repository"
  | "live_instance";

export interface ServiceNowEvidence {
  id: string;
  sourceType: ServiceNowSourceType;
  title: string;
  /** Evidence text, wrapped in the untrusted markers for model prompts. */
  content: string;
  /** Plain-text excerpt safe to render in a source card. */
  snippet?: string;
  /** Publication or collection the evidence came from, for display. */
  publicationTitle?: string;
  sourceReference: string;
  canonicalUrl?: string;
  sdkVersion?: string;
  /** Version of SDK docs used when sourceType is sdk_* */
  documentationVersion?: string;
  releaseFamily?: string;
  module?: string;
  table?: string;
  recordSysId?: string;
  projectRoot?: string;
  filePath?: string;
  lineStart?: number;
  lineEnd?: number;
  retrievedAt: string;
  authorityScore: number;
  /** Blended score after ranking; the sort key. */
  relevanceScore: number;
  /**
   * How well the retrieved text matched the question, before authority and
   * freshness were blended in. Ranking must not overwrite this, because it is
   * the only signal that separates "we found sources" from "the sources are
   * about the question".
   */
  retrievalRelevance?: number;
  freshnessScore: number;
  taskScoped: boolean;
  containsSensitiveData: boolean;
}

export interface EvidenceClaimLink {
  evidenceId: string;
  claim: string;
}

export function wrapUntrustedEvidence(content: string): string {
  return [
    "BEGIN_UNTRUSTED_SERVICENOW_EVIDENCE",
    content,
    "END_UNTRUSTED_SERVICENOW_EVIDENCE",
  ].join("\n");
}

export function deduplicateEvidence(
  items: ServiceNowEvidence[]
): ServiceNowEvidence[] {
  const seen = new Map<string, ServiceNowEvidence>();
  for (const item of items) {
    const key = [
      item.sourceType,
      item.sourceReference,
      item.content.slice(0, 240),
    ].join("|");
    const existing = seen.get(key);
    if (!existing || item.relevanceScore > existing.relevanceScore) {
      seen.set(key, item);
    }
  }
  return [...seen.values()];
}
