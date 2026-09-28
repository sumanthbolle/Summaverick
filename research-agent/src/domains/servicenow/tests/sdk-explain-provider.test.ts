import { describe, expect, it } from "vitest";
import { mkdtempSync, writeFileSync, mkdirSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { DefaultSdkExplainProvider } from "../providers/sdk-explain-provider.js";
import { defaultServiceNowDomainConfig } from "../config.js";
import { createMockCommandRunner } from "../../../core/command-runner.js";
import { isVersionAtLeast } from "../../../core/types.js";

function makeProject(version: string): string {
  const root = mkdtempSync(join(tmpdir(), "sn-fluent-"));
  writeFileSync(
    join(root, "now.config.json"),
    JSON.stringify({ scope: "x_demo", name: "Demo" }, null, 2)
  );
  writeFileSync(
    join(root, "package.json"),
    JSON.stringify(
      {
        name: "demo",
        devDependencies: { "@servicenow/sdk": version },
      },
      null,
      2
    )
  );
  return root;
}

describe("SDK explain provider", () => {
  it("detects installed SDK version from package.json", async () => {
    const root = makeProject("4.9.0");
    const provider = new DefaultSdkExplainProvider(
      defaultServiceNowDomainConfig(),
      createMockCommandRunner({})
    );
    await expect(provider.getInstalledVersion(root)).resolves.toBe("4.9.0");
  });

  it("finds multiple now.config.json projects when walking upward", async () => {
    const parent = makeProject("4.8.0");
    const child = join(parent, "apps", "child");
    mkdirSync(child, { recursive: true });
    writeFileSync(join(child, "now.config.json"), "{}");
    writeFileSync(
      join(child, "package.json"),
      JSON.stringify({
        devDependencies: { "@servicenow/sdk": "4.9.0" },
      })
    );

    const provider = new DefaultSdkExplainProvider(
      defaultServiceNowDomainConfig(),
      createMockCommandRunner({})
    );
    const projects = await provider.findFluentProjects(child);
    expect(projects.length).toBeGreaterThanOrEqual(2);
    expect(projects.some((p) => p.projectRoot === child)).toBe(true);
    expect(projects.some((p) => p.projectRoot === parent)).toBe(true);
  });

  it("marks explain unavailable below 4.6.0", async () => {
    const root = makeProject("4.5.0");
    const provider = new DefaultSdkExplainProvider(
      defaultServiceNowDomainConfig(),
      createMockCommandRunner({})
    );
    expect(isVersionAtLeast("4.5.0", "4.6.0")).toBe(false);
    await expect(provider.isExplainAvailable(root)).resolves.toBe(false);
  });

  it("follows search → peek → read sequence without opening every topic", async () => {
    const root = makeProject("4.9.0");
    const calls: string[] = [];
    const runner = createMockCommandRunner({
      "explain --help": {
        command: "help",
        stdout: "explain help",
        stderr: "",
        exitCode: 0,
      },
      "explain BusinessRule --list": {
        command: "list",
        stdout: "BusinessRule\nBusinessRule-advanced\n",
        stderr: "",
        exitCode: 0,
      },
      "explain BusinessRule --peek": {
        command: "peek",
        stdout: "Preview of BusinessRule",
        stderr: "",
        exitCode: 0,
      },
      "explain BusinessRule --format=raw": (cmd) => {
        calls.push(cmd);
        return {
          command: cmd,
          stdout: "Full BusinessRule docs",
          stderr: "",
          exitCode: 0,
        };
      },
      "explain BusinessRule-advanced --peek": {
        command: "peek2",
        stdout: "Preview advanced",
        stderr: "",
        exitCode: 0,
      },
      "explain BusinessRule-advanced --format=raw": (cmd) => {
        calls.push(cmd);
        return {
          command: cmd,
          stdout: "Full advanced",
          stderr: "",
          exitCode: 0,
        };
      },
    });

    const provider = new DefaultSdkExplainProvider(
      defaultServiceNowDomainConfig(),
      runner,
      undefined,
      async () =>
        new Response(
          JSON.stringify({
            latest: { version: "4.9.0", url: "", llms: "" },
            versions: [{ version: "4.9.0", url: "", llms: "" }],
          }),
          { status: 200 }
        )
    );

    const matches = await provider.searchTopics(root, "BusinessRule");
    expect(matches[0]?.topic).toBe("BusinessRule");
    const peek = await provider.peekTopic(root, matches[0]!.topic);
    expect(peek.preview).toContain("Preview");
    const full = await provider.readTopic(root, matches[0]!.topic);
    expect(full.sourceType).toBe("sdk_explain");
    expect(full.content).toContain("BEGIN_UNTRUSTED_SERVICENOW_EVIDENCE");
    expect(full.documentationVersion).toBe("4.9.0");
  });
});
