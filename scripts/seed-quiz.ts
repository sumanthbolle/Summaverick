/**
 * Seed quiz_categories + questions from scripts/data/*.json into D1 (T6).
 *
 *   node scripts/seed-quiz.ts --local      # or --remote
 *
 * Idempotent (ON CONFLICT DO UPDATE). Validates every question and FAILS LOUDLY
 * (non-zero exit, no partial write) if any `correct` is empty or references an
 * out-of-range option index. Absent optional keys become NULL, never ''.
 */
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";
import {
  chunk,
  execSql,
  jsonLit,
  lit,
  log,
  orNull,
  parseTarget,
  query,
} from "./_util.ts";

const HERE = dirname(fileURLToPath(import.meta.url));
const DATA = join(HERE, "data");

interface RawMode {
  id: string;
  name: string;
  desc?: string;
  count: string | number;
  timer?: number;
}
interface RawCategory {
  name: string;
  icon?: string;
  desc?: string;
  featured?: boolean;
  color?: string;
  modes: RawMode[];
}
interface RawQuestion {
  id: string;
  q: string;
  type?: string;
  difficulty?: string;
  options: string[];
  correct: number[];
  explanation?: string;
  multi?: boolean;
  topic?: string;
  source?: string;
  sourceSet?: string;
}

function main(): void {
  const target = parseTarget(process.argv.slice(2));
  log(`seed-quiz -> ${target.flag}`);

  const categories = JSON.parse(
    readFileSync(join(DATA, "quiz-categories.json"), "utf8")
  ) as Record<string, RawCategory>;
  const questions = JSON.parse(
    readFileSync(join(DATA, "quiz-questions.json"), "utf8")
  ) as Record<string, RawQuestion[]>;

  // --- validate BEFORE writing anything ---
  const offenders: string[] = [];
  let total = 0;
  for (const [cat, items] of Object.entries(questions)) {
    for (const it of items) {
      total++;
      if (!Array.isArray(it.correct) || it.correct.length === 0) {
        offenders.push(`${it.id}: empty correct[]`);
        continue;
      }
      const n = Array.isArray(it.options) ? it.options.length : 0;
      for (const idx of it.correct) {
        if (typeof idx !== "number" || idx < 0 || idx >= n) {
          offenders.push(
            `${it.id}: correct index ${idx} out of range (options=${n})`
          );
        }
      }
      if (!questions[cat]) offenders.push(`${it.id}: unknown category ${cat}`);
    }
  }
  if (offenders.length > 0) {
    log(`\nVALIDATION FAILED — ${offenders.length} offending question(s):`);
    for (const o of offenders.slice(0, 50)) log("  - " + o);
    if (offenders.length > 50) log(`  ...and ${offenders.length - 50} more`);
    process.exit(1);
  }
  log(`validated ${total} questions across ${Object.keys(questions).length} categories`);

  // --- categories ---
  const catStmts: string[] = [];
  let sort = 0;
  for (const [id, c] of Object.entries(categories)) {
    catStmts.push(
      `INSERT INTO quiz_categories (id, name, icon, description, featured, color, modes_json, sort_order) VALUES (` +
        [
          lit(id),
          lit(c.name),
          lit(orNull(c.icon)),
          lit(orNull(c.desc)),
          c.featured ? "1" : "0",
          lit(orNull(c.color)),
          jsonLit(c.modes ?? []),
          String(sort++),
        ].join(", ") +
        `) ON CONFLICT(id) DO UPDATE SET name=excluded.name, icon=excluded.icon, ` +
        `description=excluded.description, featured=excluded.featured, color=excluded.color, ` +
        `modes_json=excluded.modes_json, sort_order=excluded.sort_order;`
    );
  }
  for (const c of chunk(catStmts)) execSql(target, c);
  log(`upserted ${catStmts.length} categories`);

  // --- questions ---
  const qStmts: string[] = [];
  for (const [cat, items] of Object.entries(questions)) {
    for (const it of items) {
      qStmts.push(
        `INSERT INTO questions (id, category_id, kind, difficulty, is_multi, topic, stem, options_json, correct_json, explanation, source, source_set, retired_at) VALUES (` +
          [
            lit(it.id),
            lit(cat),
            lit(it.type ?? "mcq"),
            lit(orNull(it.difficulty)),
            it.multi ? "1" : "0",
            lit(orNull(it.topic)),
            lit(it.q),
            jsonLit(it.options),
            jsonLit(it.correct),
            lit(orNull(it.explanation)),
            lit(orNull(it.source)),
            lit(orNull(it.sourceSet)),
            "NULL",
          ].join(", ") +
          `) ON CONFLICT(id) DO UPDATE SET category_id=excluded.category_id, kind=excluded.kind, ` +
          `difficulty=excluded.difficulty, is_multi=excluded.is_multi, topic=excluded.topic, ` +
          `stem=excluded.stem, options_json=excluded.options_json, correct_json=excluded.correct_json, ` +
          `explanation=excluded.explanation, source=excluded.source, source_set=excluded.source_set;`
      );
    }
  }
  const chunks = chunk(qStmts);
  log(`writing ${qStmts.length} questions in ${chunks.length} chunks of <=100...`);
  let done = 0;
  for (const c of chunks) {
    execSql(target, c);
    done += c.length;
    process.stdout.write(`\r  ${done}/${qStmts.length}`);
  }
  process.stdout.write("\n");

  // --- verify counts ---
  const rows = query(
    target,
    "SELECT category_id, COUNT(*) AS n FROM questions GROUP BY category_id ORDER BY category_id"
  );
  log("per-category counts in D1:");
  let sum = 0;
  for (const r of rows) {
    log(`  ${r.category_id}: ${r.n}`);
    sum += Number(r.n);
  }
  log(`TOTAL: ${sum}`);
}

main();
