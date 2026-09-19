/**
 * End-to-end ServiceNow research workflow. Ported from research-agent with two
 * changes: (1) `process.cwd()` fallback replaced by "/" (no cwd in a Worker);
 * (2) the returned result is ENRICHED with the classification, plan, per-source
 * candidate counts, query expansions and the full verification result, so the
 * /api/research route can render a faithful, visible trace. The retrieval,
 * ranking, evidence-gate and verification LOGIC are unchanged.
 */
import {
  createId,
  nowIso,
  type AuthorizedResearchContext,
  type TraceSink,
  InMemoryTraceSink,
} from "./core/types";
import type { ServiceNowDomainConfig } from "./config";
import { defaultServiceNowDomainConfig } from "./config";
import { ServiceNowPolicy } from "./policy";
import { routeServiceNowQuery } from "./router";
import { DefaultSdkExplainProvider } from "./providers/sdk-explain-provider";
import { DefaultServiceNowInstanceQueryProvider } from "./providers/sdk-query-provider";
import {
  HttpServiceNowDocsProvider,
  extractSnippet,
  readableBody,
} from "./providers/servicenow-docs-provider";
import { LocalFluentRepositoryProvider } from "./providers/repository-provider";
import { explainSdkTopic } from "./tools/explain-sdk-topic";
import { searchProductDocs } from "./tools/search-product-docs";
import { inspectFluentProject } from "./tools/inspect-fluent-project";
import { rankEvidence } from "./retrieval/source-ranker";
import { filterByReleaseFamily } from "./retrieval/release-filter";
import { expandServiceNowQuery } from "./retrieval/query-expander";
import type { ServiceNowEvidence, ServiceNowSourceType } from "./schemas/evidence";
import type { ServiceNowResearchAnswer } from "./schemas/research-answer";
import { verifyServiceNowAnswer, type VerificationResult } from "./answer-verifier";
import type { ServiceNowIntentResult } from "./schemas/domain-intent";
import type { ServiceNowResearchPlan } from "./types";
import type { CommandRunner } from "./core/command-runner";
import { defaultCommandRunner } from "./core/command-runner";
import { getPromptBundle } from "./prompts/index";

const WORKER_ROOT = "/";

export interface ServiceNowResearchResult {
  routed: boolean;
  answer?: ServiceNowResearchAnswer;
  evidence: ServiceNowEvidence[];
  systemPromptAddon: string;
  intent?: string;
  planSources?: string[];
  trace: TraceSink;
  // ---- enrichment for the visible /api/research trace ----
  classification?: ServiceNowIntentResult;
  plan?: ServiceNowResearchPlan;
  verification?: VerificationResult;
  expansions?: string[];
  /** Candidate evidence count per source type, before ranking/dedup. */
  candidateBySource?: Partial<Record<ServiceNowSourceType, number>>;
  sdkVersion?: string;
}

export class ServiceNowDomainPack {
  readonly policy: ServiceNowPolicy;
  readonly sdkExplain: DefaultSdkExplainProvider;
  readonly instanceQuery: DefaultServiceNowInstanceQueryProvider;
  readonly docs: HttpServiceNowDocsProvider;
  readonly repository: LocalFluentRepositoryProvider;
  readonly trace: TraceSink;

  constructor(
    readonly config: ServiceNowDomainConfig = defaultServiceNowDomainConfig(),
    options: {
      runner?: CommandRunner;
      trace?: TraceSink;
      fetchImpl?: typeof fetch;
    } = {}
  ) {
    this.trace = options.trace ?? new InMemoryTraceSink();
    const runner = options.runner ?? defaultCommandRunner;
    const fetchImpl = options.fetchImpl ?? fetch;
    this.policy = new ServiceNowPolicy(config);
    this.policy.assertReadOnlyRelease();
    this.sdkExplain = new DefaultSdkExplainProvider(
      config,
      runner,
      this.trace,
      fetchImpl
    );
    this.instanceQuery = new DefaultServiceNowInstanceQueryProvider(
      config,
      runner,
      (root) => this.sdkExplain.getInstalledVersion(root),
      this.trace
    );
    this.docs = new HttpServiceNowDocsProvider(config, fetchImpl, this.trace);
    this.repository = new LocalFluentRepositoryProvider(config, this.policy);
  }

  async research(
    query: string,
    context: AuthorizedResearchContext
  ): Promise<ServiceNowResearchResult> {
    const route = routeServiceNowQuery(query, this.config, {
      releaseFamily: this.config.documentation.releaseFamily,
      trace: this.trace,
    });

    if (!route.isServiceNow || !route.plan || !route.intent) {
      return {
        routed: false,
        evidence: [],
        systemPromptAddon: "",
        trace: this.trace,
        classification: route.intent,
      };
    }

    const projectRoot =
      this.config.sdk.projectRoot ||
      context.workingDirectory ||
      WORKER_ROOT;

    const evidence: ServiceNowEvidence[] = [];
    const candidateBySource: Partial<Record<ServiceNowSourceType, number>> = {};
    const bump = (t: ServiceNowSourceType, n: number) => {
      candidateBySource[t] = (candidateBySource[t] ?? 0) + n;
    };
    let toolCalls = 0;
    const expansions = expandServiceNowQuery(query);

    const projects = await this.sdkExplain.findFluentProjects(projectRoot);
    const activeRoot = projects[0]?.projectRoot || projectRoot;
    const sdkVersion = await this.sdkExplain.getInstalledVersion(activeRoot);
    route.plan.sdkVersion = sdkVersion ?? undefined;

    if (route.plan.sources.includes("sdk_explain") && this.config.sdk.enabled) {
      if (await this.sdkExplain.isExplainAvailable(activeRoot)) {
        await this.sdkExplain.orientProject(activeRoot);
        const sdkEvidence = await explainSdkTopic({
          provider: this.sdkExplain,
          projectRoot: activeRoot,
          query: expansions[0] || query,
          maxTopics: 3,
        });
        evidence.push(...sdkEvidence);
        bump("sdk_explain", sdkEvidence.length);
        toolCalls += sdkEvidence.length;
      }
    }

    if (route.plan.sources.includes("product_docs") && this.config.documentation.enabled) {
      const docsEvidence = await searchProductDocs({
        provider: this.docs,
        input: {
          query,
          expansions: expansions.slice(1, 4),
          releaseFamily: route.plan.releaseFamily,
          modules: route.intent.modules,
          limit: 4,
        },
      });
      evidence.push(...docsEvidence);
      bump("product_documentation", docsEvidence.length);
      toolCalls += 1;
    }

    if (route.plan.sources.includes("local_repository")) {
      const repo = await inspectFluentProject({
        provider: this.repository,
        projectRoot: activeRoot,
      });
      evidence.push(...repo.evidence);
      bump("local_repository", repo.evidence.length);
      toolCalls += 1;
    }

    if (
      route.plan.sources.includes("live_instance") &&
      route.plan.requiresLiveInstance &&
      context.allowLiveInstance
    ) {
      // Structured proposal only — callers/tests supply concrete table queries via tools.
      // The research workflow does not invent arbitrary instance queries from free text.
      toolCalls += 0;
    }

    const filtered = filterByReleaseFamily(evidence, route.plan.releaseFamily);
    const ranked = rankEvidence(filtered, route.intent.intent, {
      releaseFamily: route.plan.releaseFamily,
      sdkVersion: route.plan.sdkVersion,
    });

    this.trace.emit({
      name: "servicenow.evidence.ranked",
      timestamp: nowIso(),
      attributes: {
        count: ranked.length,
        toolCalls,
        sourceTypes: [...new Set(ranked.map((e) => e.sourceType))].join(","),
      },
    });

    const draft = synthesizeDraftAnswer({
      query,
      intent: route.intent.intent,
      evidence: ranked,
      releaseFamily: route.plan.releaseFamily,
      sdkVersion: route.plan.sdkVersion,
      requiresInstanceValidation: route.intent.requiresLiveInstance,
    });

    this.trace.emit({
      name: "servicenow.answer.generated",
      timestamp: nowIso(),
      attributes: { evidenceCount: ranked.length },
    });

    const verified = verifyServiceNowAnswer({
      draft,
      evidence: ranked,
      config: this.config,
      trace: this.trace,
    });

    const systemPromptAddon = getPromptBundle(route.intent.intent);

    // Discard task-scoped live results after completion
    this.instanceQuery.clearTask(context.taskId);

    return {
      routed: true,
      answer: verified.answer,
      evidence: ranked,
      systemPromptAddon,
      intent: route.intent.intent,
      planSources: route.plan.sources,
      trace: this.trace,
      classification: route.intent,
      plan: route.plan,
      verification: verified,
      expansions,
      candidateBySource,
      sdkVersion: route.plan.sdkVersion,
    };
  }
}

function synthesizeDraftAnswer(input: {
  query: string;
  intent: string;
  evidence: ServiceNowEvidence[];
  releaseFamily?: string;
  sdkVersion?: string;
  requiresInstanceValidation: boolean;
}): ServiceNowResearchAnswer {
  const top = input.evidence.slice(0, 4);
  const claimLinks = top.map((e) => ({
    evidenceId: e.id,
    claim: summarizeEvidenceClaim(e),
  }));

  // The draft is a retrieval summary, not a written answer: it says what was
  // found and from where. Prose is only produced when an answer model runs
  // (see pipeline.ts), so nothing here can read as an answered question.
  const directAnswer =
    top.length === 0
      ? "Insufficient ServiceNow evidence was retrieved for a confirmed answer."
      : `Retrieved ${top.length} ${describeSources(top)} source(s) for: ${input.query}`;

  const explanation = top
    .map((e, i) => `[${i + 1}] ${e.title} — ${sourceExcerpt(e)}`)
    .join("\n\n");

  return {
    summary: directAnswer,
    directAnswer,
    explanation,
    warnings: [],
    assumptions: top.length
      ? []
      : ["No authoritative ServiceNow evidence was available for this turn."],
    evidence: claimLinks,
    confidence: top.length >= 2 ? "high" : top.length === 1 ? "medium" : "low",
    releaseFamily: input.releaseFamily,
    sdkVersion: input.sdkVersion,
    requiresInstanceValidation: input.requiresInstanceValidation,
  };
}

function describeSources(evidence: ServiceNowEvidence[]): string {
  const types = [...new Set(evidence.map((e) => e.sourceType))];
  return types.join(", ");
}

function summarizeEvidenceClaim(e: ServiceNowEvidence): string {
  return `${e.title} supports the response for ${e.sourceType}`;
}

export function stripEvidenceWrapper(content: string): string {
  return content
    .replace(/BEGIN_UNTRUSTED_SERVICENOW_EVIDENCE\n?/g, "")
    .replace(/\n?END_UNTRUSTED_SERVICENOW_EVIDENCE/g, "")
    .trim();
}

/**
 * Display excerpt for one piece of evidence. Providers that produce prose set
 * `snippet`; anything else is cleaned here so front matter and index markup
 * never reach a reader.
 */
export function sourceExcerpt(evidence: ServiceNowEvidence, maxLength = 320): string {
  if (evidence.snippet) return evidence.snippet;
  const body = readableBody(stripEvidenceWrapper(evidence.content));
  return extractSnippet(body, [], maxLength);
}

export function createResearchContext(
  partial: Partial<AuthorizedResearchContext> & { taskId?: string } = {}
): AuthorizedResearchContext {
  return {
    taskId: partial.taskId || createId("task"),
    allowLiveInstance: partial.allowLiveInstance ?? false,
    workingDirectory: partial.workingDirectory,
    activeFilePath: partial.activeFilePath,
    tenantId: partial.tenantId,
    userId: partial.userId,
  };
}
