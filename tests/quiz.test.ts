import { describe, expect, it } from "vitest";
import type { QuestionRow } from "../src/db/schema";
import {
  buildAttemptSet,
  correctDisplayed,
  displayedOptions,
  grade,
  resolveMode,
  selectForMode,
  shuffle,
  toDisplayed,
} from "../src/domain/quiz";

function q(
  id: string,
  opts: string[],
  correct: number[],
  extra: Partial<QuestionRow> = {}
): QuestionRow {
  return {
    id,
    category_id: "csa",
    kind: "mcq",
    difficulty: "easy",
    is_multi: correct.length > 1 ? 1 : 0,
    topic: null,
    stem: "stem " + id,
    options_json: JSON.stringify(opts),
    correct_json: JSON.stringify(correct),
    explanation: "because",
    source: null,
    source_set: null,
    retired_at: null,
    ...extra,
  };
}

const MODES = JSON.stringify([
  { id: "practice", count: "all" },
  { id: "quick", count: 3 },
  { id: "hard", count: "hard" },
  { id: "code", count: "code" },
]);

describe("resolveMode", () => {
  it("finds a mode by id", () => {
    expect(resolveMode(MODES, "quick")?.count).toBe(3);
  });
  it("returns null for unknown mode or bad json", () => {
    expect(resolveMode(MODES, "nope")).toBeNull();
    expect(resolveMode("{bad", "quick")).toBeNull();
  });
});

describe("selectForMode", () => {
  const rows = [
    q("a", ["1", "2"], [0], { difficulty: "easy" }),
    q("b", ["1", "2"], [0], { difficulty: "hard" }),
    q("c", ["1", "2"], [0], { difficulty: "hard" }),
    q("d", ["1", "2"], [0], { difficulty: "medium", kind: "scenario" }),
  ];
  it("'all' returns everything", () => {
    expect(selectForMode(rows, { id: "m", count: "all" })).toHaveLength(4);
  });
  it("integer caps the count", () => {
    expect(selectForMode(rows, { id: "m", count: 2 })).toHaveLength(2);
  });
  it("'hard' filters to hard difficulty", () => {
    const r = selectForMode(rows, { id: "m", count: "hard" });
    expect(r.every((x) => x.difficulty === "hard")).toBe(true);
    expect(r).toHaveLength(2);
  });
  it("'code' filters to non-mcq kinds", () => {
    const r = selectForMode(rows, { id: "m", count: "code" });
    expect(r).toHaveLength(1);
    expect(r[0]!.kind).toBe("scenario");
  });
});

describe("shuffle", () => {
  it("preserves the multiset", () => {
    const src = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9];
    const out = shuffle(src);
    expect(out.slice().sort((a, b) => a - b)).toEqual(src);
    expect(out).toHaveLength(src.length);
  });
});

describe("option permutation + grading round-trip", () => {
  it("grades a single-select question through a permutation", () => {
    const question = q("x", ["A", "B", "C", "D"], [2]); // correct original = C
    const set = buildAttemptSet([question]);
    const order = set[0]!.order; // display -> original
    // The displayed index that maps to original 2 is the correct display index.
    const correctD = correctDisplayed(question.correct_json, order);
    expect(correctD).toHaveLength(1);

    // Choosing the correct displayed index grades correct.
    const good = grade(question.correct_json, order, correctD);
    expect(good.correct).toBe(true);
    expect(good.chosenOriginal).toEqual([2]);

    // Choosing a wrong displayed index grades incorrect.
    const wrongD = [0, 1, 2, 3].filter((d) => !correctD.includes(d))[0]!;
    expect(grade(question.correct_json, order, [wrongD]).correct).toBe(false);
  });

  it("multi-select requires an exact set match", () => {
    const question = q("y", ["A", "B", "C", "D"], [0, 2]);
    const set = buildAttemptSet([question]);
    const order = set[0]!.order;
    const correctD = correctDisplayed(question.correct_json, order);
    expect(grade(question.correct_json, order, correctD).correct).toBe(true);
    // subset is wrong
    expect(grade(question.correct_json, order, [correctD[0]!]).correct).toBe(false);
  });

  it("displayedOptions reorders per the permutation", () => {
    const opts = ["A", "B", "C"];
    const order = [2, 0, 1];
    expect(displayedOptions(JSON.stringify(opts), order)).toEqual(["C", "A", "B"]);
  });

  it("toDisplayed maps original indices back to display positions", () => {
    const order = [2, 0, 1]; // display 0->orig2, 1->orig0, 2->orig1
    expect(toDisplayed(order, [0])).toEqual([1]);
    expect(toDisplayed(order, [2, 1])).toEqual([0, 2]);
  });
});
