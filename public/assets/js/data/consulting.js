/*
 * Consulting surface content. Client labels are categories, never names
 * (hard rule). Metrics tagged `illustrative` are placeholders to be replaced
 * with confirmed figures in T11 — nothing here is rendered as a verified claim.
 */

export const WORK = [
  {
    client: "a global enterprise software vendor",
    title: "GenAI platform on Bedrock, with a local fallback",
    problem: "Teams needed generative features without shipping customer data to a single hosted model.",
    approach: "A retrieval and generation layer over Bedrock, with an Ollama path for workloads that had to stay on-prem. One interface, two backends chosen per data-sensitivity.",
    constraint: "The routing decision is the client's to make per workload, so the model choice is config, not code.",
    stack: ["Bedrock", "Ollama", "TypeScript"],
    illustrative: true,
  },
  {
    client: "a warranty operations group",
    title: "Multi-agent warranty automation",
    problem: "Claims triage was manual and slow, and the rules changed often enough that a single prompt kept drifting.",
    approach: "A small set of specialised agents — intake, policy lookup, adjudication — each with a narrow tool surface and a verifier between them.",
    constraint: "No agent can approve a payout on its own; adjudication proposes, a human commits.",
    stack: ["Agents", "Tool routing", "Verifier"],
    illustrative: true,
  },
  {
    client: "a large ServiceNow customer",
    title: "Now Assist adoption strategy",
    problem: "Now Assist was licensed but barely used, and nobody could say which skills were worth turning on.",
    approach: "A read of the actual ticket mix against the shipped skills, a short list of where the payoff was real, and a rollout that turned them on in that order.",
    constraint: "I recommended leaving two skills off — the data to justify them wasn't there yet.",
    stack: ["Now Assist", "Skill Kit"],
    illustrative: true,
  },
  {
    client: "a telecom operator",
    title: "AIOps platform for network operations",
    problem: "Alert volume buried the signal; on-call was drowning in duplicates.",
    approach: "Correlation and dedup ahead of the pager, with retrieval over runbooks so an alert arrives with its likely cause and the last fix attached.",
    constraint: "Automated remediation is suggested, never executed — the runbook link is the product, not an auto-runner.",
    stack: ["Correlation", "Retrieval", "Runbooks"],
    illustrative: true,
  },
];

export const PILLARS = [
  {
    label: "ServiceNow store apps",
    text: "Scoped apps built with the Fluent SDK and taken through certification onto the ServiceNow Store, matched to the release you're actually on rather than the newest one.",
  },
  {
    label: "AI transformation",
    text: "Putting GenAI where it pays off. I look at the real work first, build the guardrails in early, and tell you which parts aren't worth automating yet.",
  },
  {
    label: "Custom agents",
    text: "Agents with a narrow tool surface, a verifier between steps, and the safety gates on by default. The research agent on this page is one of them.",
  },
  {
    label: "Products",
    text: "Some of this work turns into products. The Summaverick Research Agent is the first, running here; each one shows its sources and its own test scores.",
  },
];

export const ENGAGEMENTS = [
  {
    name: "Diagnostic",
    length: "1–2 weeks",
    what: "I read your retrieval or agent setup and tell you where it breaks — hallucination paths, missing citations, injection exposure, cost.",
    end: "A written findings doc with the failures ranked, and a fix I'd start with.",
  },
  {
    name: "Build",
    length: "6–12 weeks",
    what: "I build the retrieval or agent system with you — layered sources, a verifier, an eval suite, and the safety gates wired in from day one.",
    end: "A running system, its evals, and a team that can extend it without me.",
  },
  {
    name: "Embedded",
    length: "ongoing",
    what: "I work inside your team on agentic features — part-time, hands on the code, in your reviews.",
    end: "Whatever we ship, plus the practices that keep it honest after I leave.",
  },
];

/* Captured trace the /agent preview replays. Regenerate from a real run in T8. */
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
