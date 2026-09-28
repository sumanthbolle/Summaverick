import type { ServiceNowEvidence } from "../schemas/evidence.js";

export function filterByReleaseFamily(
  evidence: ServiceNowEvidence[],
  releaseFamily?: string
): ServiceNowEvidence[] {
  if (!releaseFamily) return evidence;
  const matched = evidence.filter(
    (e) =>
      !e.releaseFamily ||
      e.releaseFamily.toLowerCase() === releaseFamily.toLowerCase() ||
      e.sourceType.startsWith("sdk_") ||
      e.sourceType === "local_repository" ||
      e.sourceType === "live_instance"
  );
  return matched.length ? matched : evidence;
}
