/**
 * Pure quiz logic — no IO. Mode resolution, server-side selection + shuffling,
 * option-permutation bookkeeping, and grading. Route handlers do the DB work
 * and call these functions.
 *
 * The attempt's question set is fixed at start and stored as
 *   [{ id, order }]   where order[displayIndex] = originalOptionIndex
 * so both the question sequence AND each question's option ordering survive a
 * resume, and correct answers can be mapped back for grading.
 */
import type { QuestionRow } from "../db/schema";

export interface QuizMode {
  id: string;
  name?: string;
  desc?: string;
  count: string | number; // "all" | "code" | "hard" | integer
  timer?: number;
}

export interface AttemptSlot {
  id: string;
  order: number[];
}

/** Crypto-backed Fisher-Yates. Returns a shuffled copy. */
export function shuffle<T>(input: readonly T[]): T[] {
  const a = input.slice();
  for (let i = a.length - 1; i > 0; i--) {
    const j = randInt(i + 1);
    const tmp = a[i]!;
    a[i] = a[j]!;
    a[j] = tmp;
  }
  return a;
}

function randInt(maxExclusive: number): number {
  if (maxExclusive <= 1) return 0;
  const buf = new Uint32Array(1);
  crypto.getRandomValues(buf);
  return buf[0]! % maxExclusive;
}

export function resolveMode(
  modesJson: string,
  modeId: string
): QuizMode | null {
  let modes: QuizMode[];
  try {
    modes = JSON.parse(modesJson) as QuizMode[];
  } catch {
    return null;
  }
  return modes.find((m) => m.id === modeId) ?? null;
}

/**
 * Apply a mode's selection rule to the candidate rows, then shuffle question
 * order. Returns the chosen rows (not yet capped for integer modes — capping is
 * folded in here).
 */
export function selectForMode(
  rows: QuestionRow[],
  mode: QuizMode
): QuestionRow[] {
  const count = mode.count;
  let pool = rows;

  if (count === "hard") {
    pool = rows.filter((r) => r.difficulty === "hard");
  } else if (count === "code") {
    // "code" = scripting/scenario questions, i.e. anything not a plain mcq.
    pool = rows.filter((r) => r.kind !== "mcq");
  }

  const shuffled = shuffle(pool);

  if (typeof count === "number" && Number.isFinite(count)) {
    return shuffled.slice(0, Math.max(0, Math.floor(count)));
  }
  // "all" | "hard" | "code" -> whole (filtered) pool, shuffled.
  return shuffled;
}

/** Build the persisted attempt set (question order + option permutations). */
export function buildAttemptSet(chosen: QuestionRow[]): AttemptSlot[] {
  return chosen.map((row) => {
    const options = safeArray(row.options_json);
    const order = shuffle(options.map((_, i) => i));
    return { id: row.id, order };
  });
}

/** Options as displayed to the client for a given permutation. */
export function displayedOptions(
  optionsJson: string,
  order: number[]
): string[] {
  const options = safeArray(optionsJson);
  return order.map((orig) => options[orig] ?? "");
}

/**
 * Grade a response. `chosen` are DISPLAYED indices from the client; we map them
 * back to original indices via the permutation, then compare against the
 * question's correct set (original indices). Multi-select requires an exact
 * set match.
 */
export function grade(
  correctJson: string,
  order: number[],
  chosenDisplayed: number[]
): { correct: boolean; chosenOriginal: number[] } {
  const chosenOriginal = chosenDisplayed
    .map((d) => order[d])
    .filter((v): v is number => typeof v === "number");
  const correct = new Set(safeIntArray(correctJson));
  const picked = new Set(chosenOriginal);
  const same =
    correct.size === picked.size &&
    [...correct].every((c) => picked.has(c));
  return { correct: same, chosenOriginal };
}

/** Correct answers expressed as DISPLAYED indices (for the review screen). */
export function correctDisplayed(correctJson: string, order: number[]): number[] {
  const correct = new Set(safeIntArray(correctJson));
  const out: number[] = [];
  for (let d = 0; d < order.length; d++) {
    if (correct.has(order[d]!)) out.push(d);
  }
  return out;
}

/** Map a list of ORIGINAL option indices to their DISPLAYED positions. */
export function toDisplayed(order: number[], originals: number[]): number[] {
  const set = new Set(originals);
  const out: number[] = [];
  for (let d = 0; d < order.length; d++) {
    if (set.has(order[d]!)) out.push(d);
  }
  return out;
}

export function parseIntArray(json: string): number[] {
  return safeIntArray(json);
}

function safeArray(json: string): string[] {
  try {
    const v = JSON.parse(json);
    return Array.isArray(v) ? (v as string[]) : [];
  } catch {
    return [];
  }
}

function safeIntArray(json: string): number[] {
  try {
    const v = JSON.parse(json);
    return Array.isArray(v) ? (v as number[]) : [];
  } catch {
    return [];
  }
}
