import { describe, expect, it } from "vitest";
import { mkdtempSync, writeFileSync, mkdirSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import {
  ServiceNowDomainPack,
  createResearchContext,
  defaultServiceNowDomainConfig,
} from "../index.js";
import { createMockCommandRunner } from "../../../core/command-runner.js";

const SAMPLE_INDEX = `
- [IT Service Management](https://raw.githubusercontent.com/ServiceNow/ServiceNowDocs/australia/markdown/it-service-management/index.md)
- [Business rules](https://raw.githubusercontent.com/ServiceNow/ServiceNowDocs/australia/markdown/now-platform/business-rules.md)
`;

describe("ServiceNow domain pack integration", () => {
  it("keeps non-ServiceNow research unrouted", async () => {
    const pack = new ServiceNowDomainPack(defaultServiceNowDomainConfig(), {
      runner: createMockCommandRunner({}),
      fetchImpl: async () => new Response(SAMPLE_INDEX, { status: 200 }),
    });
    const result = await pack.research(
      "What are the best restaurants in Lisbon?",
      createResearchContext({ allowLiveInstance: false })
    );
    expect(result.routed).toBe(false);
  });

  it("routes ServiceNow questions and returns cited evidence-backed answers", async () => {
    const root = mkdtempSync(join(tmpdir(), "sn-int-"));
    writeFileSync(join(root, "now.config.json"), "{}");
    writeFileSync(
      join(root, "package.json"),
      JSON.stringify({
        devDependencies: { "@servicenow/sdk": "4.9.0" },
      })
    );
    mkdirSync(join(root, "src"), { recursive: true });
    writeFileSync(
      join(root, "src", "rule.now.ts"),
      `import { BusinessRule } from "@servicenow/sdk/core";\nexport const br = BusinessRule({\n  $id: Now.ID['br'],\n  name: "demo",\n});\n`
    );

    const runner = createMockCommandRunner({
      "sdk --help": {
        command: "help",
        stdout: "help",
        stderr: "",
        exitCode: 0,
      },
      "explain --help": {
        command: "eh",
        stdout: "explain help",
        stderr: "",
        exitCode: 0,
      },
      "explain quickstart --list": {
        command: "qs",
        stdout: "quickstart\n",
        stderr: "",
        exitCode: 0,
      },
      "explain fluent-language --list": {
        command: "fl",
        stdout: "BusinessRule\nTable\n",
        stderr: "",
        exitCode: 0,
      },
      "explain keys-file": {
        command: "kf",
        stdout: "keys file docs",
        stderr: "",
        exitCode: 0,
      },
      "explain BusinessRule --list": {
        command: "list",
        stdout: "BusinessRule\n",
        stderr: "",
        exitCode: 0,
      },
      "explain BusinessRule --peek": {
        command: "peek",
        stdout: "BusinessRule preview",
        stderr: "",
        exitCode: 0,
      },
      "explain BusinessRule --format=raw": {
        command: "full",
        stdout: "BusinessRule creates server-side logic in Fluent apps.",
        stderr: "",
        exitCode: 0,
      },
      "explain structure --list": {
        command: "slist",
        stdout: "structure\n",
        stderr: "",
        exitCode: 0,
      },
      "explain structure --peek": {
        command: "speek",
        stdout: "structure preview",
        stderr: "",
        exitCode: 0,
      },
      "explain structure --format=raw": {
        command: "sfull",
        stdout: "Fluent project structure docs",
        stderr: "",
        exitCode: 0,
      },
      "explain naming --list": {
        command: "nlist",
        stdout: "naming\n",
        stderr: "",
        exitCode: 0,
      },
      "explain naming --peek": {
        command: "npeek",
        stdout: "naming preview",
        stderr: "",
        exitCode: 0,
      },
      "explain naming --format=raw": {
        command: "nfull",
        stdout: "Fluent naming docs",
        stderr: "",
        exitCode: 0,
      },
    });

    const fetchImpl: typeof fetch = async (input) => {
      const url = String(input);
      if (url.includes("versions.json")) {
        return new Response(
          JSON.stringify({
            latest: { version: "4.9.0", url: "", llms: "" },
            versions: [{ version: "4.9.0", url: "", llms: "" }],
          }),
          { status: 200 }
        );
      }
      if (url.includes("llms.txt")) {
        return new Response(SAMPLE_INDEX, { status: 200 });
      }
      return new Response(
        "---\ntitle: Business rules\n---\n# Business rules\nServer-side logic in ServiceNow.",
        { status: 200 }
      );
    };

    const pack = new ServiceNowDomainPack(
      defaultServiceNowDomainConfig({
        sdk: {
          enabled: true,
          projectRoot: root,
          minimumExplainVersion: "4.6.0",
          minimumQueryVersion: "4.8.0",
        },
        instance: {
          ...defaultServiceNowDomainConfig().instance,
          enabled: false,
        },
      }),
      { runner, fetchImpl }
    );

    const result = await pack.research(
      "Create a BusinessRule using Fluent SDK",
      createResearchContext({
        allowLiveInstance: false,
        workingDirectory: root,
      })
    );

    expect(result.routed).toBe(true);
    expect(result.intent).toBe("fluent_sdk");
    expect(result.evidence.length).toBeGreaterThan(0);
    expect(result.answer?.evidence.length).toBeGreaterThan(0);
    expect(result.answer?.sdkVersion).toBe("4.9.0");
    expect(result.systemPromptAddon).toContain("UNTRUSTED_SERVICENOW_EVIDENCE");
  });

  it("does not persist live instance results as domain knowledge", async () => {
    const config = defaultServiceNowDomainConfig();
    expect(config.security.persistLiveInstanceResults).toBe(false);
    expect(config.security.permitWriteOperations).toBe(false);
    expect(config.instance.enabled).toBe(false);
  });
});
