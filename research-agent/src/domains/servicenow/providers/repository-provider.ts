import { existsSync } from "node:fs";
import { readFile, readdir } from "node:fs/promises";
import { join, relative } from "node:path";
import { createId, nowIso } from "../../../core/types.js";
import type { ServiceNowEvidence } from "../schemas/evidence.js";
import { wrapUntrustedEvidence } from "../schemas/evidence.js";
import { sanitizeEvidenceContent } from "../security/prompt-injection.js";
import type { DeletionRiskAssessment } from "../types.js";
import { ServiceNowPolicy } from "../policy.js";
import type { ServiceNowDomainConfig } from "../config.js";

const FLUENT_DEFINITION_RE =
  /\b(Table|BusinessRule|Record|Acl|ScriptInclude|ClientScript|UiPolicy)\s*\(/g;

export interface RepositoryProvider {
  inspectFluentProject(projectRoot: string): Promise<ServiceNowEvidence[]>;
  findDefinitionUsages(
    projectRoot: string,
    symbol: string
  ): Promise<{ filePath: string; line: number; excerpt: string }[]>;
  assessDeletionRisk(
    projectRoot: string,
    filePath: string,
    definitionKind: string
  ): Promise<DeletionRiskAssessment>;
}

export class LocalFluentRepositoryProvider implements RepositoryProvider {
  constructor(
    _config: ServiceNowDomainConfig,
    private readonly policy = new ServiceNowPolicy(_config)
  ) {}

  async inspectFluentProject(projectRoot: string): Promise<ServiceNowEvidence[]> {
    const evidence: ServiceNowEvidence[] = [];
    const configPath = join(projectRoot, "now.config.json");
    if (existsSync(configPath)) {
      const content = await readFile(configPath, "utf8");
      evidence.push(makeRepoEvidence(projectRoot, "now.config.json", content, 1));
    }

    const keysPath = join(projectRoot, "src", "fluent", "keys.ts");
    const altKeys = join(projectRoot, "keys.ts");
    for (const path of [keysPath, altKeys]) {
      if (existsSync(path)) {
        const content = await readFile(path, "utf8");
        evidence.push(
          makeRepoEvidence(projectRoot, relative(projectRoot, path), content, 0.95)
        );
      }
    }

    const nowFiles = await findFiles(projectRoot, (name) => name.endsWith(".now.ts"));
    for (const file of nowFiles.slice(0, 25)) {
      const content = await readFile(file, "utf8");
      evidence.push(
        makeRepoEvidence(
          projectRoot,
          relative(projectRoot, file),
          content.slice(0, 6000),
          0.85
        )
      );
    }

    return evidence;
  }

  async findDefinitionUsages(
    projectRoot: string,
    symbol: string
  ): Promise<{ filePath: string; line: number; excerpt: string }[]> {
    const files = await findFiles(
      projectRoot,
      (name) =>
        name.endsWith(".ts") ||
        name.endsWith(".js") ||
        name.endsWith(".now.ts")
    );
    const hits: { filePath: string; line: number; excerpt: string }[] = [];
    for (const file of files) {
      const content = await readFile(file, "utf8");
      const lines = content.split(/\r?\n/);
      lines.forEach((line, idx) => {
        if (line.includes(symbol)) {
          hits.push({
            filePath: relative(projectRoot, file),
            line: idx + 1,
            excerpt: line.trim().slice(0, 200),
          });
        }
      });
    }
    return hits.slice(0, 50);
  }

  async assessDeletionRisk(
    projectRoot: string,
    filePath: string,
    definitionKind: string
  ): Promise<DeletionRiskAssessment> {
    const abs = join(projectRoot, filePath);
    const content = existsSync(abs) ? await readFile(abs, "utf8") : "";
    const keys = extractKeys(content);
    const dependents: string[] = [];
    for (const key of keys) {
      const usages = await this.findDefinitionUsages(projectRoot, key);
      for (const u of usages) {
        if (u.filePath !== filePath) dependents.push(`${u.filePath}:${u.line}`);
      }
    }
    return this.policy.assessFluentDeletion({
      definitionKind,
      filePath,
      keysReferenced: keys,
      dependentFiles: [...new Set(dependents)],
    });
  }
}

export function detectFluentDefinitions(content: string): string[] {
  const kinds = new Set<string>();
  let match: RegExpExecArray | null;
  const re = new RegExp(FLUENT_DEFINITION_RE.source, "g");
  while ((match = re.exec(content)) !== null) {
    kinds.add(match[1]!);
  }
  return [...kinds];
}

function extractKeys(content: string): string[] {
  const keys = new Set<string>();
  const re = /\$id:\s*Now\.(?:ID|ref)\((['"`])([^'"`]+)\1\)/g;
  let match: RegExpExecArray | null;
  while ((match = re.exec(content)) !== null) {
    keys.add(match[2]!);
  }
  return [...keys];
}

function makeRepoEvidence(
  projectRoot: string,
  filePath: string,
  content: string,
  relevance: number
): ServiceNowEvidence {
  return {
    id: createId("repo"),
    sourceType: "local_repository",
    title: `Repository: ${filePath}`,
    content: wrapUntrustedEvidence(sanitizeEvidenceContent(content)),
    sourceReference: `file://${filePath}`,
    projectRoot,
    filePath,
    retrievedAt: nowIso(),
    authorityScore: 0.8,
    relevanceScore: relevance,
    freshnessScore: 0.9,
    taskScoped: true,
    containsSensitiveData: false,
  };
}

async function findFiles(
  root: string,
  predicate: (name: string) => boolean
): Promise<string[]> {
  const out: string[] = [];
  async function walk(dir: string): Promise<void> {
    let entries;
    try {
      entries = await readdir(dir, { withFileTypes: true });
    } catch {
      return;
    }
    for (const entry of entries) {
      if (
        entry.name === "node_modules" ||
        entry.name === ".git" ||
        entry.name === "dist"
      ) {
        continue;
      }
      const full = join(dir, entry.name);
      if (entry.isDirectory()) {
        await walk(full);
      } else if (predicate(entry.name)) {
        out.push(full);
      }
    }
  }
  await walk(root);
  return out;
}
