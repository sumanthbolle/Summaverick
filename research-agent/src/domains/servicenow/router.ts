import {
  classifyServiceNowIntent,
  isServiceNowDomainQuery,
} from "./retrieval/query-classifier.js";
import type { ServiceNowIntentResult } from "./schemas/domain-intent.js";
import type { PlannedSource, ServiceNowResearchPlan } from "./types.js";
import type { ServiceNowDomainConfig } from "./config.js";
import { nowIso, type TraceSink } from "../../core/types.js";

export interface ServiceNowRouteDecision {
  isServiceNow: boolean;
  intent?: ServiceNowIntentResult;
  plan?: ServiceNowResearchPlan;
}

export function routeServiceNowQuery(
  query: string,
  config: ServiceNowDomainConfig,
  options?: {
    sdkVersion?: string;
    releaseFamily?: string;
    trace?: TraceSink;
  }
): ServiceNowRouteDecision {
  if (!config.enabled || !isServiceNowDomainQuery(query)) {
    return { isServiceNow: false };
  }

  const intent = classifyServiceNowIntent(query);
  options?.trace?.emit({
    name: "servicenow.intent.classified",
    timestamp: nowIso(),
    attributes: {
      intent: intent.intent,
      confidence: intent.confidence,
      requiresLiveInstance: intent.requiresLiveInstance,
    },
  });

  const sources: PlannedSource[] = [];
  if (intent.requiresSdkDocs && config.sdk.enabled) sources.push("sdk_explain");
  if (intent.requiresProductDocs && config.documentation.enabled) {
    sources.push("product_docs");
  }
  if (intent.requiresRepositoryContext) sources.push("local_repository");
  if (intent.requiresLiveInstance && config.instance.enabled) {
    sources.push("live_instance");
  }

  const plan: ServiceNowResearchPlan = {
    intent: intent.intent,
    questionsToResolve: [query.trim()],
    sources,
    requiresLiveInstance: intent.requiresLiveInstance && config.instance.enabled,
    releaseFamily:
      intent.requestedReleaseFamily ||
      options?.releaseFamily ||
      config.documentation.releaseFamily,
    sdkVersion: options?.sdkVersion,
    maximumToolCalls: 8,
  };

  options?.trace?.emit({
    name: "servicenow.research.plan.created",
    timestamp: nowIso(),
    attributes: {
      intent: plan.intent,
      sources: plan.sources.join(","),
      releaseFamily: plan.releaseFamily,
    },
  });

  return { isServiceNow: true, intent, plan };
}
