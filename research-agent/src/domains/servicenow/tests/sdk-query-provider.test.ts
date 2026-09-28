import { describe, expect, it } from "vitest";
import { mkdtempSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { DefaultServiceNowInstanceQueryProvider } from "../providers/sdk-query-provider.js";
import { DefaultSdkExplainProvider } from "../providers/sdk-explain-provider.js";
import {
  defaultServiceNowDomainConfig,
  DEFAULT_METADATA_TABLES,
} from "../config.js";
import { createMockCommandRunner } from "../../../core/command-runner.js";
import { validateInstanceQuery } from "../security/query-allowlist.js";
import { isVersionAtLeast } from "../../../core/types.js";

describe("SDK query provider", () => {
  it("marks query unavailable below 4.8.0", async () => {
    expect(isVersionAtLeast("4.7.1", "4.8.0")).toBe(false);
    const root = mkdtempSync(join(tmpdir(), "sn-q-"));
    writeFileSync(
      join(root, "package.json"),
      JSON.stringify({
        devDependencies: { "@servicenow/sdk": "4.7.1" },
      })
    );
    const config = defaultServiceNowDomainConfig({
      instance: {
        ...defaultServiceNowDomainConfig().instance,
        enabled: true,
      },
    });
    const explain = new DefaultSdkExplainProvider(
      config,
      createMockCommandRunner({})
    );
    const provider = new DefaultServiceNowInstanceQueryProvider(
      config,
      createMockCommandRunner({}),
      (r) => explain.getInstalledVersion(r)
    );
    await expect(provider.isAvailable(root)).resolves.toBe(false);
  });

  it("enforces allowlist, fields, and max rows", () => {
    const config = defaultServiceNowDomainConfig({
      instance: {
        ...defaultServiceNowDomainConfig().instance,
        enabled: true,
        maximumRows: 10,
      },
    });

    const blocked = validateInstanceQuery(
      {
        table: "oauth_credential",
        encodedQuery: "nameISNOTEMPTY",
        fields: ["name"],
        limit: 100,
        purpose: "test",
      },
      config
    );
    expect(blocked.allowed).toBe(false);

    const business = validateInstanceQuery(
      {
        table: "incident",
        encodedQuery: "active=true",
        fields: ["number"],
        limit: 5,
        purpose: "test",
      },
      config
    );
    expect(business.allowed).toBe(false);

    const ok = validateInstanceQuery(
      {
        table: "sys_dictionary",
        encodedQuery: "name=incident^element=priority",
        fields: ["name", "element", "internal_type", "password"],
        limit: 100,
        purpose: "field type",
      },
      config
    );
    expect(ok.allowed).toBe(true);
    expect(ok.sanitizedLimit).toBe(10);
    expect(ok.sanitizedFields).not.toContain("password");
    expect(ok.sanitizedFields).toContain("element");
  });

  it("starts with metadata tables only", () => {
    expect(DEFAULT_METADATA_TABLES).toContain("sys_db_object");
    expect(DEFAULT_METADATA_TABLES).not.toContain("incident");
  });

  it("runs structured query commands when authorized", async () => {
    const root = mkdtempSync(join(tmpdir(), "sn-q2-"));
    writeFileSync(
      join(root, "package.json"),
      JSON.stringify({
        devDependencies: { "@servicenow/sdk": "4.9.0" },
      })
    );
    const config = defaultServiceNowDomainConfig({
      instance: {
        ...defaultServiceNowDomainConfig().instance,
        enabled: true,
      },
    });
    const runner = createMockCommandRunner({
      "query --help": {
        command: "qh",
        stdout: "query help",
        stderr: "",
        exitCode: 0,
      },
      "explain query": {
        command: "eq",
        stdout: "query docs",
        stderr: "",
        exitCode: 0,
      },
      "explain encoded-query-guide": {
        command: "eqg",
        stdout: "encoded query docs",
        stderr: "",
        exitCode: 0,
      },
      "sdk query": {
        command: "q",
        stdout: JSON.stringify([
          { name: "incident", element: "priority", internal_type: "integer" },
        ]),
        stderr: "",
        exitCode: 0,
      },
    });
    const explain = new DefaultSdkExplainProvider(config, runner);
    const provider = new DefaultServiceNowInstanceQueryProvider(
      config,
      runner,
      (r) => explain.getInstalledVersion(r)
    );

    const result = await provider.query(
      {
        table: "sys_dictionary",
        encodedQuery: "name=incident^element=priority",
        fields: ["name", "element", "internal_type"],
        limit: 5,
        purpose: "field type",
      },
      { taskId: "t1", allowLiveInstance: true, projectRoot: root }
    );
    expect(result.persisted).toBe(false);
    expect(result.taskScoped).toBe(true);
    expect(result.rowCount).toBe(1);
    provider.clearTask("t1");
  });
});
