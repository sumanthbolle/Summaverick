/**
 * Preflight for wrangler in GitHub Actions. Distinguishes "token missing"
 * from Cloudflare 7403 (token cannot use D1 / wrong account id / Zone ID
 * pasted as Account ID), and checks that the token may manage the Worker
 * named in wrangler.jsonc — listing scripts can succeed without that.
 *
 *   node scripts/check-cloudflare-auth.mjs [--require-worker]
 *
 * Reads CLOUDFLARE_API_TOKEN and CLOUDFLARE_ACCOUNT_ID. Never prints the token.
 * Writes `d1_ok` and `worker_access` to GITHUB_OUTPUT. Exits 1 when the token
 * or account id is unusable; missing Worker access is fatal only with
 * --require-worker (a real `wrangler deploy`). D1-only failures are warnings.
 */
import { readFileSync, appendFileSync } from "node:fs";

const CF = "https://api.cloudflare.com/client/v4";
const REQUIRE_WORKER = process.argv.includes("--require-worker");
/** Cloudflare's answer for a Worker name that does not exist in the account. */
const WORKER_NOT_FOUND = 10090;

function gh(kind, message) {
  console.log(`::${kind}::${message.replace(/\n/g, "%0A")}`);
}

function summary(lines) {
  const path = process.env.GITHUB_STEP_SUMMARY;
  if (!path) return;
  appendFileSync(path, lines.join("\n") + "\n");
}

function output(kv) {
  const path = process.env.GITHUB_OUTPUT;
  if (!path) return;
  appendFileSync(
    path,
    Object.entries(kv)
      .map(([k, v]) => `${k}=${v}`)
      .join("\n") + "\n"
  );
}

function wranglerIds() {
  const raw = readFileSync("wrangler.jsonc", "utf8");
  return {
    workerName: raw.match(/"name"\s*:\s*"([^"]+)"/)?.[1] ?? null,
    databaseId: raw.match(/"database_id"\s*:\s*"([^"]+)"/)?.[1] ?? null,
  };
}

async function cf(token, path) {
  const res = await fetch(CF + path, {
    headers: {
      authorization: `Bearer ${token}`,
      "content-type": "application/json",
    },
  });
  let body = { success: false };
  try {
    body = await res.json();
  } catch {
    /* ignore */
  }
  return { status: res.status, body };
}

function errText(body) {
  const e = body?.errors?.[0];
  if (!e) return "(no error body)";
  return `${e.message ?? "unknown"} [code: ${e.code ?? "?"}]`;
}

/** Account-owned tokens cannot be verified through /user/tokens/verify.
 * New tokens identify their type with cfat_/cfut_; legacy tokens do not.
 * Wrangler likewise treats code 1000 on the user endpoint as a possible
 * account token. Verify against the configured account before accepting it.
 */
async function verifyToken(token, accountId) {
  const accountPath = `/accounts/${accountId}/tokens/verify`;
  if (token.startsWith("cfat_")) {
    return { ...(await cf(token, accountPath)), type: "account" };
  }
  const user = await cf(token, "/user/tokens/verify");
  if (user.body.success || !user.body.errors?.some(error => error.code === 1000)) {
    return { ...user, type: "user" };
  }
  return { ...(await cf(token, accountPath)), type: "account" };
}

async function main() {
  const token = (process.env.CLOUDFLARE_API_TOKEN ?? "").trim();
  const accountId = (process.env.CLOUDFLARE_ACCOUNT_ID ?? "").trim();
  const { workerName, databaseId } = wranglerIds();

  const lines = ["## Cloudflare deploy preflight", ""];
  let fatal = 0;
  let d1Ok = false;
  let workerAccess = false;
  const workerProblems = [];

  if (!token) {
    gh(
      "error",
      "CLOUDFLARE_API_TOKEN is not set. GitHub → Settings → Secrets and variables → Actions → New repository secret. Create a token at https://dash.cloudflare.com/profile/api-tokens with Edit Cloudflare Workers PLUS Account → D1 → Edit."
    );
    lines.push("- Token: **missing**");
    summary(lines);
    output({ d1_ok: "false", worker_access: "false" });
    process.exit(1);
  }
  console.log(`CLOUDFLARE_API_TOKEN is present (${token.length} chars)`);
  lines.push(`- Token: present (${token.length} chars)`);

  if (!accountId) {
    gh(
      "error",
      "CLOUDFLARE_ACCOUNT_ID is not set. Copy it from the Cloudflare dashboard Workers overview (right sidebar). Do not use the Zone ID from the domain overview — both are 32 hex characters."
    );
    lines.push("- Account ID: **missing**");
    summary(lines);
    output({ d1_ok: "false", worker_access: "false" });
    process.exit(1);
  }
  console.log(`CLOUDFLARE_ACCOUNT_ID is present (${accountId.length} chars)`);
  lines.push(`- Account ID: \`${accountId}\``);

  if (!/^[a-f0-9]{32}$/i.test(accountId)) {
    gh("error", "CLOUDFLARE_ACCOUNT_ID must be the 32-character hexadecimal Account ID from the Workers overview.");
    lines.push("- Account ID: **invalid format**");
    summary(lines);
    output({ d1_ok: "false", worker_access: "false" });
    process.exit(1);
  }

  const verify = await verifyToken(token, accountId);
  if (verify.status < 200 || verify.status >= 300 || !verify.body.success || verify.body.result?.status !== "active") {
    const reason = verify.body.success
      ? `Token status is ${verify.body.result?.status ?? "unknown"} (HTTP ${verify.status})`
      : errText(verify.body);
    gh(
      "error",
      `API token verification failed (${reason}). Check the token's validity and account scope. Use a User API Token from My Profile → API Tokens or an Account API Token from Manage Account → Account API Tokens, not the Global API Key. Paste only the token value into the production CLOUDFLARE_API_TOKEN secret.`
    );
    lines.push(`- Token verify (${verify.type}): **failed** — ${reason}`);
    summary(lines);
    output({ d1_ok: "false", worker_access: "false" });
    process.exit(1);
  }
  console.log(`Token verify: ok (${verify.type} token)`);
  lines.push(`- Token verify: ok (${verify.type} token)`);

  const zone = await cf(token, `/zones/${accountId}`);
  if (zone.body.success && zone.body.result) {
    const zoneName = zone.body.result.name ?? "unknown zone";
    const realAccount = zone.body.result.account?.id ?? "(unknown)";
    gh(
      "error",
      `CLOUDFLARE_ACCOUNT_ID is a Zone ID for ${zoneName}, not an Account ID. Set the variable to the Account ID from Workers overview (right sidebar). That account id is ${realAccount}.`
    );
    lines.push(
      `- Account ID: **this is the Zone ID for ${zoneName}**. Real account id: \`${realAccount}\`.`
    );
    summary(lines);
    output({ d1_ok: "false", worker_access: "false" });
    process.exit(1);
  }

  const accounts = await cf(token, "/accounts?per_page=50");
  const list = Array.isArray(accounts.body.result) ? accounts.body.result : [];
  if (accounts.body.success && list.length > 0) {
    const names = list.map((a) => `${a.name} (${a.id})`).join(", ");
    console.log(`Token can see ${list.length} account(s): ${names}`);
    lines.push(`- Accounts visible to token: ${names}`);
    if (!list.some((a) => a.id === accountId)) {
      gh(
        "error",
        `CLOUDFLARE_ACCOUNT_ID ${accountId} is not one of the accounts this token can access (${names}). Copy the Account ID from the Workers overview of the account that owns summaverick.com.`
      );
      lines.push("- Account match: **no**");
      fatal++;
    }
  } else {
    console.log(
      `Could not list accounts (${errText(accounts.body)}). Continuing with the configured id.`
    );
    lines.push(`- Accounts list: skipped (${errText(accounts.body)})`);
  }

  const acct = await cf(token, `/accounts/${accountId}`);
  if (!acct.body.success) {
    gh(
      "warning",
      `GET /accounts/${accountId} failed: ${errText(acct.body)}. If this is a Zone ID, replace it with the Account ID from the Workers overview.`
    );
    lines.push(`- Account lookup: failed — ${errText(acct.body)}`);
  } else {
    console.log(`Account lookup: ${acct.body.result?.name ?? accountId}`);
    lines.push(`- Account lookup: ${acct.body.result?.name ?? "ok"}`);
  }

  const workers = await cf(token, `/accounts/${accountId}/workers/scripts`);
  if (workers.body.success) {
    console.log("Workers Scripts: listable");
    lines.push("- Workers Scripts: listable");
  } else {
    workerProblems.push(`Token cannot list Workers scripts (${errText(workers.body)}).`);
    lines.push(`- Workers Scripts: **unauthorized** — ${errText(workers.body)}`);
  }

  if (workerName && workers.body.success) {
    const service = await cf(token, `/accounts/${accountId}/workers/services/${encodeURIComponent(workerName)}`);
    if (service.body.success) {
      workerAccess = true;
      console.log(`Worker ${workerName}: token can manage it`);
      lines.push(`- Worker \`${workerName}\`: token can manage it`);
    } else if (service.body.errors?.some((e) => e.code === WORKER_NOT_FOUND)) {
      workerAccess = true;
      console.log(`Worker ${workerName}: not in this account yet; wrangler deploy will create it`);
      lines.push(`- Worker \`${workerName}\`: not created yet`);
    } else {
      workerProblems.push(`Token cannot manage the ${workerName} Worker (${errText(service.body)}).`);
      lines.push(`- Worker \`${workerName}\`: **no access** — ${errText(service.body)}`);
    }
  }

  if (workerProblems.length > 0) {
    const fix =
      "Edit the token (Manage Account → Account API Tokens, or My Profile → API Tokens) and add Account → Workers Scripts → Edit, plus Zone → Workers Routes → Edit for summaverick.com (custom domains cannot use per-Worker roles).";
    const consequence = REQUIRE_WORKER
      ? "wrangler deploy cannot succeed."
      : "Skipping steps that write to the Worker; Cloudflare Workers Builds still deploys it.";
    gh(REQUIRE_WORKER ? "error" : "warning", `${workerProblems.join(" ")} ${fix} ${consequence}`);
    if (REQUIRE_WORKER) fatal++;
  }

  if (databaseId) {
    lines.push(`- D1 database_id in wrangler.jsonc: \`${databaseId}\``);
    const d1List = await cf(token, `/accounts/${accountId}/d1/database`);
    if (!d1List.body.success) {
      gh(
        "warning",
        `D1 API unauthorized (${errText(d1List.body)}). The "Edit Cloudflare Workers" token template does not include D1. Edit the token at https://dash.cloudflare.com/profile/api-tokens → Account → D1 → Edit. Remote migrations are skipped; the library stays empty until D1 works.`
      );
      lines.push(
        `- D1: **unauthorized** (${errText(d1List.body)}). Add Account → D1 → Edit to the API token.`
      );
    } else {
      const dbs = Array.isArray(d1List.body.result) ? d1List.body.result : [];
      const names = dbs
        .map((d) => `${d.name ?? "?"} (${d.uuid ?? "?"})`)
        .join(", ");
      console.log(`D1 databases: ${names || "(none)"}`);
      lines.push(`- D1 databases: ${names || "(none)"}`);
      const found = dbs.some((d) => d.uuid === databaseId);
      if (!found) {
        gh(
          "warning",
          `wrangler.jsonc database_id ${databaseId} is not in this account. Create one with \`pnpm exec wrangler d1 create summaverick --location apac\` and put the UUID in wrangler.jsonc.`
        );
        lines.push("- D1 match: **database_id not in this account**");
      } else {
        d1Ok = true;
        console.log("D1: authorized and database_id matches");
        lines.push("- D1: authorized, database_id matches");
      }
    }
  } else {
    gh("warning", "wrangler.jsonc has no database_id; skipping D1 probe.");
    lines.push("- D1: no database_id in wrangler.jsonc");
  }

  output({ d1_ok: d1Ok ? "true" : "false", worker_access: workerAccess ? "true" : "false" });
  summary(lines);

  if (fatal > 0) {
    process.exit(1);
  }
  if (!d1Ok) {
    console.log(
      "D1 is not usable with this token/account. Remote migrations will be skipped."
    );
  }
}

main().catch((err) => {
  console.error(err);
  process.exit(1);
});
