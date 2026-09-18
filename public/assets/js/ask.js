/*
 * The research agent page.
 *
 * The answer comes first, the sources are inspectable next to it, and the
 * retrieval trace is available but collapsed — it is developer detail, not the
 * result. Every outcome is labelled for what it is: a model-written answer, a
 * set of matching passages with no written answer, no relevant source, an
 * unreachable source, a rejected query, a rate limit, or an unreachable agent.
 * None of them is presented as another.
 */

import { el, $ } from "./lib/dom.js";
import { streamResearch } from "./lib/agent-stream.js";
import { initChrome } from "./chrome.js";

const CHIPS = [
  "How do I use GlideRecord to query the incident table?",
  "How do I define a Business Rule with the Fluent SDK?",
  "What is the CMDB and how do CI relationships work?",
  "How do ACLs evaluate on a table?",
];

/** How each outcome introduces itself. Nothing here overstates the run. */
const MODES = {
  model_answer: {
    tone: "ok",
    label: "Model answer, written from the sources below",
    note: (d) =>
      `Written by ${d.llmModel || "the configured model"} from the retrieved passages only. Check it against the sources.`,
  },
  source_passages: {
    tone: "info",
    label: "Matching sources — no written answer",
    note: (d) =>
      d.llmReason === "failed"
        ? `The answer model was unavailable for this run${d.llmError ? ` (${d.llmError})` : ""}, so nothing was written. These are the documentation passages that matched your question.`
        : "No answer model is configured on this deployment, so nothing was written for you. These are the documentation passages that matched your question, in relevance order.",
  },
  insufficient_evidence: {
    tone: "warn",
    label: "No source close enough to answer from",
    note: () =>
      "Retrieval ran and found nothing that matched your question closely enough to answer from. Rather than stitch loosely related pages together, the agent stops here.",
  },
  sources_unavailable: {
    tone: "warn",
    label: "Documentation source unavailable",
    note: () =>
      "The ServiceNow documentation could not be reached for this run, so there was nothing to answer from. Your question is still in the box — try again in a moment.",
  },
  out_of_scope: {
    tone: "info",
    label: "Outside this agent's scope",
    note: () =>
      "This agent only researches the ServiceNow platform. It did not guess at an answer from outside that scope.",
  },
};

const CHECK_LABELS = {
  sourcesFound: "Sources found",
  relevance: "Source relevance",
  groundedness: "Answer groundedness",
  completeness: "Answer completeness",
};

function initAsk() {
  const form = $("[data-ask-form]");
  const input = $("[data-ask-input]");
  const chipsWrap = $("[data-ask-chips]");
  const results = $("[data-ask-results]");
  const live = $("[data-ask-live]");
  const output = $("[data-ask-output]");
  const status = $("[data-ask-status]");
  const runBtn = $("[data-ask-run]");
  if (!form || !input || !results || !live || !output) return;

  let lastQuery = "";
  let stepIndex = 0;

  const tick = (kind) =>
    kind === "block" ? "✕" : kind === "active" ? "○" : kind === "muted" ? "·" : "✓";

  const addStep = (label, detail, kind) => {
    const row = el("div", { class: "trace-step", dataset: { kind: kind || "ok" } }, [
      el("span", { class: "tick", "aria-hidden": "true", text: tick(kind) }),
      el("div", {}, [
        el("span", { class: "k", text: label }),
        " ",
        el("span", { class: "v", text: detail || "" }),
      ]),
    ]);
    live.append(row);
    const i = stepIndex++;
    requestAnimationFrame(() =>
      setTimeout(() => row.classList.add("show"), 20 + Math.min(i, 6) * 40)
    );
  };

  /** Percentages read better than 0–1 scores for a non-developer. */
  const pct = (n) => `${Math.round((n || 0) * 100)}%`;
  const plural = (n, word) => `${n} ${n === 1 ? word : `${word}s`}`;

  const checkRows = (checks) => {
    if (!checks) return [];
    const relevance = checks.relevance || {};
    const grounded = checks.groundedness || {};
    const count = checks.sourcesFound?.count ?? 0;
    const rows = [
      [
        CHECK_LABELS.sourcesFound,
        count === 0 ? "none released" : `${plural(count, "passage")} released`,
        checks.sourcesFound?.ok ? "ok" : "warn",
      ],
      [
        CHECK_LABELS.relevance,
        // With nothing retrieved there is no closest passage, and quoting 0%
        // would imply one was compared and scored badly.
        count === 0
          ? "not checked — there were no passages to compare against your question"
          : `${relevance.verdict || "none"} — the closest passage matched ${pct(relevance.topScore)} of your question's distinctive terms`,
        count === 0
          ? "idle"
          : relevance.verdict === "strong" ? "ok" : relevance.verdict === "weak" ? "warn" : "bad",
      ],
      [
        CHECK_LABELS.groundedness,
        grounded.status === "checked"
          ? `${grounded.grounded} of ${plural(grounded.sentences, "sentence")} trace back to a cited passage, by ${grounded.method}`
          : "not applicable — no model wrote an answer, so there is no prose to check",
        grounded.status === "checked"
          ? (grounded.unsupported || []).length === 0
            ? "ok"
            : "warn"
          : "idle",
      ],
      [
        CHECK_LABELS.completeness,
        checks.completeness?.note ||
          "not assessed — no check decides whether your question was fully answered",
        "idle",
      ],
    ];

    return [
      el("h3", { class: "checks__title", text: "What was and was not checked" }),
      el(
        "dl",
        { class: "checks" },
        rows.flatMap(([term, detail, tone]) => [
          el("dt", { dataset: { tone }, text: term }),
          el("dd", { text: detail }),
        ])
      ),
    ];
  };

  const sourceList = (sources) => {
    if (!sources || !sources.length) return null;
    return el("section", { class: "sources", "aria-label": "Sources" }, [
      el("h3", { class: "sources__title", text: `Sources (${sources.length})` }),
      el(
        "ol",
        { class: "sources__list" },
        sources.map((s) =>
          el("li", { class: "source" }, [
            el("p", {
              class: "source__meta",
              text: [
                s.sourceType === "product_documentation"
                  ? "ServiceNow product documentation"
                  : s.sourceType.replace(/_/g, " "),
                s.publication,
                `match ${pct(s.relevance)}`,
              ]
                .filter(Boolean)
                .join(" · "),
            }),
            el("h4", { class: "source__title" }, [
              s.url
                ? el("a", { href: s.url, rel: "noopener", target: "_blank", text: s.title })
                : s.title,
            ]),
            el("p", { class: "source__snippet", text: s.snippet }),
          ])
        )
      ),
    ]);
  };

  const scopeNote = () =>
    el("p", { class: "answer-card__scope" }, [
      "This agent reads the ServiceNow product documentation for the current release family, and the Fluent SDK documentation. It has no access to your instance, and it does not research anything outside ServiceNow.",
    ]);

  // A bordered button, not the quiet variant: in a failed run this is the one
  // thing to do next, and it has to look like something to press.
  const retryButton = () =>
    el("button", {
      class: "btn",
      type: "button",
      text: "Try that question again",
      onclick: () => ask(lastQuery),
    });

  const continuation = () =>
    el("p", { class: "answer-card__next" }, [
      "Building something like this? ",
      el("a", { href: "/#contact", text: "Tell us about the workflow" }),
      " — retrieval, evaluation and fallback behaviour included.",
    ]);

  /** Move the live steps into a collapsed details block below the answer. */
  const collapseTrace = (stepCount) => {
    if (!stepCount) return null;
    const details = el("details", { class: "trace-details" }, [
      el("summary", { text: `How the agent got here (${stepCount} steps)` }),
    ]);
    const body = el("div", { class: "trace" });
    while (live.firstChild) body.append(live.firstChild);
    details.append(body);
    return details;
  };

  const renderOutcome = ({ mode, heading, note, children }) => {
    const spec = MODES[mode] || {};
    const tone = spec.tone || "info";
    const stepCount = live.childElementCount;
    const card = el("article", { class: "answer-card", dataset: { mode } }, [
      el("p", { class: "status-pill", dataset: { tone }, text: heading }),
      note ? el("p", { class: "answer-card__note", text: note }) : null,
      ...(children || []).filter(Boolean),
      collapseTrace(stepCount),
    ]);
    output.replaceChildren(card);
    if (status) status.textContent = heading;
  };

  const renderAnswer = (data) => {
    const spec = MODES[data.mode] || MODES.source_passages;
    const children = [];

    if (data.mode === "model_answer") {
      children.push(el("div", { class: "answer-card__text", text: data.text }));
    }
    if (data.mode === "insufficient_evidence" || data.mode === "out_of_scope") {
      children.push(scopeNote());
    }
    if (data.mode === "sources_unavailable") {
      children.push(el("div", { class: "answer-card__actions" }, [retryButton()]));
    }

    const sources = sourceList(data.sources);
    if (sources) children.push(sources);
    children.push(...checkRows(data.checks));
    if (data.mode === "model_answer" || data.mode === "source_passages") {
      children.push(continuation());
    }

    renderOutcome({
      mode: data.mode,
      heading: spec.label,
      note: spec.note ? spec.note(data) : null,
      children,
    });
  };

  const renderProblem = ({ mode, heading, note, withRetry = true, withScope = false }) => {
    renderOutcome({
      mode,
      heading,
      note,
      children: [
        withScope ? scopeNote() : null,
        withRetry ? el("div", { class: "answer-card__actions" }, [retryButton()]) : null,
      ],
    });
  };

  const setBusy = (busy) => {
    if (runBtn) {
      runBtn.disabled = busy;
      runBtn.textContent = busy ? "Researching…" : "Ask";
    }
    results.dataset.state = busy ? "running" : "done";
    if (busy && status) status.textContent = "Searching the ServiceNow documentation…";
  };

  async function ask(query) {
    const q = (query || "").trim();
    if (!q) {
      input.focus();
      return;
    }
    // The question stays in the box through every outcome, so a retry after a
    // rate limit or an outage never costs the visitor their typing.
    lastQuery = q;
    input.value = q;
    results.hidden = false;
    stepIndex = 0;
    live.replaceChildren();
    output.replaceChildren();
    setBusy(true);

    let answered = false;
    try {
      await streamResearch(q, {
        stage: (d) => addStep(d.label, d.detail, d.kind),
        blocked: (d) => {
          answered = true;
          renderProblem({
            mode: "blocked",
            heading: "Query rejected before any model call",
            note:
              d.reason ||
              "The question matched a known prompt-injection pattern, so it was rejected before any model call. That is a pattern check, not a guarantee.",
            withRetry: false,
          });
        },
        answer: (d) => {
          answered = true;
          renderAnswer(d);
        },
        error: (d) => {
          answered = true;
          renderProblem({
            mode: "sources_unavailable",
            heading: "The run failed",
            note: d.message
              ? `${d.message}. Your question is still in the box.`
              : "The run failed before an answer was produced. Your question is still in the box.",
          });
        },
        rateLimited: (msg) => {
          answered = true;
          renderProblem({
            mode: "sources_unavailable",
            heading: "Rate limit reached",
            note: `${msg || "Too many questions from this browser."} The public demo is capped at 6 questions a minute and 50 a day. Your question is still in the box.`,
          });
        },
      });
      if (!answered) {
        renderProblem({
          mode: "sources_unavailable",
          heading: "The run ended without an answer",
          note: "The connection closed before a result arrived. Your question is still in the box.",
        });
      }
    } catch (err) {
      renderProblem({
        mode: "sources_unavailable",
        heading: "The agent could not be reached",
        note: "The research endpoint did not respond. Your question is still in the box — try again shortly.",
      });
    } finally {
      setBusy(false);
    }
  }

  chipsWrap?.replaceChildren(
    ...CHIPS.map((c) =>
      el("button", { class: "chip", type: "button", text: c, onclick: () => ask(c) })
    )
  );

  form.addEventListener("submit", (e) => {
    e.preventDefault();
    ask(input.value);
  });
  // Enter submits; Shift+Enter for a newline.
  input.addEventListener("keydown", (e) => {
    if (e.key === "Enter" && !e.shiftKey) {
      e.preventDefault();
      ask(input.value);
    }
  });
}

initChrome();
initAsk();
