import type { DefaultSdkExplainProvider } from "../providers/sdk-explain-provider.js";
import { suggestSdkSearchTerms } from "../retrieval/query-expander.js";
import type { ServiceNowEvidence } from "../schemas/evidence.js";

export async function explainSdkTopic(options: {
  provider: DefaultSdkExplainProvider;
  projectRoot: string;
  query: string;
  maxTopics?: number;
}): Promise<ServiceNowEvidence[]> {
  const terms = suggestSdkSearchTerms(options.query);
  const evidence: ServiceNowEvidence[] = [];
  const maxTopics = options.maxTopics ?? 3;

  for (const term of terms.slice(0, 3)) {
    const matches = await options.provider.searchTopics(options.projectRoot, term);
    for (const match of matches.slice(0, maxTopics)) {
      const preview = await options.provider.peekTopic(
        options.projectRoot,
        match.topic
      );
      if (!preview.preview.trim()) continue;
      const full = await options.provider.readTopic(
        options.projectRoot,
        match.topic
      );
      full.relevanceScore = Math.max(full.relevanceScore, match.score);
      evidence.push(full);
      if (evidence.length >= maxTopics) return evidence;
    }
  }
  return evidence;
}
