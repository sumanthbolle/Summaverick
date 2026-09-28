import {
  createId,
  nowIso,
  type AuthorizedResearchContext,
  type TraceSink,
  InMemoryTraceSink,
} from "../../core/types.js";
import type { ServiceNowDomainConfig } from "./config.js";
import { defaultServiceNowDomainConfig } from "./config.js";
import { ServiceNowPolicy } from "./policy.js";
import { routeServiceNowQuery } from "./router.js";
import { DefaultSdkExplainProvider } from "./providers/sdk-explain-provider.js";
import { DefaultServiceNowInstanceQueryProvider } from "./providers/sdk-query-provider.js";
import { HttpServiceNowDocsProvider } from "./providers/servicenow-docs-provider.js";
import { LocalFluentRepositoryProvider } from "./providers/repository-provider.js";
import { explainSdkTopic } from "./tools/explain-sdk-topic.js";
import { searchProductDocs } from "./tools/search-product-docs.js";
import { inspectFluentProject } from "./tools/inspect-fluent-project.js";
import { rankEvidence } from "./retrieval/source-ranker.js";
import { filterByReleaseFamily } from "./retrieval/release-filter.js";
import { expandServiceNowQuery } from "./retrieval/query-expander.js";
import type { ServiceNowEvidence } from "./schemas/evidence.js";
import type { ServiceNowResearchAnswer } from "./schemas/research-answer.js";
import { verifyServiceNowAnswer } from "./answer-verifier.js";
import type { CommandRunner } from "../../core/command-runner.js";
import { defaultCommandRunner } from "../../core/command-runner.js";
import { getPromptBundle } from "./prompts/index.js";

export interface ServiceNowResearchResult {
  routed: boolean;
  answer?: ServiceNowResearchAnswer;
  evidence: ServiceNowEvidence[];
  systemPromptAddon: string;
  intent?: string;
  planSources?: string[];
  trace: TraceSink;
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
      };
    }

    const projectRoot =
      this.config.sdk.projectRoot ||
      context.workingDirectory ||
      process.cwd();

    const evidence: ServiceNowEvidence[] = [];
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
        toolCalls += sdkEvidence.length;
      }
    }

    if (route.plan.sources.includes("product_docs") && this.config.documentation.enabled) {
      const docsEvidence = await searchProductDocs({
        provider: this.docs,
        input: {
          query: expansions.slice(0, 3).join(" "),
          releaseFamily: route.plan.releaseFamily,
          modules: route.intent.modules,
          limit: 4,
        },
      });
      evidence.push(...docsEvidence);
      toolCalls += 1;
    }

    if (route.plan.sources.includes("local_repository")) {
      const repo = await inspectFluentProject({
        provider: this.repository,
        projectRoot: activeRoot,
      });
      evidence.push(...repo.evidence);
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

  const directAnswer =
    top.length === 0
      ? "Insufficient ServiceNow evidence was retrieved for a confirmed answer."
      : `Based on ${describeSources(top)}, here is the evidence-backed answer for: ${input.query}`;

  const explanation = top
    .map((e, i) => `[${i + 1}] ${e.title}: ${stripEvidenceWrapper(e.content).slice(0, 400)}`)
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

function stripEvidenceWrapper(content: string): string {
  return content
    .replace(/BEGIN_UNTRUSTED_SERVICENOW_EVIDENCE\n?/g, "")
    .replace(/\n?END_UNTRUSTED_SERVICENOW_EVIDENCE/g, "")
    .trim();
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
