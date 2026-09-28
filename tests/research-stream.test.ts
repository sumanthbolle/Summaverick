import { describe, expect, it } from "vitest";
import { countCitations, readCompletionStream } from "../src/domain/research/pipeline";

function streamOf(chunks: string[]): ReadableStream<Uint8Array> {
  const enc = new TextEncoder();
  return new ReadableStream({
    start(controller) {
      for (const c of chunks) controller.enqueue(enc.encode(c));
      controller.close();
    },
  });
}

describe("countCitations", () => {
  it("counts each listed source once and flags numbers with no source", () => {
    const prose = "Use GlideRecord [1]. Query with addQuery [1][2]. See also [7].";
    expect(countCitations(prose, 4)).toEqual({ resolved: 2, unresolved: 1 });
  });

  it("ignores markdown links and plain brackets", () => {
    expect(countCitations("See [the docs](https://x.test) and [a] note.", 4)).toEqual({
      resolved: 0,
      unresolved: 0,
    });
  });
});

describe("readCompletionStream", () => {
  it("forwards delta chunks in order, across split network reads", async () => {
    const pieces: string[] = [];
    const full = await readCompletionStream(
      streamOf([
        'data: {"choices":[{"delta":{"content":"Glide"}}]}\n',
        'data: {"choices":[{"delta":{"content":"Record [1]"}}]}\n\ndata: {"cho',
        'ices":[{"delta":{"content":"."}}]}\n\n',
        "data: [DONE]\n\n",
      ]),
      (t) => { pieces.push(t); }
    );
    expect(pieces).toEqual(["Glide", "Record [1]", "."]);
    expect(full).toBe("GlideRecord [1].");
  });

  it("handles chunks that carry the whole message so far", async () => {
    const pieces: string[] = [];
    const full = await readCompletionStream(
      streamOf([
        'data: {"choices":[{"message":{"content":"ACLs"}}]}\n',
        'data: {"choices":[{"message":{"content":"ACLs evaluate"}}]}\n',
        "data: not json\n",
      ]),
      (t) => { pieces.push(t); }
    );
    expect(pieces).toEqual(["ACLs", " evaluate"]);
    expect(full).toBe("ACLs evaluate");
  });
});
