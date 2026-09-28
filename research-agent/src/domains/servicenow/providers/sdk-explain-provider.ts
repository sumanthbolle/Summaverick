import { createHash } from "node:crypto";
import { existsSync } from "node:fs";
import { readFile } from "node:fs/promises";
import { dirname, join, resolve } from "node:path";
import {
  createId,
  isVersionAtLeast,
  nowIso,
  type TraceSink,
} from "../../../core/types.js";
import type { CommandRunner } from "../../../core/command-runner.js";
import { defaultCommandRunner } from "../../../core/command-runner.js";
import type { ServiceNowDomainConfig } from "../config.js";
import type { ServiceNowEvidence } from "../schemas/evidence.js";
import { wrapUntrustedEvidence } from "../schemas/evidence.js";
import { sanitizeEvidenceContent } from "../security/prompt-injection.js";
import { evaluateSdkCommand } from "../security/command-policy.js";
import type {
  FluentProjectInfo,
  SdkEvidenceMetadata,
  SdkProjectOrientation,
  SdkTopicMatch,
  SdkTopicPreview,
} from "../types.js";

const VERSIONS_URL = "https://servicenow.github.io/sdk/versions.json";

export interface SdkExplainProvider {
  getInstalledVersion(projectRoot: string): Promise<string | null>;
  findFluentProjects(startDir: string): Promise<FluentProjectInfo[]>;
  resolveDocumentationVersion(
    installedVersion: string | null
  ): Promise<{ documentationVersion: string; versionMismatch: boolean }>;
  orientProject(projectRoot: string): Promise<SdkProjectOrientation>;
  searchTopics(projectRoot: string, searchTerm: string): Promise<SdkTopicMatch[]>;
  peekTopic(projectRoot: string, topic: string): Promise<SdkTopicPreview>;
  readTopic(projectRoot: string, topic: string): Promise<ServiceNowEvidence>;
  isExplainAvailable(projectRoot: string): Promise<boolean>;
}

interface VersionsManifest {
  latest: { version: string; url: string; llms: string };
  versions: Array<{ version: string; url: string; llms: string }>;
}

export class DefaultSdkExplainProvider implements SdkExplainProvider {
  private readonly orientationCache = new Map<string, SdkProjectOrientation>();
  private readonly topicCache = new Map<string, ServiceNowEvidence>();
  private readonly helpCache = new Set<string>();
  private versionsManifest: VersionsManifest | null = null;

  constructor(
    private readonly config: ServiceNowDomainConfig,
    private readonly runner: CommandRunner = defaultCommandRunner,
    private readonly trace?: TraceSink,
    private readonly fetchImpl: typeof fetch = fetch
  ) {}

  async getInstalledVersion(projectRoot: string): Promise<string | null> {
    const pkgPath = join(projectRoot, "package.json");
    if (!existsSync(pkgPath)) return null;
    try {
      const raw = await readFile(pkgPath, "utf8");
      const pkg = JSON.parse(raw) as {
        dependencies?: Record<string, string>;
        devDependencies?: Record<string, string>;
      };
      const pinned =
        pkg.devDependencies?.["@servicenow/sdk"] ||
        pkg.dependencies?.["@servicenow/sdk"];
      if (pinned) return pinned.replace(/^[\^~>=]*/, "");

      const list = await this.runAllowed(
        "npm list @servicenow/sdk --depth=0 --json",
        projectRoot
      );
      if (list.exitCode === 0 && list.stdout) {
        const parsed = JSON.parse(list.stdout) as {
          dependencies?: { "@servicenow/sdk"?: { version?: string } };
        };
        return parsed.dependencies?.["@servicenow/sdk"]?.version ?? null;
      }
    } catch {
      return null;
    }
    return null;
  }

  async findFluentProjects(startDir: string): Promise<FluentProjectInfo[]> {
    const roots = new Map<string, FluentProjectInfo>();
    let dir = resolve(startDir);
    for (;;) {
      const configPath = join(dir, "now.config.json");
      if (existsSync(configPath)) {
        const installedSdkVersion = await this.getInstalledVersion(dir);
        roots.set(dir, {
          projectRoot: dir,
          configPath,
          packageJsonPath: existsSync(join(dir, "package.json"))
            ? join(dir, "package.json")
            : undefined,
          installedSdkVersion,
        });
      }
      const parent = dirname(dir);
      if (parent === dir) break;
      dir = parent;
    }
    return [...roots.values()];
  }

  async resolveDocumentationVersion(
    installedVersion: string | null
  ): Promise<{ documentationVersion: string; versionMismatch: boolean }> {
    const manifest = await this.loadVersions();
    if (!installedVersion) {
      return {
        documentationVersion: manifest.latest.version,
        versionMismatch: false,
      };
    }
    const exact = manifest.versions.find((v) => v.version === installedVersion);
    if (exact) {
      return { documentationVersion: exact.version, versionMismatch: false };
    }
    const stables = manifest.versions.filter(
      (v) => !v.version.includes("-")
    );
    let best = stables[0]?.version ?? manifest.latest.version;
    let bestDistance = Number.POSITIVE_INFINITY;
    for (const v of stables) {
      const distance = Math.abs(
        semverToNumber(v.version) - semverToNumber(installedVersion)
      );
      if (
        isVersionAtLeast(installedVersion, v.version) &&
        distance <= bestDistance
      ) {
        best = v.version;
        bestDistance = distance;
      }
    }

    return {
      documentationVersion: best,
      versionMismatch: best !== installedVersion,
    };
  }

  async isExplainAvailable(projectRoot: string): Promise<boolean> {
    const version = await this.getInstalledVersion(projectRoot);
    if (!version) return false;
    return isVersionAtLeast(version, this.config.sdk.minimumExplainVersion);
  }

  async orientProject(projectRoot: string): Promise<SdkProjectOrientation> {
    this.trace?.emit({
      name: "servicenow.sdk.orientation.started",
      timestamp: nowIso(),
      attributes: { projectRoot },
    });

    const installedSdkVersion = await this.getInstalledVersion(projectRoot);
    const lockHash = await hashFileIfExists(join(projectRoot, "package-lock.json"));
    const cacheKey = `${projectRoot}|${installedSdkVersion ?? "none"}|${lockHash ?? "nolock"}`;
    const cached = this.orientationCache.get(cacheKey);
    if (cached) return cached;

    if (installedSdkVersion && !isVersionAtLeast(installedSdkVersion, "4.6.0")) {
      const limited: SdkProjectOrientation = {
        projectRoot,
        installedSdkVersion,
        packageLockHash: lockHash,
        quickstartTopics: [],
        fluentLanguageTopics: [],
        helpText: `SDK ${installedSdkVersion} is below 4.6.0; explain is unavailable.`,
        cachedAt: nowIso(),
      };
      this.orientationCache.set(cacheKey, limited);
      return limited;
    }

    await this.ensureHelp(projectRoot, "explain");

    const help = await this.runAllowed("npx @servicenow/sdk --help", projectRoot);
    const quickstart = await this.runAllowed(
      "npx @servicenow/sdk explain quickstart --list --format=raw",
      projectRoot
    );
    const fluent = await this.runAllowed(
      "npx @servicenow/sdk explain fluent-language --list --format=raw",
      projectRoot
    );
    const keys = await this.runAllowed(
      "npx @servicenow/sdk explain keys-file --format=raw",
      projectRoot
    );

    const orientation: SdkProjectOrientation = {
      projectRoot,
      installedSdkVersion,
      packageLockHash: lockHash,
      quickstartTopics: parseTopicList(quickstart.stdout),
      fluentLanguageTopics: parseTopicList(fluent.stdout),
      helpText: help.stdout,
      keysFileDocs: keys.stdout,
      cachedAt: nowIso(),
    };

    this.orientationCache.set(cacheKey, orientation);
    this.trace?.emit({
      name: "servicenow.sdk.orientation.completed",
      timestamp: nowIso(),
      attributes: {
        projectRoot,
        sdkVersion: installedSdkVersion,
        topicCount:
          orientation.quickstartTopics.length +
          orientation.fluentLanguageTopics.length,
      },
    });
    return orientation;
  }

  async searchTopics(
    projectRoot: string,
    searchTerm: string
  ): Promise<SdkTopicMatch[]> {
    await this.ensureHelp(projectRoot, "explain");
    const result = await this.runAllowed(
      `npx @servicenow/sdk explain ${shellArg(searchTerm)} --list --format=raw`,
      projectRoot
    );
    this.trace?.emit({
      name: "servicenow.sdk.explain.searched",
      timestamp: nowIso(),
      attributes: { searchTerm, projectRoot },
    });
    return parseTopicList(result.stdout).map((topic, index) => ({
      topic,
      score: Math.max(0.2, 1 - index * 0.05),
      snippet: topic,
    }));
  }

  async peekTopic(projectRoot: string, topic: string): Promise<SdkTopicPreview> {
    await this.ensureHelp(projectRoot, "explain");
    const installed = await this.getInstalledVersion(projectRoot);
    const docs = await this.resolveDocumentationVersion(installed);
    const result = await this.runAllowed(
      `npx @servicenow/sdk explain ${shellArg(topic)} --peek --format=raw`,
      projectRoot
    );
    return {
      topic,
      preview: result.stdout.slice(0, 1200),
      documentationVersion: docs.documentationVersion,
    };
  }

  async readTopic(projectRoot: string, topic: string): Promise<ServiceNowEvidence> {
    await this.ensureHelp(projectRoot, "explain");
    const installed = await this.getInstalledVersion(projectRoot);
    const docs = await this.resolveDocumentationVersion(installed);
    const cacheKey = `${docs.documentationVersion}|${topic}|${projectRoot}`;
    const cached = this.topicCache.get(cacheKey);
    if (cached) return cached;

    const result = await this.runAllowed(
      `npx @servicenow/sdk explain ${shellArg(topic)} --format=raw`,
      projectRoot
    );

    const content = sanitizeEvidenceContent(result.stdout || result.stderr);
    const meta: SdkEvidenceMetadata = {
      installedSdkVersion: installed ?? undefined,
      documentationVersion: docs.documentationVersion,
      topic,
      projectRoot,
      versionMismatch: docs.versionMismatch,
    };

    const evidence: ServiceNowEvidence = {
      id: createId("sdk"),
      sourceType: "sdk_explain",
      title: `SDK explain: ${topic}`,
      content: wrapUntrustedEvidence(content),
      sourceReference: `sdk://explain/${topic}`,
      canonicalUrl: `https://servicenow.github.io/sdk/${docs.documentationVersion}/`,
      sdkVersion: installed ?? undefined,
      documentationVersion: docs.documentationVersion,
      projectRoot,
      retrievedAt: nowIso(),
      authorityScore: 0.95,
      relevanceScore: 0.7,
      freshnessScore: 0.9,
      taskScoped: false,
      containsSensitiveData: false,
    };

    if (meta.versionMismatch) {
      evidence.content = wrapUntrustedEvidence(
        `[VERSION MISMATCH] Installed SDK ${installed} vs documentation ${docs.documentationVersion}.\n\n${content}`
      );
    }

    this.topicCache.set(cacheKey, evidence);
    this.trace?.emit({
      name: "servicenow.sdk.topic.read",
      timestamp: nowIso(),
      attributes: {
        topic,
        documentationVersion: docs.documentationVersion,
        installedSdkVersion: installed,
      },
    });
    return evidence;
  }

  private async ensureHelp(projectRoot: string, subcommand: string): Promise<void> {
    const key = `${projectRoot}|${subcommand}`;
    if (this.helpCache.has(key)) return;
    await this.runAllowed(
      `npx @servicenow/sdk ${subcommand} --help`,
      projectRoot
    );
    this.helpCache.add(key);
  }

  private async runAllowed(command: string, cwd: string) {
    // npm list / cat package.json are allowlisted separately
    if (
      command.startsWith("npm list") ||
      command.startsWith("cat package.json")
    ) {
      return this.runner(command, { cwd });
    }
    const decision = evaluateSdkCommand(command, {
      permitWriteOperations: false,
    });
    if (!decision.allowed || decision.requiresApproval) {
      throw new Error(decision.reason);
    }
    return this.runner(decision.normalizedCommand, { cwd });
  }

  private async loadVersions(): Promise<VersionsManifest> {
    if (this.versionsManifest) return this.versionsManifest;
    try {
      const res = await this.fetchImpl(VERSIONS_URL);
      if (!res.ok) throw new Error(`versions.json HTTP ${res.status}`);
      this.versionsManifest = (await res.json()) as VersionsManifest;
      return this.versionsManifest;
    } catch {
      return {
        latest: {
          version: "4.9.0",
          url: "https://servicenow.github.io/sdk/",
          llms: "https://servicenow.github.io/sdk/llms.txt",
        },
        versions: [
          {
            version: "4.9.0",
            url: "https://servicenow.github.io/sdk/4.9.0/",
            llms: "https://servicenow.github.io/sdk/4.9.0/llms.txt",
          },
        ],
      };
    }
  }
}

function parseTopicList(stdout: string): string[] {
  return stdout
    .split(/\r?\n/)
    .map((l) => l.trim())
    .filter((l) => l && !l.startsWith("#") && !l.toLowerCase().includes("usage:"));
}

function shellArg(value: string): string {
  if (/^[A-Za-z0-9_./:@-]+$/.test(value)) return value;
  return `'${value.replace(/'/g, `'\\''`)}'`;
}

async function hashFileIfExists(path: string): Promise<string | undefined> {
  if (!existsSync(path)) return undefined;
  const raw = await readFile(path);
  return createHash("sha256").update(raw).digest("hex").slice(0, 16);
}

function semverToNumber(version: string): number {
  const [a, b, c] = version.replace(/^v/, "").split("-")[0]!.split(".").map(Number);
  return (a || 0) * 1_000_000 + (b || 0) * 1_000 + (c || 0);
}
