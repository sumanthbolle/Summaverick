/**
 * Server-Sent Events helper for Workers. Returns a streaming Response plus a
 * small writer the handler uses to push named events. The body is a
 * ReadableStream, so it flushes to the browser as each event is written — the
 * client sees the trace build stage by stage.
 */

export interface SseStream {
  response: Response;
  send(event: string, data: unknown): Promise<void>;
  comment(text: string): Promise<void>;
  close(): Promise<void>;
}

export function createSseStream(extraHeaders: Record<string, string> = {}): SseStream {
  const { readable, writable } = new TransformStream();
  const writer = writable.getWriter();
  const encoder = new TextEncoder();

  const write = (chunk: string) => writer.write(encoder.encode(chunk));

  const response = new Response(readable, {
    headers: {
      "content-type": "text/event-stream; charset=utf-8",
      "cache-control": "no-cache, no-transform",
      "connection": "keep-alive",
      "x-accel-buffering": "no",
      ...extraHeaders,
    },
  });

  return {
    response,
    async send(event, data) {
      const payload = typeof data === "string" ? data : JSON.stringify(data);
      await write(`event: ${event}\ndata: ${payload}\n\n`);
    },
    async comment(text) {
      await write(`: ${text}\n\n`);
    },
    async close() {
      try {
        await writer.close();
      } catch {
        /* already closed */
      }
    },
  };
}
