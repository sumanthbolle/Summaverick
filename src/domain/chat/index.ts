import type { SonarResponse } from "../../lib/perplexity";
import { isServiceNowDomainQuery, classifyServiceNowIntent, SERVICENOW_DOMAIN_SYSTEM_ADDON, serviceNowSearchHint } from "./servicenow";
import { isUpscDomainQuery, UPSC_DOMAIN_SYSTEM_ADDON } from "./upsc";

const SYSTEM_PROMPT = `You are Summaverick, a general-purpose AI research assistant. You answer questions on any topic — world news, science, technology, business, culture, code, careers, personal decisions, and everyday curiosity — by synthesizing real sources into clear, grounded answers.

Scope:
- Treat every topic as in-scope unless it is unsafe or genuinely unanswerable.
- Do not assume the user is asking about any one domain (e.g. ServiceNow, enterprise IT) unless their question names it explicitly.
- Default to a global perspective. When location matters (news, regulations, sports, weather, markets), ask or state which region you are answering for.

Answer style:
- Give thorough, practical answers — not just summaries. Lead with the concrete answer, then the reasoning and sources.
- Use citation markers like [1], [2] to reference your sources inline. Only cite sources you actually used.
- Structure longer answers with markdown: short headings, bold for key terms, bullet lists for enumerations, code blocks for code.
- When comparing options, use a clear pros/cons or tradeoff structure.
- If the question is ambiguous, address the most likely interpretation first, then note alternatives in one sentence.
- For code: produce runnable, idiomatic code with brief inline comments only when they add value. Note the language/runtime assumed.
- For emotional or personal questions: be warm but not performative. Validate the situation briefly, then give a concrete suggestion.
- Be direct. Skip filler like "Great question!" or "Sure, I'd be happy to help." No self-references like "As an AI…".`;

// A structured answer carries citations on both supported Perplexity wires,
// including the Agent wire, whose wrapper otherwise returns only message text.
const RESPONSE_SCHEMA = {
  type: "object",
  properties: {
    answer: { type: "string" },
    sources: {
      type: "array", items: {
        type: "object", properties: {
          index: { type: "integer" }, title: { type: "string" }, url: { type: "string" },
          snippet: { type: "string" },
        }, required: ["index", "title", "url", "snippet"],
      },
    },
    relatedQuestions: { type: "array", items: { type: "string" } },
  },
  required: ["answer", "sources", "relatedQuestions"],
};

export function sanitize(value: unknown): string {
  return typeof value === "string" ? value.replace(/[\x00-\x08\x0B\x0C\x0E-\x1F\x7F]/g, "").trim() : "";
}

export function buildChatPayload(query: string, context: unknown, depth: unknown): Record<string, unknown> {
  let system = SYSTEM_PROMPT;
  if (isUpscDomainQuery(query)) system += "\n\n" + UPSC_DOMAIN_SYSTEM_ADDON;
  else if (isServiceNowDomainQuery(query)) {
    const intent = classifyServiceNowIntent(query);
    system += "\n\n" + SERVICENOW_DOMAIN_SYSTEM_ADDON + "\n" + serviceNowSearchHint(intent.releaseFamily ?? "australia");
  }
  system += "\nReturn a JSON object with answer (Markdown), sources (index, title, url, snippet), and relatedQuestions (up to four useful follow-ups). Citation numbers in answer must match source indices. Include only http(s) URLs you actually used. Use an empty sources array when no sources were used.";
  const messages: { role: "system" | "user" | "assistant"; content: string }[] = [{ role: "system", content: system }];
  if (Array.isArray(context)) {
    for (const item of context.slice(-10)) {
      if (!item || (item.role !== "user" && item.role !== "assistant")) continue;
      const content = sanitize(item.content).slice(0, 4000);
      if (content) messages.push({ role: item.role, content });
    }
  }
  messages.push({ role: "user", content: query });
  return {
    model: "sonar", messages, temperature: 0.2,
    max_tokens: depth === "research" ? 3000 : depth === "deeper" ? 2000 : 1400,
    web_search_options: { search_context_size: "high" },
    response_format: { type: "json_schema", json_schema: { schema: RESPONSE_SCHEMA } },
  };
}

function httpUrl(value: unknown): string | null {
  if (typeof value !== "string") return null;
  try {
    const url = new URL(value);
    return ["https:", "http:"].includes(url.protocol) ? url.toString() : null;
  } catch { return null; }
}

export function buildChatResult(data: SonarResponse) {
  const content = data.choices?.[0]?.message?.content;
  let structured: Record<string, unknown> | null = null;
  if (typeof content === "string") {
    try { structured = JSON.parse(content.replace(/^```(?:json)?\s*|\s*```$/g, "")); } catch { /* legacy text response */ }
  }
  const answer = sanitize(structured?.answer ?? content);
  if (!answer) return null;
  const native = Array.isArray(data.search_results) ? data.search_results : [];
  const citations = Array.isArray(data.citations) ? data.citations : [];
  const rows: unknown[] = Array.isArray(structured?.sources) ? structured.sources : citations.length
    ? citations.map(url => native.find(row => row?.url === url) ?? { url }) : native;
  const sources = rows.slice(0, 8).flatMap((value, i) => {
    if (!value || typeof value !== "object") return [];
    const row = value as Record<string, unknown>;
    const url = httpUrl(row.url);
    if (!url) return [];
    return [{
      // Never renumber filtered sources: existing [n] markers must stay correct.
      index: Number.isInteger(row.index) && Number(row.index) > 0 ? Number(row.index) : i + 1,
      title: sanitize(row.title).slice(0, 300), url,
      snippet: sanitize(row.snippet).slice(0, 1000),
      source: new URL(url).hostname.replace(/^www\./, ""),
      date: typeof row.date === "string" ? row.date : null,
    }];
  });
  const related = structured?.relatedQuestions ?? data.related_questions;
  return {
    type: "rich", answer, sources,
    relatedQuestions: Array.isArray(related) ? related.map(sanitize).filter(Boolean).slice(0, 4) : [],
  };
}
