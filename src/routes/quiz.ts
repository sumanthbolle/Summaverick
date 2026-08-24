/**
 * Quiz API (T7).
 *
 * INVARIANT: correct answers + explanations NEVER leave the Worker until the
 * specific question has been answered (per-response) or the attempt is finished
 * (full review). The client receives only stems + (reshuffled) options up front.
 */
import type { Ctx, RouteDef, RouteMaker } from "../types";
import type { AttemptSlot } from "../domain/quiz";
import {
  createAttempt,
  finishAttempt,
  getAttempt,
  getCategory,
  getQuestionById,
  getQuestionsByIds,
  getResponsesForAttempt,
  listAttempts,
  listCategoriesWithCounts,
  selectCandidateQuestions,
  insertResponse,
} from "../db/queries";
import {
  buildAttemptSet,
  correctDisplayed,
  displayedOptions,
  grade,
  parseIntArray,
  resolveMode,
  selectForMode,
  toDisplayed,
} from "../domain/quiz";
import {
  badRequest,
  forbidden,
  json,
  newId,
  notFound,
  nowMs,
  ok,
  readJson,
} from "../lib/json";

function slots(attemptQuestionIds: string): AttemptSlot[] {
  try {
    return JSON.parse(attemptQuestionIds) as AttemptSlot[];
  } catch {
    return [];
  }
}

function owns(ctx: Ctx, userId: string | null, deviceId: string): boolean {
  if (userId && ctx.session.userId) return userId === ctx.session.userId;
  return deviceId === ctx.session.deviceId;
}

export function quizRoutes(route: RouteMaker): RouteDef[] {
  return [
    // ---- categories with LIVE counts ----
    route("GET", "/api/quiz/categories", async (_req, ctx) => {
      try {
        const rows = await listCategoriesWithCounts(ctx.env.DB);
        return ok({
          categories: rows.map((c) => ({
            id: c.id,
            name: c.name,
            icon: c.icon,
            description: c.description,
            featured: c.featured === 1,
            color: c.color,
            count: c.count,
            modes: safeModes(c.modes_json),
          })),
        });
      } catch (err) {
        console.error("quiz categories", err);
        return json(
          {
            ok: false,
            error: "unavailable",
            message: "Quiz catalog is still being set up.",
            categories: [],
          },
          { status: 503 }
        );
      }
    }),

    // ---- start an attempt ----
    route("POST", "/api/quiz/attempt", async (req, ctx) => {
      const body = await readJson<{ categoryId?: string; mode?: string }>(req);
      if (!body?.categoryId || !body?.mode) {
        return badRequest("categoryId and mode are required");
      }
      const category = await getCategory(ctx.env.DB, body.categoryId);
      if (!category) return notFound("unknown category");
      const mode = resolveMode(category.modes_json, body.mode);
      if (!mode) return badRequest("unknown mode for category");

      const candidates = await selectCandidateQuestions(
        ctx.env.DB,
        category.id,
        {}
      );
      const chosen = selectForMode(candidates, mode);
      if (chosen.length === 0) {
        return badRequest("no questions match this mode");
      }
      const set = buildAttemptSet(chosen);

      const attemptId = newId("att");
      const now = nowMs();
      await createAttempt(ctx.env.DB, {
        id: attemptId,
        user_id: ctx.session.userId,
        device_id: ctx.session.deviceId,
        category_id: category.id,
        mode: mode.id,
        question_ids: JSON.stringify(set),
        started_at: now,
        finished_at: null,
        score: null,
      });

      const byId = new Map(chosen.map((q) => [q.id, q]));
      return ok({
        attemptId,
        mode: { id: mode.id, name: mode.name, timer: mode.timer ?? 0 },
        total: set.length,
        questions: set.map((s) => {
          const q = byId.get(s.id)!;
          return {
            id: q.id,
            stem: q.stem,
            isMulti: q.is_multi === 1,
            difficulty: q.difficulty,
            topic: q.topic,
            options: displayedOptions(q.options_json, s.order),
            // NO correct answer, NO explanation.
          };
        }),
      });
    }),

    // ---- resume ----
    route("GET", "/api/quiz/attempt/:id", async (_req, ctx) => {
      const attempt = await getAttempt(ctx.env.DB, ctx.params.id!);
      if (!attempt) return notFound("no such attempt");
      if (!owns(ctx, attempt.user_id, attempt.device_id)) {
        return forbidden("not your attempt");
      }
      const set = slots(attempt.question_ids);
      const rows = await getQuestionsByIds(
        ctx.env.DB,
        set.map((s) => s.id)
      );
      const byId = new Map(rows.map((q) => [q.id, q]));
      const responses = await getResponsesForAttempt(ctx.env.DB, attempt.id);
      const answered = new Set(responses.map((r) => r.question_id));

      return ok({
        attemptId: attempt.id,
        categoryId: attempt.category_id,
        mode: attempt.mode,
        finished: attempt.finished_at != null,
        total: set.length,
        answeredCount: answered.size,
        // Unanswered questions carry NO answer data.
        questions: set.map((s) => {
          const q = byId.get(s.id)!;
          return {
            id: q.id,
            stem: q.stem,
            isMulti: q.is_multi === 1,
            difficulty: q.difficulty,
            topic: q.topic,
            options: displayedOptions(q.options_json, s.order),
            answered: answered.has(q.id),
          };
        }),
        // Prior responses may reveal answers — those questions are already done.
        responses: responses.map((r) => {
          const s = set.find((x) => x.id === r.question_id)!;
          const q = byId.get(r.question_id)!;
          return {
            questionId: r.question_id,
            correct: r.correct === 1,
            chosen: toDisplayed(s.order, parseIntArray(r.chosen_json)),
            correctOptions: correctDisplayed(q.correct_json, s.order),
            explanation: q.explanation,
          };
        }),
      });
    }),

    // ---- answer one question (server-side grading) ----
    route("POST", "/api/quiz/attempt/:id/response", async (req, ctx) => {
      const attempt = await getAttempt(ctx.env.DB, ctx.params.id!);
      if (!attempt) return notFound("no such attempt");
      if (!owns(ctx, attempt.user_id, attempt.device_id)) {
        return forbidden("not your attempt");
      }
      if (attempt.finished_at != null) return badRequest("attempt is finished");

      const body = await readJson<{
        questionId?: string;
        chosen?: number[];
        msTaken?: number;
      }>(req);
      if (!body?.questionId || !Array.isArray(body.chosen)) {
        return badRequest("questionId and chosen[] are required");
      }
      const set = slots(attempt.question_ids);
      const slot = set.find((s) => s.id === body.questionId);
      if (!slot) return badRequest("question not part of this attempt");

      const q = await getQuestionById(ctx.env.DB, body.questionId);
      if (!q) return notFound("question missing");

      const { correct, chosenOriginal } = grade(
        q.correct_json,
        slot.order,
        body.chosen
      );
      await insertResponse(ctx.env.DB, {
        id: newId("res"),
        attempt_id: attempt.id,
        question_id: q.id,
        chosen_json: JSON.stringify(chosenOriginal),
        correct: correct ? 1 : 0,
        ms_taken: typeof body.msTaken === "number" ? body.msTaken : null,
        answered_at: nowMs(),
      });

      // Now that the question is answered, revealing the answer is allowed.
      return ok({
        correct,
        correctOptions: correctDisplayed(q.correct_json, slot.order),
        explanation: q.explanation,
      });
    }),

    // ---- finish -> store score, return full review ----
    route("POST", "/api/quiz/attempt/:id/finish", async (_req, ctx) => {
      const attempt = await getAttempt(ctx.env.DB, ctx.params.id!);
      if (!attempt) return notFound("no such attempt");
      if (!owns(ctx, attempt.user_id, attempt.device_id)) {
        return forbidden("not your attempt");
      }
      const set = slots(attempt.question_ids);
      const rows = await getQuestionsByIds(
        ctx.env.DB,
        set.map((s) => s.id)
      );
      const byId = new Map(rows.map((q) => [q.id, q]));
      const responses = await getResponsesForAttempt(ctx.env.DB, attempt.id);
      const respById = new Map(responses.map((r) => [r.question_id, r]));

      const correctCount = responses.filter((r) => r.correct === 1).length;
      const score = set.length > 0 ? correctCount / set.length : 0;
      const now = nowMs();
      if (attempt.finished_at == null) {
        await finishAttempt(ctx.env.DB, attempt.id, now, score);
      }

      return ok({
        attemptId: attempt.id,
        score,
        correctCount,
        total: set.length,
        review: set.map((s) => {
          const q = byId.get(s.id)!;
          const r = respById.get(s.id);
          return {
            id: q.id,
            stem: q.stem,
            options: displayedOptions(q.options_json, s.order),
            isMulti: q.is_multi === 1,
            correctOptions: correctDisplayed(q.correct_json, s.order),
            explanation: q.explanation,
            chosen: r ? toDisplayed(s.order, parseIntArray(r.chosen_json)) : [],
            correct: r ? r.correct === 1 : false,
            answered: !!r,
          };
        }),
      });
    }),

    // ---- history for the current user/device ----
    route("GET", "/api/quiz/history", async (_req, ctx) => {
      const attempts = await listAttempts(ctx.env.DB, {
        userId: ctx.session.userId,
        deviceId: ctx.session.deviceId,
      });
      return json({
        ok: true,
        attempts: attempts.map((a) => ({
          id: a.id,
          categoryId: a.category_id,
          mode: a.mode,
          startedAt: a.started_at,
          finishedAt: a.finished_at,
          score: a.score,
          total: slots(a.question_ids).length,
        })),
      });
    }),
  ];
}

function safeModes(jsonStr: string): unknown[] {
  try {
    const v = JSON.parse(jsonStr);
    return Array.isArray(v) ? v : [];
  } catch {
    return [];
  }
}
