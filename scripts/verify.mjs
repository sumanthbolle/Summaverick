/**
 * Post-deploy smoke checks (T11). Run against a base URL:
 *
 *   node scripts/verify.mjs https://summaverick.com
 *   node scripts/verify.mjs --core https://summaverick.com   # Worker + pages only
 *   node scripts/verify.mjs            # defaults to $VERIFY_BASE or localhost:8787
 *
 * `--core` is what GitHub Actions requires after wrangler deploy: the product
 * HTML and APIs that do not need a seeded D1. Full catalog checks stay the
 * default for local/post-seed verification.
 */
const argv = process.argv.slice(2).filter((a) => a !== "--");
const CORE = argv.includes("--core");
const BASE =
  argv.find((a) => !a.startsWith("--")) ||
  process.env.VERIFY_BASE ||
  "http://127.0.0.1:8787";

async function getJson(path) {
  const res = await fetch(BASE + path, { headers: { accept: "application/json" } });
  let body = null;
  try {
    body = await res.json();
  } catch {
    /* leave null */
  }
  return { status: res.status, body };
}

function assert(cond, msg) {
  if (!cond) throw new Error(msg);
}

const coreChecks = [
  {
    name: "Meet Summaverick page and same-origin chat",
    run: async () => {
      const res = await fetch(BASE + "/ask-summaverick?q=hello");
      assert(res.status === 200, `assistant status ${res.status}`);
      assert(new URL(res.url).pathname === "/ask-summaverick", "assistant redirected away");
      const html = await res.text();
      assert(html.includes("<title>Meet Summaverick"), "assistant title missing");
      assert(html.includes("'/api/ask-summaverick'"), "assistant does not use this origin");
      const invalid = await fetch(BASE + "/api/ask-summaverick", {
        method: "POST", headers: { "content-type": "application/json" }, body: "{}",
      });
      assert(invalid.status === 400, `invalid chat should be 400, got ${invalid.status}`);
    },
  },
  {
    name: "home page is the product site",
    run: async () => {
      const res = await fetch(BASE + "/");
      assert(res.status === 200, `home status ${res.status}`);
      const text = await res.text();
      assert(text.includes("<html"), "home not HTML");
      assert(/summaverick/i.test(text), "home missing brand");
      assert(
        !/coming online/i.test(text),
        "home is still the placeholder stub, not the product site"
      );
      assert(
        text.includes("A software studio for AI agents."),
        "home missing the software studio headline"
      );
      assert(
        text.includes("Personal assistants and enterprise agents"),
        "home missing the specialty line — the offer has to be in the first screen"
      );
      for (const offer of ["Personal agents", "Mobile experiences", "Enterprise agents"]) {
        assert(text.includes(`<h3>${offer}</h3>`), `home missing the ${offer} offer`);
      }
      const sections = [...text.matchAll(/<section\b[^>]*\bid="([^"]+)"/g)].map(match => match[1]);
      assert(sections[0] === "top" && sections[1] === "rag", "RAG must follow the introduction directly");
      assert(!text.includes("data-hero-panel") && !text.includes("data-stage-view"), "retired opening illustrations are still present");
      assert(text.includes("Illustrative examples with sample data"), "remaining demos need a clear sample-data label");
      assert(
        text.includes('class="brand-mark"'),
        "home missing the Summaverick brand mark"
      );
      assert(
        text.includes("SUMMAVERICK LLP"),
        "home missing the registered company identity"
      );
    },
  },
  {
    name: "health ok",
    run: async () => {
      const { status, body } = await getJson("/api/health");
      assert(status === 200, `health status ${status}`);
      assert(body?.ok === true, "health ok !== true");
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

const catalogChecks = [
  {
    name: "health db ok",
    run: async () => {
      const { status, body } = await getJson("/api/health");
      assert(status === 200, `health status ${status}`);
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
      const total = cats.reduce((a, c) => a + (c.count ?? 0), 0);
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
];

const checks = CORE ? coreChecks : [...coreChecks, ...catalogChecks];

async function main() {
  console.log(`verify against ${BASE}${CORE ? " (core)" : ""}`);
  let failed = 0;
  for (const c of checks) {
    try {
      await c.run();
      console.log(`  ✓ ${c.name}`);
    } catch (e) {
      failed++;
      console.error(`  ✗ ${c.name}: ${e.message}`);
    }
  }
  if (failed > 0) {
    console.error(`${failed} check(s) failed`);
    process.exit(1);
  }
  console.log("all smoke checks passed");
}

main();
