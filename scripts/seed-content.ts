/**
 * Content migration (T8). posts.json (52) + interviews.json (57) ->
 *   - HTML body        -> R2 at content/{kind}/{id}.html
 *   - metadata row     -> content
 *   - title/excerpt/plaintext-body -> content_fts
 *
 *   node scripts/seed-content.ts --local   # or --remote
 *
 * Fixes the known defects: forPersona parsed from a Python-literal string into a
 * real JSON array; single canonical row per item (root copies are the source).
 * Idempotent: content upserts on id; FTS rows are deleted-then-inserted per id.
 */
import { execFileSync } from "node:child_process";
import { mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import { chunk, execSql, lit, log, orNull, parseTarget, query } from "./_util.ts";
import type { Target } from "./_util.ts";

const HERE = dirname(fileURLToPath(import.meta.url));
const DATA = join(HERE, "data");
const BUCKET = "summaverick-content";

interface Post {
  id: number | string;
  uniqueId?: string;
  title: string;
  excerpt?: string;
  category?: string;
  learningPath?: string;
  difficulty?: string;
  forPersona?: string | string[];
  readTime?: string;
  dateISO?: string;
  content: string;
}
interface Interview {
  id: number | string;
  question: string;
  answer: string;
  category?: string;
  company?: string;
  difficulty?: string;
  dateISO?: string;
}

interface ContentItem {
  id: string; // kind-namespaced PK, e.g. "post-97" (post/interview ids overlap)
  numId: string; // original numeric id, used for the R2 path + old-URL mapping
  kind: "post" | "interview";
  slug: string;
  title: string;
  excerpt: string | null;
  category: string | null;
  learning_path: string | null;
  difficulty: string | null;
  personas: string[] | null;
  company: string | null;
  read_time: string | null;
  body_html: string;
  body_text: string;
  published_at: number | null;
}

// --- helpers ---------------------------------------------------------------
function parsePythonList(v: string | string[] | undefined): string[] | null {
  if (v == null) return null;
  if (Array.isArray(v)) return v.map(String);
  const s = String(v).trim();
  if (!s || s === "[]") return [];
  // Extract quoted items — handles the "['Developers', 'Architects']" form.
  const items: string[] = [];
  const re = /'([^']*)'|"([^"]*)"/g;
  let m: RegExpExecArray | null;
  while ((m = re.exec(s)) !== null) items.push(m[1] ?? m[2] ?? "");
  return items;
}

function stripTags(html: string): string {
  return html
    .replace(/<script[\s\S]*?<\/script>/gi, " ")
    .replace(/<style[\s\S]*?<\/style>/gi, " ")
    .replace(/<[^>]+>/g, " ")
    .replace(/&nbsp;/g, " ")
    .replace(/&amp;/g, "&")
    .replace(/&lt;/g, "<")
    .replace(/&gt;/g, ">")
    .replace(/\s+/g, " ")
    .trim();
}

function slugify(s: string): string {
  return s
    .toLowerCase()
    .replace(/[^a-z0-9]+/g, "-")
    .replace(/^-+|-+$/g, "")
    .slice(0, 80)
    .replace(/-+$/g, "");
}

/** Post slug: uniqueId with a trailing -<timestamp> stripped, else slugify. */
function postSlug(p: Post): string {
  if (p.uniqueId) return p.uniqueId.replace(/-\d{10,}$/, "");
  return slugify(p.title);
}

function isoToMs(iso: string | undefined): number | null {
  if (!iso) return null;
  const t = Date.parse(iso);
  return Number.isFinite(t) ? t : null;
}

function r2Put(target: Target, key: string, body: string): void {
  const dir = mkdtempSync(join(tmpdir(), "sv-r2-"));
  const file = join(dir, "body.html");
  try {
    writeFileSync(file, body, "utf8");
    execFileSync(
      "node_modules/.bin/wrangler",
      [
        "r2",
        "object",
        "put",
        `${BUCKET}/${key}`,
        `--file=${file}`,
        "--content-type=text/html; charset=utf-8",
        target.flag,
      ],
      { stdio: ["ignore", "ignore", "inherit"] }
    );
  } finally {
    rmSync(dir, { recursive: true, force: true });
  }
}

// --- main ------------------------------------------------------------------
function main(): void {
  const target = parseTarget(process.argv.slice(2));
  const now = Date.now();
  log(`seed-content -> ${target.flag}`);

  const posts = JSON.parse(
    readFileSync(join(DATA, "posts.json"), "utf8")
  ) as Post[];
  const interviews = JSON.parse(
    readFileSync(join(DATA, "interviews.json"), "utf8")
  ) as Interview[];

  const items: ContentItem[] = [];
  const usedSlugs = new Set<string>();

  const uniqueSlug = (base: string, id: string): string => {
    let slug = base || `item-${id}`;
    if (usedSlugs.has(slug)) slug = `${slug}-${id}`;
    usedSlugs.add(slug);
    return slug;
  };

  for (const p of posts) {
    const numId = String(p.id);
    items.push({
      id: `post-${numId}`,
      numId,
      kind: "post",
      slug: uniqueSlug(postSlug(p), numId),
      title: p.title,
      excerpt: orNull(p.excerpt),
      category: orNull(p.category),
      learning_path: orNull(p.learningPath),
      difficulty: orNull(p.difficulty),
      personas: parsePythonList(p.forPersona),
      company: null,
      read_time: orNull(p.readTime),
      body_html: p.content ?? "",
      body_text: stripTags(p.content ?? ""),
      published_at: isoToMs(p.dateISO),
    });
  }

  for (const iv of interviews) {
    const numId = String(iv.id);
    const bodyText = stripTags(iv.answer ?? "");
    items.push({
      id: `interview-${numId}`,
      numId,
      kind: "interview",
      slug: uniqueSlug(slugify(iv.question), numId),
      title: iv.question,
      excerpt: bodyText.slice(0, 200) || null,
      category: orNull(iv.category),
      learning_path: null,
      difficulty: orNull(iv.difficulty),
      personas: null,
      company: orNull(iv.company),
      read_time: null,
      body_html: iv.answer ?? "",
      body_text: bodyText,
      published_at: isoToMs(iv.dateISO),
    });
  }

  // 1) R2 bodies
  log(`writing ${items.length} bodies to R2...`);
  let n = 0;
  for (const it of items) {
    r2Put(target, `content/${it.kind}/${it.numId}.html`, it.body_html);
    process.stdout.write(`\r  ${++n}/${items.length}`);
  }
  process.stdout.write("\n");

  // 2) content rows (upsert)
  const rowStmts = items.map((it) => {
    const key = `content/${it.kind}/${it.numId}.html`;
    return (
      `INSERT INTO content (id, kind, slug, title, excerpt, category, learning_path, difficulty, personas_json, company, read_time, body_r2_key, status, published_at, updated_at) VALUES (` +
      [
        lit(it.id),
        lit(it.kind),
        lit(it.slug),
        lit(it.title),
        lit(it.excerpt),
        lit(it.category),
        lit(it.learning_path),
        lit(it.difficulty),
        it.personas === null ? "NULL" : lit(JSON.stringify(it.personas)),
        lit(it.company),
        lit(it.read_time),
        lit(key),
        lit("published"),
        it.published_at === null ? "NULL" : String(it.published_at),
        String(now),
      ].join(", ") +
      `) ON CONFLICT(id) DO UPDATE SET kind=excluded.kind, slug=excluded.slug, title=excluded.title, ` +
      `excerpt=excluded.excerpt, category=excluded.category, learning_path=excluded.learning_path, ` +
      `difficulty=excluded.difficulty, personas_json=excluded.personas_json, company=excluded.company, ` +
      `read_time=excluded.read_time, body_r2_key=excluded.body_r2_key, status=excluded.status, ` +
      `published_at=excluded.published_at, updated_at=excluded.updated_at;`
    );
  });
  for (const c of chunk(rowStmts)) execSql(target, c);
  log(`upserted ${rowStmts.length} content rows`);

  // 3) FTS (delete-then-insert per id for idempotency)
  const ftsStmts: string[] = [];
  for (const it of items) {
    ftsStmts.push(`DELETE FROM content_fts WHERE content_id = ${lit(it.id)};`);
    ftsStmts.push(
      `INSERT INTO content_fts (title, excerpt, body, content_id) VALUES (` +
        [
          lit(it.title),
          lit(it.excerpt ?? ""),
          lit(it.body_text),
          lit(it.id),
        ].join(", ") +
        `);`
    );
  }
  for (const c of chunk(ftsStmts)) execSql(target, c);
  log(`rebuilt ${items.length} FTS entries`);

  // 4) verify
  const counts = query(
    target,
    "SELECT kind, COUNT(*) AS n FROM content GROUP BY kind ORDER BY kind"
  );
  for (const r of counts) log(`  ${r.kind}: ${r.n}`);
}

main();
