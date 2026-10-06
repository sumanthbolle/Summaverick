/**
 * The one place that talks to Perplexity.
 *
 * Perplexity is retiring the Sonar chat-completions endpoints (announced end
 * date 2026-09-27) in favour of the Agent API, `POST /v1/responses`. Callers
 * keep building Sonar-shaped payloads and keep reading
 * `choices[0].message.content`; this module sends the request over whichever
 * wire works and hands back the Sonar shape. A change to either API is a change
 * to this file and nothing else.
 *
 *   agent  POST /v1/responses. Where Perplexity is moving.
 *   sonar  The legacy endpoints. Perplexity says it is answering synchronous
 *          and streaming Sonar requests by rewriting them as Agent API
 *          requests, model by model, until they are switched off.
 *
 * `auto` (the default) tries the agent wire first and falls back to sonar when
 * the agent wire fails in a way sonar would not share: a rejected request, a
 * response in an unexpected shape, a 5xx, a timeout. A failure puts the agent
 * wire on a short cooldown so one bad wire does not add a failed request to
 * every call. Account-level answers (401, 403, 429) are the same on both wires
 * and are thrown, not retried. Once text has reached the reader there is no
 * clean way to start over, so a stream that fails midway is thrown as well.
 *
 * `PERPLEXITY_WIRE=sonar` or `=agent` pins one wire (a kill switch if the other
 * misbehaves). Pinning `agent` never falls back.
 *
 * NOT CONFIRMED AGAINST PERPLEXITY'S DOCS. Everything below that says what the
 * Agent API accepts is in `buildAgentRequest` and `readAgentResponse`; it comes
 * from secondary descriptions of the API, not the reference. The first rejected
 * request is logged with Perplexity's own error text, so the fix is there.
 */

const AGENT_URL = "https://api.perplexity.ai/v1/responses";
const SONAR_URL = "https://api.perplexity.ai/v1/sonar";

const DEFAULT_TIMEOUT_MS = 30_000;
/** How long the agent wire sits out after failing in `auto` mode. */
const AGENT_COOLDOWN_MS = 5 * 60_000;

export type PerplexityWire = "agent" | "sonar";
export type WirePreference = PerplexityWire | "auto";

/** The Sonar request vocabulary every caller already uses. */
interface SonarPayload {
  model: string;
  messages: { role: "system" | "user" | "assistant"; content: string }[];
  disable_search?: boolean;
  search_recency_filter?: string;
  response_format?: unknown;
  [other: string]: unknown;
}

/** A Sonar-shaped response, whichever wire produced it. */
export interface SonarResponse {
  choices: { message: { role?: string; content: unknown } }[];
  [other: string]: unknown;
}

export interface PerplexityConfig {
  apiKey: string;
  /** Raw `PERPLEXITY_WIRE` value; anything but "agent" or "sonar" means auto. */
  wire?: string;
}

export interface PerplexityCall extends PerplexityConfig {
  /** A Sonar chat-completions body. */
  payload: Record<string, unknown>;
  /** Longest wait for response headers (and, when streaming, between chunks). */
  timeoutMs?: number;
  /** Receives text as it is written. Presence switches the call to streaming. */
  onDelta?: (text: string) => void | Promise<void>;
  /** Legacy endpoint. Defaults to /v1/sonar. */
  sonarUrl?: string;
  fetchImpl?: typeof fetch;
}

export interface PerplexityResult {
  /** Sonar-shaped body, so `parseStructured` and friends read it unchanged. */
  data: SonarResponse;
  text: string;
  wire: PerplexityWire;
  /**
   * The request said not to search the web, but the answer was produced with
   * search. Any `[n]` markers in it may then point at the model's own results
   * and not the sources it was given.
   */
  searchLeaked: boolean;
}

export class PerplexityError extends Error {
  constructor(
    readonly wire: PerplexityWire,
    readonly status: number | null,
    message: string,
    /** Perplexity's own error text, for the log only. */
    readonly detail = ""
  ) {
    super(message);
    this.name = "PerplexityError";
  }
}

/** `null` when no key is configured, so a route can answer "not configured". */
export function perplexityConfig(env: {
  PERPLEXITY_API_KEY?: string;
  PERPLEXITY_WIRE?: string;
}): PerplexityConfig | null {
  return env.PERPLEXITY_API_KEY
    ? { apiKey: env.PERPLEXITY_API_KEY, wire: env.PERPLEXITY_WIRE }
    : null;
}

/* ───────────────────────────── the two wires ───────────────────────────── */

/**
 * Sonar request → Agent API request. Only fields with a known Agent API home
 * are forwarded. Not forwarded, because their Agent API names are unconfirmed
 * and an unknown field would get the whole request rejected: `temperature`,
 * `max_tokens`, `web_search_options` (`user_location`, `search_context_size`).
 */
export function buildAgentRequest(
  payload: SonarPayload,
  stream: boolean
): Record<string, unknown> {
  const system = payload.messages
    .filter((m) => m.role === "system")
    .map((m) => m.content)
    .join("\n\n");

  const body: Record<string, unknown> = {
    model: agentModelId(payload.model),
    input: payload.messages
      .filter((m) => m.role !== "system")
      .map((m) => ({ role: m.role, content: m.content })),
    stream,
  };
  if (system) body.instructions = system;
  if (payload.response_format) body.response_format = payload.response_format;
  if (!payload.disable_search) {
    const search: Record<string, unknown> = { type: "web_search" };
    if (payload.search_recency_filter) {
      search.search_recency_filter = payload.search_recency_filter;
    }
    body.tools = [search];
  }
  return body;
}

/** "sonar" → "perplexity/sonar"; an id that already names a provider is kept. */
function agentModelId(model: string): string {
  return model.includes("/") ? model : `perplexity/${model}`;
}

/** Agent API response → the text it wrote, and whether it searched. */
export function readAgentResponse(json: unknown): { text: string; searched: boolean } {
  const root = (json ?? {}) as { output?: unknown; output_text?: unknown };
  let text = "";
  let searched = false;
  if (Array.isArray(root.output)) {
    for (const item of root.output as { type?: unknown; content?: unknown }[]) {
      const type = String(item?.type ?? "");
      if (/search/i.test(type)) searched = true;
      if (type !== "message") continue;
      if (typeof item.content === "string") {
        text += item.content;
      } else if (Array.isArray(item.content)) {
        for (const part of item.content as { text?: unknown }[]) {
          if (typeof part?.text === "string") text += part.text;
        }
      }
    }
  }
  if (!text && typeof root.output_text === "string") text = root.output_text;
  return { text, searched };
}

/**
 * Reads a completion stream and forwards each new piece of text. Understands
 * both shapes: Sonar chunks (`data: {"choices":[{"delta":…}]}`, or a chunk
 * carrying the whole message so far) and Agent API events
 * (`response.output_text.delta`, then `response.completed`).
 */
export async function readStream(
  body: ReadableStream<Uint8Array>,
  onDelta: (text: string) => void | Promise<void>,
  onChunk?: () => void
): Promise<{ text: string; searched: boolean }> {
  const reader = body.getReader();
  const dec = new TextDecoder();
  let buf = "";
  let full = "";
  let searched = false;

  const emit = async (piece: string) => {
    if (!piece) return;
    full += piece;
    await onDelta(piece);
  };

  for (;;) {
    const { value, done } = await reader.read();
    if (done) break;
    onChunk?.();
    buf += dec.decode(value, { stream: true });
    let nl: number;
    while ((nl = buf.indexOf("\n")) >= 0) {
      const line = buf.slice(0, nl).trim();
      buf = buf.slice(nl + 1);
      if (!line.startsWith("data:")) continue;
      const data = line.slice(5).trim();
      if (!data || data === "[DONE]") continue;
      let chunk: {
        type?: unknown;
        delta?: unknown;
        item?: { type?: unknown };
        response?: unknown;
        search_results?: unknown;
        choices?: { delta?: { content?: string }; message?: { content?: string } }[];
      };
      try {
        chunk = JSON.parse(data);
      } catch {
        continue;
      }

      if (typeof chunk.type === "string") {
        if (/search/i.test(chunk.type) || /search/i.test(String(chunk.item?.type ?? ""))) {
          searched = true;
        }
        if (chunk.type === "response.output_text.delta" && typeof chunk.delta === "string") {
          await emit(chunk.delta);
        } else if (chunk.type === "response.completed") {
          // Only needed when the stream carried no deltas of its own.
          const final = readAgentResponse((chunk.response ?? {}) as unknown);
          if (final.searched) searched = true;
          if (!full) await emit(final.text);
        }
        continue;
      }

      if (Array.isArray(chunk.search_results) && chunk.search_results.length) searched = true;
      const choice = chunk.choices?.[0];
      let piece = choice?.delta?.content ?? "";
      if (!piece && choice?.message?.content && choice.message.content.startsWith(full)) {
        piece = choice.message.content.slice(full.length);
      }
      await emit(piece);
    }
  }
  return { text: full, searched };
}

/** The text-only form of `readStream`. */
export async function readCompletionStream(
  body: ReadableStream<Uint8Array>,
  onDelta: (text: string) => void | Promise<void>
): Promise<string> {
  return (await readStream(body, onDelta)).text;
}

/* ───────────────────────────────── calling ──────────────────────────────── */

// Whether the agent wire is worth trying, per isolate. Not shared state that
// matters: a cold isolate costs one extra request, which is the whole price.
let agentDownUntil = 0;

/** Test hook: forget any agent-wire cooldown. */
export function resetPerplexityState(): void {
  agentDownUntil = 0;
}

export async function callPerplexity(call: PerplexityCall): Promise<PerplexityResult> {
  const payload = asSonarPayload(call.payload);
  const pref = wirePreference(call.wire);
  const timeoutMs = call.timeoutMs ?? DEFAULT_TIMEOUT_MS;
  const run: Run = {
    apiKey: call.apiKey,
    payload,
    timeoutMs,
    onDelta: call.onDelta,
    sonarUrl: call.sonarUrl ?? SONAR_URL,
    f: call.fetchImpl ?? ((input, init) => fetch(input, init)),
  };

  if (pref !== "sonar" && (pref === "agent" || Date.now() >= agentDownUntil)) {
    const emitted = { any: false };
    try {
      return await viaAgent(run, pref === "agent" ? timeoutMs : agentAttemptMs(timeoutMs), emitted);
    } catch (err) {
      if (pref === "agent" || emitted.any || !mayFallBack(err)) throw err;
      agentDownUntil = Date.now() + AGENT_COOLDOWN_MS;
      console.warn(
        `perplexity: agent wire failed (${describe(err)}); using the sonar wire for ${AGENT_COOLDOWN_MS / 60_000} min`
      );
    }
  }
  return viaSonar(run, timeoutMs);
}

interface Run {
  apiKey: string;
  payload: SonarPayload;
  timeoutMs: number;
  onDelta?: PerplexityCall["onDelta"];
  sonarUrl: string;
  f: typeof fetch;
}

async function viaAgent(
  run: Run,
  ms: number,
  emitted: { any: boolean }
): Promise<PerplexityResult> {
  const stream = Boolean(run.onDelta);
  const wd = watchdog(ms);
  try {
    const res = await post(run.f, AGENT_URL, run.apiKey, buildAgentRequest(run.payload, stream), wd);
    if (!res.ok) throw await httpError("agent", res);

    let text: string;
    let searched: boolean;
    if (stream && res.body) {
      ({ text, searched } = await readStream(
        res.body,
        async (piece) => {
          emitted.any = true;
          await run.onDelta!(piece);
        },
        wd.touch
      ));
    } else {
      ({ text, searched } = readAgentResponse(await res.json()));
      if (text && run.onDelta) {
        emitted.any = true;
        await run.onDelta(text);
      }
    }

    if (!text.trim()) {
      throw new PerplexityError("agent", null, "Perplexity agent response had no message text");
    }
    if (run.payload.response_format && !isJson(text)) {
      throw new PerplexityError("agent", null, "Perplexity agent answer was not the JSON the request asked for");
    }
    return {
      data: { choices: [{ message: { role: "assistant", content: text } }] },
      text,
      wire: "agent",
      searchLeaked: Boolean(run.payload.disable_search) && searched,
    };
  } finally {
    wd.stop();
  }
}

async function viaSonar(run: Run, ms: number): Promise<PerplexityResult> {
  const stream = Boolean(run.onDelta);
  const wd = watchdog(ms);
  try {
    const body: Record<string, unknown> = stream ? { ...run.payload, stream: true } : { ...run.payload };
    let res = await post(run.f, run.sonarUrl, run.apiKey, body, wd);
    let searchFlagDropped = false;
    if (res.status === 400 && body.disable_search) {
      // A model or account that rejects `disable_search` still gets an answer;
      // the evidence-only instruction and the citation count still apply, and
      // the caller is told the model was free to search.
      delete body.disable_search;
      searchFlagDropped = true;
      await res.body?.cancel().catch(() => {});
      res = await post(run.f, run.sonarUrl, run.apiKey, body, wd);
    }
    if (!res.ok) throw await httpError("sonar", res);

    if (stream && res.body) {
      const { text, searched } = await readStream(res.body, run.onDelta!, wd.touch);
      return {
        data: { choices: [{ message: { role: "assistant", content: text } }] },
        text,
        wire: "sonar",
        searchLeaked: searchFlagDropped || (Boolean(run.payload.disable_search) && searched),
      };
    }

    const data = (await res.json()) as SonarResponse;
    const content = data?.choices?.[0]?.message?.content;
    const text = typeof content === "string" ? content : "";
    if (text && run.onDelta) await run.onDelta(text);
    const results = (data as { search_results?: unknown }).search_results;
    return {
      data,
      text,
      wire: "sonar",
      searchLeaked:
        searchFlagDropped ||
        (Boolean(run.payload.disable_search) && Array.isArray(results) && results.length > 0),
    };
  } finally {
    wd.stop();
  }
}

/* ───────────────────────────────── helpers ──────────────────────────────── */

function asSonarPayload(raw: Record<string, unknown>): SonarPayload {
  if (typeof raw.model !== "string" || !Array.isArray(raw.messages)) {
    throw new Error("Perplexity payload needs a model and a messages array");
  }
  return raw as SonarPayload;
}

function wirePreference(raw: string | undefined): WirePreference {
  const v = (raw ?? "").trim().toLowerCase();
  return v === "agent" || v === "sonar" ? v : "auto";
}

/** In `auto`, the agent wire gets half the budget (at least 8 s) before sonar steps in. */
function agentAttemptMs(timeoutMs: number): number {
  return Math.min(timeoutMs, Math.max(8_000, Math.floor(timeoutMs / 2)));
}

/** Account-level answers are the same on sonar, so retrying there changes nothing. */
function mayFallBack(err: unknown): boolean {
  return !(err instanceof PerplexityError && (err.status === 401 || err.status === 403 || err.status === 429));
}

function describe(err: unknown): string {
  if (err instanceof PerplexityError) {
    return err.detail ? `${err.message}: ${err.detail}` : err.message;
  }
  return err instanceof Error ? err.message : String(err);
}

async function httpError(wire: PerplexityWire, res: Response): Promise<PerplexityError> {
  let detail = "";
  try {
    detail = (await res.text()).slice(0, 300);
  } catch {
    /* no body to report */
  }
  return new PerplexityError(wire, res.status, `Perplexity ${wire} ${res.status}`, detail);
}

function isJson(text: string): boolean {
  try {
    JSON.parse(text);
    return true;
  } catch {
    return false;
  }
}

/** Aborts after `ms` of silence. `touch` restarts the clock (a chunk arrived). */
function watchdog(ms: number) {
  const ctrl = new AbortController();
  let timer = setTimeout(() => ctrl.abort(), ms);
  return {
    signal: ctrl.signal,
    touch() {
      clearTimeout(timer);
      timer = setTimeout(() => ctrl.abort(), ms);
    },
    stop() {
      clearTimeout(timer);
    },
  };
}

function post(
  f: typeof fetch,
  url: string,
  apiKey: string,
  body: unknown,
  wd: { signal: AbortSignal }
): Promise<Response> {
  return f(url, {
    method: "POST",
    headers: { authorization: `Bearer ${apiKey}`, "content-type": "application/json" },
    body: JSON.stringify(body),
    signal: wd.signal,
  });
}
