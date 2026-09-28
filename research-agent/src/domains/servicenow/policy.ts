import type { ServiceNowDomainConfig } from "./config.js";
import { evaluateSdkCommand } from "./security/command-policy.js";
import { validateInstanceQuery } from "./security/query-allowlist.js";
import type { ServiceNowInstanceQueryInput } from "./schemas/instance-query.js";
import type { DeletionRiskAssessment } from "./types.js";

export interface PolicyAuditEntry {
  timestamp: string;
  action: string;
  allowed: boolean;
  detail: string;
}

export class ServiceNowPolicy {
  readonly auditLog: PolicyAuditEntry[] = [];

  constructor(private readonly config: ServiceNowDomainConfig) {}

  get permitWriteOperations(): false {
    return this.config.security.permitWriteOperations;
  }

  get persistLiveInstanceResults(): false {
    return this.config.security.persistLiveInstanceResults;
  }

  assertReadOnlyRelease(): void {
    if (this.config.security.permitWriteOperations) {
      throw new Error(
        "ServiceNow domain pack must remain read-only; permitWriteOperations is locked false."
      );
    }
  }

  evaluateCommand(command: string) {
    const decision = evaluateSdkCommand(command, {
      permitWriteOperations: this.config.security.permitWriteOperations,
    });
    this.audit({
      action: "sdk.command.evaluate",
      allowed: decision.allowed && !decision.requiresApproval,
      detail: decision.reason,
    });
    return decision;
  }

  evaluateInstanceQuery(input: ServiceNowInstanceQueryInput) {
    const decision = validateInstanceQuery(input, this.config);
    this.audit({
      action: "instance.query.evaluate",
      allowed: decision.allowed,
      detail: decision.reason,
    });
    return decision;
  }

  assessFluentDeletion(input: {
    definitionKind: string;
    filePath: string;
    keysReferenced: string[];
    dependentFiles: string[];
  }): DeletionRiskAssessment {
    const assessment: DeletionRiskAssessment = {
      definitionKind: input.definitionKind,
      filePath: input.filePath,
      keysReferenced: input.keysReferenced,
      dependentFiles: input.dependentFiles,
      requiresUserApproval: true,
      safeToAutoDelete: false,
      impactSummary: `Deleting Fluent ${input.definitionKind} in ${input.filePath} may remove platform metadata on deploy/upgrade. Keys: ${
        input.keysReferenced.join(", ") || "(none found)"
      }. Dependents: ${
        input.dependentFiles.join(", ") || "(none found)"
      }. Explicit user approval is required.`,
    };
    this.audit({
      action: "fluent.deletion.blocked",
      allowed: false,
      detail: assessment.impactSummary,
    });
    return assessment;
  }

  private audit(entry: Omit<PolicyAuditEntry, "timestamp">): void {
    this.auditLog.push({
      ...entry,
      timestamp: new Date().toISOString(),
    });
  }
}
