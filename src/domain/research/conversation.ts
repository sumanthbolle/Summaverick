/**
 * Multi-turn support for the research chat. The browser sends the earlier
 * turns with each question; nothing about a conversation is stored server-side
 * beyond the individual runs.
 *
 * History comes from the client, so it is treated like any other untrusted
 * input: shape-checked, trimmed to a few recent pairs, length-capped, and any
 * pair whose question trips the injection scan is dropped.
 *
 * An assistant turn is more than input. The model reads it as its own earlier
 * words, so text the client wrote and labelled "assistant" would carry the
 * model's own authority. Each answer therefore leaves the server with a
 * signature (`signAnswer`), the client echoes it back, and `authenticateHistory`
 * replays only the assistant text whose signature checks out. Anything else
 * becomes a placeholder: the follow-up is still answered, without that text.
 */
import { hmacSha256Hex, timingSafeEqual } from "../../lib/crypto";
import {
  classifyServiceNowIntent,
  isServiceNowDomainQuery,
} from "./retrieval/query-classifier";
import { scanForPromptInjection } from "./security/prompt-injection";

export interface ChatTurn {
  role: "user" | "assistant";
  content: string;
  /** Signature the server issued with this answer, echoed back by the client. */
  sig?: string;
}

/** Stands in for an assistant turn the server cannot vouch for. */
export const UNVERIFIED_ANSWER = "(The earlier answer is not available.)";

const SIGNATURE_CONTEXT = "summaverick/research-answer/v1";
const MAX_SIGNATURE_CHARS = 128;

/** Most recent question/answer pairs kept as context. */
export const MAX_HISTORY_PAIRS = 3;
const MAX_USER_CHARS = 500;
const MAX_ASSISTANT_CHARS = 3000;
const MAX_RETRIEVAL_QUERY = 500;

/**
 * Returns complete user → assistant pairs, oldest first, at most
 * MAX_HISTORY_PAIRS of them. Anything malformed is skipped, not rejected, so a
 * bad history degrades to a fresh question instead of an error.
 */
export function normalizeHistory(raw: unknown): ChatTurn[] {
  if (!Array.isArray(raw)) return [];
  const turns: ChatTurn[] = [];
  for (const item of raw.slice(-MAX_HISTORY_PAIRS * 4)) {
    if (!item || typeof item !== "object") continue;
    const role = (item as { role?: unknown }).role;
    const content = (item as { content?: unknown }).content;
    if ((role !== "user" && role !== "assistant") || typeof content !== "string") continue;
    const text = content.trim();
    if (!text) continue;
    const sig = (item as { sig?: unknown }).sig;
    turns.push({
      role,
      // An assistant turn keeps exactly the text its signature was made over.
      content: role === "user" ? text.slice(0, MAX_USER_CHARS) : canonicalAnswer(text),
      ...(role === "assistant" && typeof sig === "string" && sig.length <= MAX_SIGNATURE_CHARS
        ? { sig }
        : {}),
    });
  }

  const pairs: ChatTurn[][] = [];
  for (let i = 0; i < turns.length - 1; i++) {
    const q = turns[i]!;
    const a = turns[i + 1]!;
    if (q.role === "user" && a.role === "assistant") {
      if (!scanForPromptInjection(q.content).suspicious) pairs.push([q, a]);
      i++;
    }
  }
  return pairs.slice(-MAX_HISTORY_PAIRS).flat();
}

/**
 * The text retrieval should search for. A question that names ServiceNow
 * things on its own ("How do business rules work?") is searched as asked; a
 * follow-up that leans on the conversation ("and for field-level ones?") is
 * searched together with the previous question so it stays on topic.
 */
export function retrievalQueryFor(question: string, history: ChatTurn[]): string {
  const lastQuestion = [...history].reverse().find((t) => t.role === "user")?.content;
  if (!lastQuestion) return question;
  const standsAlone =
    isServiceNowDomainQuery(question) && classifyServiceNowIntent(question).confidence >= 0.5;
  if (standsAlone) return question;
  return `${lastQuestion}\n${question}`.slice(0, MAX_RETRIEVAL_QUERY);
}

/**
 * The exact text that is signed and later checked, and what `normalizeHistory`
 * keeps. Trimming again after the cut is what makes this idempotent: a cut that
 * lands on whitespace must not leave text that canonicalises to something else.
 */
function canonicalAnswer(text: string): string {
  return text.trim().slice(0, MAX_ASSISTANT_CHARS).trimEnd();
}

/**
 * Signature for an answer the server just wrote, or null when no secret is
 * configured (then no assistant text can be vouched for later).
 */
export async function signAnswer(
  text: string,
  secret: string | undefined
): Promise<string | null> {
  if (!secret) return null;
  return hmacSha256Hex(secret, `${SIGNATURE_CONTEXT}\n${canonicalAnswer(text)}`);
}

/**
 * Takes `normalizeHistory` output and returns turns safe to hand to the model.
 * User turns pass through (they were scanned there and are the reader's own
 * words). An assistant turn keeps its text only if its signature matches and
 * the text passes the same injection scan; otherwise the text is replaced, so
 * a forged "assistant" turn never reaches the model as one.
 */
export async function authenticateHistory(
  history: ChatTurn[],
  secret: string | undefined
): Promise<ChatTurn[]> {
  const out: ChatTurn[] = [];
  for (const turn of history) {
    if (turn.role === "user") {
      out.push({ role: "user", content: turn.content });
      continue;
    }
    const expected = await signAnswer(turn.content, secret);
    const genuine =
      expected !== null &&
      typeof turn.sig === "string" &&
      timingSafeEqual(expected, turn.sig) &&
      !scanForPromptInjection(turn.content).suspicious;
    out.push({ role: "assistant", content: genuine ? turn.content : UNVERIFIED_ANSWER });
  }
  return out;
}
