import type { ServiceNowEvidence, ServiceNowSourceType } from "../schemas/evidence.js";
import type { ServiceNowIntent } from "../schemas/domain-intent.js";
import { deduplicateEvidence } from "../schemas/evidence.js";

const AUTHORITY: Partial<
  Record<ServiceNowIntent | "default", Partial<Record<ServiceNowSourceType, number>>>
> = {
  fluent_sdk: {
    sdk_explain: 1,
    sdk_api_reference: 0.95,
    sdk_example: 0.9,
    local_repository: 0.85,
    product_documentation: 0.7,
  },
  product_concept: {
    product_documentation: 1,
    live_instance: 0.75,
    local_repository: 0.65,
    sdk_explain: 0.55,
  },
  instance_schema: {
    live_instance: 1,
    local_repository: 0.8,
    product_documentation: 0.7,
    sdk_explain: 0.5,
  },
  instance_record_lookup: {
    live_instance: 1,
    local_repository: 0.75,
    product_documentation: 0.65,
    sdk_explain: 0.45,
  },
  default: {
    product_documentation: 0.9,
    sdk_explain: 0.85,
    local_repository: 0.8,
    live_instance: 0.75,
    sdk_example: 0.7,
    sdk_api_reference: 0.88,
  },
};

export function rankEvidence(
  evidence: ServiceNowEvidence[],
  intent: ServiceNowIntent,
  options?: { releaseFamily?: string; sdkVersion?: string }
): ServiceNowEvidence[] {
  const weights = AUTHORITY[intent] ?? AUTHORITY.default ?? {};
  const ranked = evidence.map((item) => {
    const authority =
      (weights[item.sourceType] ?? 0.5) * item.authorityScore;
    const releaseBoost =
      options?.releaseFamily && item.releaseFamily === options.releaseFamily
        ? 0.08
        : 0;
    const sdkBoost =
      options?.sdkVersion && item.sdkVersion === options.sdkVersion ? 0.08 : 0;
    const score =
      authority * 0.45 +
      item.relevanceScore * 0.35 +
      item.freshnessScore * 0.1 +
      releaseBoost +
      sdkBoost;
    return { ...item, relevanceScore: clamp(score) };
  });

  return deduplicateEvidence(ranked).sort(
    (a, b) => b.relevanceScore - a.relevanceScore
  );
}

function clamp(n: number): number {
  return Math.max(0, Math.min(1, n));
}
