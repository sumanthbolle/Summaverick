/**
 * Post-deploy smoke checks (T11). Run against a base URL:
 *
 *   node scripts/verify.ts https://summaverick.com
 *   node scripts/verify.ts            # defaults to $VERIFY_BASE or localhost:8787
 *
 * Exits non-zero on the first failed check so CI can fail the deploy.
 */
const BASE =
  process.argv[2] ||
  process.env.VERIFY_BASE ||
  "http://127.0.0.1:8787";

interface Check {
  name: string;
  run: () => Promise<void>;
}

async function getJson(path: string): Promise<{ status: number; body: any }> {
  const res = await fetch(BASE + path, { headers: { accept: "application/json" } });
  let body: unknown = null;
  try {
    body = await res.json();
  } catch {
    /* leave null */
  }
  return { status: res.status, body };
}

function assert(cond: unknown, msg: string): void {
  if (!cond) throw new Error(msg);
}

const checks: Check[] = [
  {
    name: "health ok + db ok",
    run: async () => {
      const { status, body } = await getJson("/api/health");
      assert(status === 200, `health status ${status}`);
      assert(body?.ok === true, "health ok !== true");
      assert(body?.db === "ok", `db not ok: ${body?.db}`);
    },
  },
  {
    name: "quiz categories = 9, total 1318, no answers leaked",
    run: async () => {
      const { status, body } = await getJson("/api/quiz/categories");
      assert(status === 200, `categories status ${status}`);
      const cats = body?.categories ?? [];
      assert(cats.length === 9, `expected 9 categories, got ${cats.length}`);
      const total = cats.reduce((a: number, c: any) => a + (c.count ?? 0), 0);
      assert(total === 1318, `expected 1318 questions, got ${total}`);
    },
  },
  {
    name: "content feed returns posts",
    run: async () => {
      const { status, body } = await getJson("/api/content?kind=post");
      assert(status === 200, `content status ${status}`);
      assert((body?.items ?? []).length > 0, "no post items");
    },
  },
  {
    name: "home page serves HTML",
    run: async () => {
      const res = await fetch(BASE + "/");
      assert(res.status === 200, `home status ${res.status}`);
      const text = await res.text();
      assert(text.includes("<html"), "home not HTML");
      assert(/summaverick/i.test(text), "home missing brand");
    },
  },
  {
    name: "advocate demo metadata",
    run: async () => {
      const { status, body } = await getJson("/api/advocate");
      assert(status === 200, `advocate status ${status}`);
      assert(body?.ok === true, "advocate ok !== true");
      assert(
        Array.isArray(body?.scenarios) && body.scenarios.includes("cooperative"),
        "advocate scenarios missing"
      );
    },
  },
];

async function main(): Promise<void> {
  console.log(`verify against ${BASE}`);
  let failed = 0;
  for (const c of checks) {
    try {
      await c.run();
      console.log(`  ✓ ${c.name}`);
    } catch (e) {
      failed++;
      console.error(`  ✗ ${c.name}: ${(e as Error).message}`);
    }
  }
  if (failed > 0) {
    console.error(`${failed} check(s) failed`);
    process.exit(1);
  }
  console.log("all smoke checks passed");
}

main();
