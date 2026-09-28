import type { LocalFluentRepositoryProvider } from "../providers/repository-provider.js";
import { detectFluentDefinitions } from "../providers/repository-provider.js";
import type { ServiceNowEvidence } from "../schemas/evidence.js";
import type { DeletionRiskAssessment } from "../types.js";

export async function inspectFluentProject(options: {
  provider: LocalFluentRepositoryProvider;
  projectRoot: string;
}): Promise<{
  evidence: ServiceNowEvidence[];
  definitionKinds: string[];
}> {
  const evidence = await options.provider.inspectFluentProject(options.projectRoot);
  const definitionKinds = new Set<string>();
  for (const item of evidence) {
    for (const kind of detectFluentDefinitions(item.content)) {
      definitionKinds.add(kind);
    }
  }
  return { evidence, definitionKinds: [...definitionKinds] };
}

export async function checkFluentDeletionSafety(options: {
  provider: LocalFluentRepositoryProvider;
  projectRoot: string;
  filePath: string;
  definitionKind: string;
}): Promise<DeletionRiskAssessment> {
  return options.provider.assessDeletionRisk(
    options.projectRoot,
    options.filePath,
    options.definitionKind
  );
}
