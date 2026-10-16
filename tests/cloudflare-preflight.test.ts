import { spawn } from "node:child_process";
import { fileURLToPath } from "node:url";
import { describe, expect, it } from "vitest";

const root = fileURLToPath(new URL("../", import.meta.url));
const script = new URL("../scripts/check-cloudflare-auth.mjs", import.meta.url).href;
const accountId = "a".repeat(32);
const databaseId = "e60dc612-a809-418b-b2af-46eb39b5d11f";
const active = { success: true, result: { status: "active" } };
const rejected = { success: false, errors: [{ code: 1000, message: "Invalid API Token" }] };

async function preflight(options: {
  token?: string;
  accountId?: string;
  user?: unknown;
  account?: unknown;
  workersAllowed?: boolean;
  d1Allowed?: boolean;
} = {}) {
  const id = options.accountId ?? accountId;
  // All HTTP calls are scripted fixtures; no credentials or network are used.
  const fixture = {
    "/user/tokens/verify": options.user ?? active,
    [`/accounts/${id}/tokens/verify`]: options.account ?? active,
    [`/zones/${id}`]: rejected,
    "/accounts": { success: true, result: [{ id, name: "Fixture account" }] },
    [`/accounts/${id}`]: { success: true, result: { name: "Fixture account" } },
    [`/accounts/${id}/workers/scripts`]: options.workersAllowed === false ? rejected : { success: true, result: [] },
    [`/accounts/${id}/d1/database`]: options.d1Allowed === false ? rejected : { success: true, result: [{ uuid: databaseId, name: "summaverick" }] },
  };
  const source = `
    const fixtures=${JSON.stringify(fixture)};
    const paths=[];
    globalThis.fetch=async (url, init)=>{
      const path=new URL(url).pathname.replace('/client/v4','');
      paths.push(path);
      if(!fixtures[path]) throw new Error('Unexpected API request: '+path);
      return Response.json(fixtures[path]);
    };
    process.on('exit',()=>console.log('REQUEST_PATHS='+JSON.stringify(paths)));
    await import(${JSON.stringify(script)});
  `;
  return new Promise<{ code: number | null; output: string; paths: string[] }>((resolve, reject) => {
    const child = spawn(process.execPath, ["--input-type=module", "-e", source], {
      cwd: root, timeout: 5000,
      env: {
        ...process.env,
        CLOUDFLARE_API_TOKEN: options.token ?? "cfut_synthetic_fixture",
        CLOUDFLARE_ACCOUNT_ID: id,
        GITHUB_OUTPUT: "", GITHUB_STEP_SUMMARY: "",
      },
    });
    let output = "";
    child.stdout.on("data", chunk => { output += chunk; });
    child.stderr.on("data", chunk => { output += chunk; });
    child.on("error", reject);
    child.on("close", code => {
      const paths = JSON.parse(output.match(/REQUEST_PATHS=(\[[^\n]*\])/u)?.[1] ?? "[]") as string[];
      resolve({ code, output, paths });
    });
  });
}

describe("Cloudflare deployment credential preflight", () => {
  it("verifies an active user token and checks Workers access", async () => {
    const run = await preflight();
    expect(run.code).toBe(0);
    expect(run.output).toContain("Token verify: ok (user token)");
    expect(run.paths[0]).toBe("/user/tokens/verify");
    expect(run.paths).not.toContain(`/accounts/${accountId}/tokens/verify`);
    expect(run.paths).toContain(`/accounts/${accountId}/workers/scripts`);
    expect(run.output).not.toContain("cfut_synthetic_fixture");
  });

  it("uses the account endpoint directly for prefixed account-owned tokens", async () => {
    const run = await preflight({ token: "cfat_synthetic_fixture", user: rejected });
    expect(run.code).toBe(0);
    expect(run.output).toContain("Token verify: ok (account token)");
    expect(run.paths[0]).toBe(`/accounts/${accountId}/tokens/verify`);
    expect(run.paths).not.toContain("/user/tokens/verify");
    expect(run.output).not.toContain("cfat_synthetic_fixture");
  });

  it("recognizes a legacy account token only after successful account verification", async () => {
    const run = await preflight({ token: "synthetic_legacy_fixture", user: rejected });
    expect(run.code).toBe(0);
    expect(run.paths.slice(0, 2)).toEqual(["/user/tokens/verify", `/accounts/${accountId}/tokens/verify`]);
  });

  it("rejects a token rejected by both endpoints", async () => {
    const run = await preflight({ user: rejected, account: rejected });
    expect(run.code).toBe(1);
    expect(run.paths).toHaveLength(2);
    expect(run.output).toContain("API token verification failed");
  });

  it.each(["disabled", "expired", undefined])("rejects a token that is not active: %s", async status => {
    const run = await preflight({ token: "cfat_synthetic_fixture", account: { success: true, result: { status } } });
    expect(run.code).toBe(1);
    expect(run.paths).toHaveLength(1);
  });

  it("still fails when an active token lacks Workers permissions", async () => {
    const run = await preflight({ token: "cfat_synthetic_fixture", workersAllowed: false });
    expect(run.code).toBe(1);
    expect(run.output).toContain("Token cannot list Workers scripts");
  });

  it("keeps D1-only authorization failures nonblocking", async () => {
    const run = await preflight({ token: "cfat_synthetic_fixture", d1Allowed: false });
    expect(run.code).toBe(0);
    expect(run.output).toContain("D1 API unauthorized");
  });

  it("rejects malformed account IDs before sending the token", async () => {
    const run = await preflight({ accountId: "not-an-account-id" });
    expect(run.code).toBe(1);
    expect(run.paths).toHaveLength(0);
    expect(run.output).toContain("32-character hexadecimal Account ID");
  });
});
