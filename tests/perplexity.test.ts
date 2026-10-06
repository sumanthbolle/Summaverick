/**
 * The Perplexity client, against scripted fetches. What matters here is which
 * wire a call travels over and what the caller is told, because Perplexity is
 * retiring the Sonar endpoints and nothing here can reach the live API.
 */
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import {
  PerplexityError,
  buildAgentRequest,
  callPerplexity,
  perplexityConfig,
  readAgentResponse,
  resetPerplexityState,
} from "../src/lib/perplexity";

const AGENT_URL = "https://api.perplexity.ai/v1/responses";
const SONAR_URL = "https://api.perplexity.ai/v1/sonar";

type Reply = () => Response | Promise<Response>;

interface Call {
  url: string;
  body: any;
  headers: Record<string, string>;
}

/** A fetch that answers with `replies` in order (the last one repeats). */
function scripted(...replies: Reply[]) {
  const calls: Call[] = [];
  const f = (async (input: RequestInfo | URL, init?: RequestInit) => {
    calls.push({
      url: String(input),
      body: JSON.parse(String(init?.body)),
      headers: (init?.headers ?? {}) as Record<string, string>,
    });
    const reply = replies[Math.min(calls.length - 1, replies.length - 1)]!;
    return reply();
  }) as typeof fetch;
  return { f, calls };
}

const agentOk = (text: string, before: unknown[] = []): Reply => () =>
  Response.json({
    output: [...before, { type: "message", content: [{ type: "output_text", text }] }],
  });
const sonarOk = (text: string, extra: Record<string, unknown> = {}): Reply => () =>
  Response.json({ choices: [{ message: { content: text } }], ...extra });
const httpStatus = (status: number, body = ""): Reply => () => new Response(body, { status });

const enc = new TextEncoder();
function sse(...frames: string[]): Reply {
  return () =>
    new Response(
      new ReadableStream({
        start(c) {
          for (const f of frames) c.enqueue(enc.encode(f));
          c.close();
        },
      }),
      { status: 200 }
    );
}
const ev = (obj: unknown) => `data: ${JSON.stringify(obj)}\n\n`;

const NEWS = {
  model: "sonar",
  messages: [
    { role: "system", content: "Return strict JSON only." },
    { role: "user", content: "Top story today?" },
  ],
  temperature: 0.1,
  max_tokens: 400,
  search_recency_filter: "day",
  web_search_options: { search_context_size: "medium", user_location: { country: "IN" } },
};

const ANSWER = {
  model: "sonar",
  messages: [
    { role: "system", content: "You are a ServiceNow specialist." },
    { role: "user", content: "How do ACLs evaluate?" },
  ],
  temperature: 0.2,
  disable_search: true,
};

let warn: ReturnType<typeof vi.spyOn>;
beforeEach(() => {
  resetPerplexityState();
  warn = vi.spyOn(console, "warn").mockImplementation(() => {});
});
afterEach(() => warn.mockRestore());

describe("the agent request", () => {
  it("moves the system message to `instructions` and prefixes the model", () => {
    const body = buildAgentRequest(NEWS as never, false);
    expect(body.model).toBe("perplexity/sonar");
    expect(body.instructions).toBe("Return strict JSON only.");
    expect(body.input).toEqual([{ role: "user", content: "Top story today?" }]);
    expect(body.stream).toBe(false);
  });

  it("asks for web search with the recency filter, and only forwards confirmed fields", () => {
    const body = buildAgentRequest(NEWS as never, false);
    expect(body.tools).toEqual([{ type: "web_search", search_recency_filter: "day" }]);
    for (const unconfirmed of ["temperature", "max_tokens", "web_search_options"]) {
      expect(body).not.toHaveProperty(unconfirmed);
    }
  });

  it("sends no search tool when the caller disabled search", () => {
    expect(buildAgentRequest(ANSWER as never, true)).not.toHaveProperty("tools");
  });

  it("forwards a JSON schema and keeps a model id that already names its provider", () => {
    const schema = { type: "json_schema", json_schema: { schema: { type: "object" } } };
    const body = buildAgentRequest(
      { ...NEWS, model: "anthropic/claude", response_format: schema } as never,
      false
    );
    expect(body.response_format).toEqual(schema);
    expect(body.model).toBe("anthropic/claude");
  });
});

describe("a call over the agent wire", () => {
  it("returns the answer in the Sonar shape callers already parse", async () => {
    const { f, calls } = scripted(agentOk('{"headline":"x"}'));
    const r = await callPerplexity({ apiKey: "k", payload: NEWS, fetchImpl: f });
    expect(calls).toHaveLength(1);
    expect(calls[0]!.url).toBe(AGENT_URL);
    expect(calls[0]!.headers.authorization).toBe("Bearer k");
    expect(r.wire).toBe("agent");
    expect(r.data.choices[0]!.message.content).toBe('{"headline":"x"}');
    expect(r.text).toBe('{"headline":"x"}');
  });

  it("reads message text from either content shape", () => {
    expect(readAgentResponse({ output: [{ type: "message", content: "plain" }] }).text).toBe("plain");
    expect(readAgentResponse({ output_text: "top-level" }).text).toBe("top-level");
    expect(readAgentResponse({ output: [] })).toEqual({ text: "", searched: false });
  });

  it("is given the key from the environment only when one is set", () => {
    expect(perplexityConfig({})).toBeNull();
    expect(perplexityConfig({ PERPLEXITY_API_KEY: "k", PERPLEXITY_WIRE: "sonar" })).toEqual({
      apiKey: "k",
      wire: "sonar",
    });
  });
});

describe("falling back to the sonar wire", () => {
  it("retries a rejected agent request on sonar, with the original payload", async () => {
    const { f, calls } = scripted(httpStatus(400, '{"error":"unknown field"}'), sonarOk("from sonar"));
    const r = await callPerplexity({ apiKey: "k", payload: NEWS, fetchImpl: f });
    expect(calls.map((c) => c.url)).toEqual([AGENT_URL, SONAR_URL]);
    expect(calls[1]!.body.temperature).toBe(0.1);
    expect(calls[1]!.body.model).toBe("sonar");
    expect(r.wire).toBe("sonar");
    expect(r.text).toBe("from sonar");
    // The log carries Perplexity's own words, so the field to fix is findable.
    expect(String(warn.mock.calls[0]![0])).toContain("unknown field");
  });

  it("goes straight to sonar while the agent wire is cooling down", async () => {
    const { f, calls } = scripted(httpStatus(404), sonarOk("one"), sonarOk("two"));
    await callPerplexity({ apiKey: "k", payload: NEWS, fetchImpl: f });
    await callPerplexity({ apiKey: "k", payload: NEWS, fetchImpl: f });
    expect(calls.map((c) => c.url)).toEqual([AGENT_URL, SONAR_URL, SONAR_URL]);
  });

  it("falls back on a 5xx, a network error, and a 200 with no message", async () => {
    for (const first of [
      httpStatus(503),
      () => Promise.reject(new TypeError("network down")),
      () => Response.json({ output: [{ type: "search_results" }] }),
    ] as Reply[]) {
      resetPerplexityState();
      const { f, calls } = scripted(first, sonarOk("ok"));
      const r = await callPerplexity({ apiKey: "k", payload: NEWS, fetchImpl: f });
      expect(r.wire).toBe("sonar");
      expect(calls).toHaveLength(2);
    }
  });

  it("falls back when the agent wire stays silent past its timeout", async () => {
    const hang = ((_: RequestInfo | URL, init?: RequestInit) =>
      new Promise((_resolve, reject) =>
        init?.signal?.addEventListener("abort", () => reject(new DOMException("aborted", "AbortError")))
      )) as typeof fetch;
    const urls: string[] = [];
    const f = ((input: RequestInfo | URL, init?: RequestInit) => {
      urls.push(String(input));
      return urls.length === 1 ? hang(input, init) : Promise.resolve(sonarOk("late but fine")());
    }) as typeof fetch;
    const r = await callPerplexity({ apiKey: "k", payload: NEWS, fetchImpl: f, timeoutMs: 40 });
    expect(urls).toEqual([AGENT_URL, SONAR_URL]);
    expect(r.text).toBe("late but fine");
  });

  it("does not retry account-level failures: sonar would say the same", async () => {
    for (const status of [401, 403, 429]) {
      resetPerplexityState();
      const { f, calls } = scripted(httpStatus(status), sonarOk("never"));
      await expect(callPerplexity({ apiKey: "k", payload: NEWS, fetchImpl: f })).rejects.toMatchObject({
        status,
      });
      expect(calls).toHaveLength(1);
    }
  });

  it("falls back when a structured request comes back as prose", async () => {
    const schema = { type: "json_schema", json_schema: { schema: { type: "object" } } };
    const payload = { ...NEWS, response_format: schema };
    const prose = scripted(agentOk("Here is the story you asked for."), sonarOk('{"headline":"x"}'));
    const a = await callPerplexity({ apiKey: "k", payload, fetchImpl: prose.f });
    expect(a.wire).toBe("sonar");

    resetPerplexityState();
    const json = scripted(agentOk('{"headline":"x"}'));
    const b = await callPerplexity({ apiKey: "k", payload, fetchImpl: json.f });
    expect(b.wire).toBe("agent");
  });

  it("honours a pinned wire", async () => {
    const sonarOnly = scripted(sonarOk("s"));
    await callPerplexity({ apiKey: "k", wire: "sonar", payload: NEWS, fetchImpl: sonarOnly.f });
    expect(sonarOnly.calls.map((c) => c.url)).toEqual([SONAR_URL]);

    const agentOnly = scripted(httpStatus(400), sonarOk("never"));
    await expect(
      callPerplexity({ apiKey: "k", wire: "agent", payload: NEWS, fetchImpl: agentOnly.f })
    ).rejects.toBeInstanceOf(PerplexityError);
    expect(agentOnly.calls).toHaveLength(1);
  });

  it("uses the legacy URL the caller names", async () => {
    const { f, calls } = scripted(sonarOk("s"));
    await callPerplexity({
      apiKey: "k",
      wire: "sonar",
      payload: NEWS,
      sonarUrl: "https://api.perplexity.ai/chat/completions",
      fetchImpl: f,
    });
    expect(calls[0]!.url).toBe("https://api.perplexity.ai/chat/completions");
  });

  it("rejects a payload with no model or messages", async () => {
    await expect(callPerplexity({ apiKey: "k", payload: { model: "sonar" } })).rejects.toThrow(
      /model and a messages array/
    );
  });
});

describe("whether the model was free to search", () => {
  it("is flagged when the agent wire searched although search was disabled", async () => {
    const searched = scripted(agentOk("Answer [1]", [{ type: "search_results", results: [] }]));
    expect((await callPerplexity({ apiKey: "k", payload: ANSWER, fetchImpl: searched.f })).searchLeaked).toBe(true);

    resetPerplexityState();
    const clean = scripted(agentOk("Answer [1]"));
    expect((await callPerplexity({ apiKey: "k", payload: ANSWER, fetchImpl: clean.f })).searchLeaked).toBe(false);
  });

  it("is not a leak when the caller wanted search", async () => {
    const { f } = scripted(agentOk("News", [{ type: "search_results" }]));
    expect((await callPerplexity({ apiKey: "k", payload: NEWS, fetchImpl: f })).searchLeaked).toBe(false);
  });

  it("is flagged when sonar rejects `disable_search` and the call goes ahead without it", async () => {
    const { f, calls } = scripted(httpStatus(400), httpStatus(400), sonarOk("Answer [1]"));
    const r = await callPerplexity({ apiKey: "k", payload: ANSWER, fetchImpl: f });
    expect(calls.map((c) => c.url)).toEqual([AGENT_URL, SONAR_URL, SONAR_URL]);
    expect(calls[1]!.body.disable_search).toBe(true);
    expect(calls[2]!.body).not.toHaveProperty("disable_search");
    expect(r.searchLeaked).toBe(true);
  });

  it("does not repeat a rejected sonar request that never set the flag", async () => {
    const { f, calls } = scripted(httpStatus(400));
    await expect(
      callPerplexity({ apiKey: "k", wire: "sonar", payload: NEWS, fetchImpl: f })
    ).rejects.toMatchObject({ status: 400 });
    expect(calls).toHaveLength(1);
  });
});

describe("streaming", () => {
  const delta = (t: string) => ev({ type: "response.output_text.delta", delta: t });

  it("forwards agent text deltas in order", async () => {
    const { f, calls } = scripted(
      sse(delta("Access "), delta("control [1]"), ev({ type: "response.completed", response: {} }))
    );
    const pieces: string[] = [];
    const r = await callPerplexity({ apiKey: "k", payload: ANSWER, fetchImpl: f, onDelta: (t) => void pieces.push(t) });
    expect(pieces).toEqual(["Access ", "control [1]"]);
    expect(r.text).toBe("Access control [1]");
    expect(r.wire).toBe("agent");
    expect(calls[0]!.body.stream).toBe(true);
  });

  it("uses the completed event when the stream carried no deltas", async () => {
    const final = { output: [{ type: "message", content: [{ type: "output_text", text: "All at once" }] }] };
    const { f } = scripted(sse(ev({ type: "response.completed", response: final })));
    const pieces: string[] = [];
    const r = await callPerplexity({ apiKey: "k", payload: ANSWER, fetchImpl: f, onDelta: (t) => void pieces.push(t) });
    expect(pieces).toEqual(["All at once"]);
    expect(r.text).toBe("All at once");
  });

  it("notices search events in the stream", async () => {
    const { f } = scripted(sse(ev({ type: "response.web_search_call.completed" }), delta("Answer")));
    const r = await callPerplexity({ apiKey: "k", payload: ANSWER, fetchImpl: f, onDelta: () => {} });
    expect(r.searchLeaked).toBe(true);
  });

  it("starts over on sonar when the agent wire fails before any text", async () => {
    const chunk = (t: string) => `data: ${JSON.stringify({ choices: [{ delta: { content: t } }] })}\n\n`;
    const { f, calls } = scripted(httpStatus(500), sse(chunk("From "), chunk("sonar")));
    const pieces: string[] = [];
    const r = await callPerplexity({ apiKey: "k", payload: ANSWER, fetchImpl: f, onDelta: (t) => void pieces.push(t) });
    expect(calls.map((c) => c.url)).toEqual([AGENT_URL, SONAR_URL]);
    expect(calls[1]!.body.stream).toBe(true);
    expect(pieces).toEqual(["From ", "sonar"]);
    expect(r.wire).toBe("sonar");
  });

  it("reports a leak when a streamed sonar call had to drop `disable_search`", async () => {
    // The research route streams, so this is the path a real answer takes.
    const chunk = (t: string) => `data: ${JSON.stringify({ choices: [{ delta: { content: t } }] })}\n\n`;
    const { f, calls } = scripted(httpStatus(500), httpStatus(400), sse(chunk("Answer [1]")));
    const pieces: string[] = [];
    const r = await callPerplexity({ apiKey: "k", payload: ANSWER, fetchImpl: f, onDelta: (t) => void pieces.push(t) });
    expect(calls.map((c) => c.url)).toEqual([AGENT_URL, SONAR_URL, SONAR_URL]);
    expect(calls[1]!.body.disable_search).toBe(true);
    expect(calls[2]!.body).not.toHaveProperty("disable_search");
    expect(calls[2]!.body.stream).toBe(true);
    expect(pieces).toEqual(["Answer [1]"]);
    expect(r.searchLeaked).toBe(true);
  });

  it("does not start over once text has reached the reader", async () => {
    // The chunk is delivered on the first pull and the failure comes on the
    // next one. (Erroring inside `start` would discard the queued chunk.)
    const dying: Reply = () => {
      let sent = false;
      return new Response(
        new ReadableStream({
          pull(c) {
            if (!sent) {
              sent = true;
              c.enqueue(enc.encode(delta("Half an ans")));
            } else {
              c.error(new Error("socket closed"));
            }
          },
        }),
        { status: 200 }
      );
    };
    const { f, calls } = scripted(dying, sonarOk("never"));
    const pieces: string[] = [];
    await expect(
      callPerplexity({ apiKey: "k", payload: ANSWER, fetchImpl: f, onDelta: (t) => void pieces.push(t) })
    ).rejects.toThrow("socket closed");
    expect(pieces).toEqual(["Half an ans"]);
    expect(calls).toHaveLength(1);
  });
});
