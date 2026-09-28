/**
 * Multi-turn support for the research chat. The browser sends the earlier
 * turns with each question; nothing about a conversation is stored server-side
 * beyond the individual runs.
 *
 * History comes from the client, so it is treated like any other untrusted
 * input: shape-checked, trimmed to a few recent pairs, length-capped, and any
 * pair whose question trips the injection scan is dropped.
 */
import {
  classifyServiceNowIntent,
  isServiceNowDomainQuery,
} from "./retrieval/query-classifier";
import { scanForPromptInjection } from "./security/prompt-injection";

export interface ChatTurn {
  role: "user" | "assistant";
  content: string;
}

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
    turns.push({
      role,
      content: text.slice(0, role === "user" ? MAX_USER_CHARS : MAX_ASSISTANT_CHARS),
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
