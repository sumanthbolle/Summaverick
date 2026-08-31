/*
 * Captured agent-preview fixtures. Live traces come from POST /api/research/stream;
 * these run only when the Worker is absent (static hosting).
 */

export const AGENT_TRACE = {
  ok: [
    { k: "classify", v: "intent → platform_development · confidence 0.8", kind: "ok" },
    { k: "expand", v: "+2 query variants", kind: "ok" },
    { k: "injection check", v: "clean", kind: "ok" },
    { k: "Layer 2 · product docs", v: "4 candidate(s) · answered here", kind: "ok" },
    { k: "Layer 3 · live instance", v: "off by default — skipped", kind: "muted" },
    { k: "dedup + rank", v: "4 ranked of 4 candidate(s)", kind: "ok" },
    { k: "verify", v: "6 citation(s) · 0 unsupported claim(s)", kind: "ok" },
    { k: "answer", v: "returned with citations", kind: "ok" },
  ],
  blocked: [
    { k: "classify", v: "intent → suspicious", kind: "ok" },
    { k: "injection check", v: "prompt-injection pattern detected", kind: "block" },
    { k: "blocked", v: "query rejected before the model — nothing was sent", kind: "block" },
  ],
};

export const INJECTION_MARKERS = [
  "ignore all", "ignore previous", "disregard", "system prompt",
  "print the", "reveal", "admin token", "act as",
];
