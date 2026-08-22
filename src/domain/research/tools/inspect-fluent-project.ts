import type { LocalFluentRepositoryProvider } from "../providers/repository-provider";
import { detectFluentDefinitions } from "../providers/repository-provider";
import type { ServiceNowEvidence } from "../schemas/evidence";
import type { DeletionRiskAssessment } from "../types";

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
