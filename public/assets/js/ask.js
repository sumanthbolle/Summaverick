/*
 * ServiceNow reference search. Submits to /api/research/stream and renders what
 * the run actually produced:
 *
 *   model_answer          prose from an answer model, sources underneath
 *   source_results        matching documentation pages — never called an answer
 *   insufficient_evidence no page matched closely enough to quote
 *   out_of_domain         outside the ServiceNow scope of the tool
 *
 * The retrieval trace is developer detail, so it collapses into a disclosure.
 * Labels describe the checks that ran: linked sources are not a quality verdict,
 * so answer quality is reported as not evaluated rather than "verified".
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

const MODE_TEXT = {
  source_results:
    "This deployment returns matching ServiceNow documentation pages with an excerpt and a link. It does not write answers.",
  model_answer:
    "This deployment writes an answer from the documentation it retrieves, and lists the sources it used so you can check them.",
};

function initAsk() {
  const form = $("[data-ask-form]");
  const input = $("[data-ask-input]");
  const chipsWrap = $("[data-ask-chips]");
  const results = $("[data-ask-results]");
  const statusLine = $("[data-ask-status]");
  const output = $("[data-ask-answer]");
  const runButton = $("[data-ask-run]");
  const modeLine = $("[data-ask-mode]");
  if (!form || !input || !results || !output) return;

  let lastQuestion = "";

  /* The mode is knowable before anyone asks anything, so say it up front. */
  async function showMode() {
    if (!modeLine) return;
    try {
      const res = await fetch("/api/research/mode");
      if (!res.ok) return;
      const data = await res.json();
      const text = MODE_TEXT[data.mode];
      if (text) modeLine.textContent = text;
    } catch (e) {
      /* Keep the conservative wording already in the HTML. */
    }
  }

  const setBusy = (busy) => {
    if (!runButton) return;
    runButton.disabled = busy;
    runButton.textContent = busy ? "Searching…" : "Search";
  };

  const say = (text) => {
    if (statusLine) statusLine.textContent = text;
  };

  const sourceCard = (source, index) => {
    const heading = source.url
      ? el("a", { class: "src-title", href: source.url, rel: "noopener", text: source.title })
      : el("span", { class: "src-title", text: source.title });
    return el("li", { class: "src" }, [
      el("span", { class: "src-index", text: String(index + 1) }),
      el("div", {}, [
        heading,
        source.snippet ? el("p", { class: "src-snippet", text: source.snippet }) : null,
        el("p", { class: "src-meta", text: sourceLabel(source) }),
      ]),
    ]);
  };

  const checksRow = (mode, checks) => {
    if (!checks) return null;
    const items = [`${checks.sourcesFound} source${checks.sourcesFound === 1 ? "" : "s"} found`];
    if (mode === "model_answer") {
      items.push(
        `${checks.claimsLinkedToSource} of ${checks.claimsTotal} claims linked to a source`
      );
      items.push("answer quality not evaluated");
    } else if (mode === "source_results") {
      items.push("no written answer to check");
    }
    if (checks.promptInjectionPatternInSources) {
      items.push("a retrieved page contained instruction-like text; it was treated as data");
    }
    return el(
      "ul",
      { class: "ask-checks" },
      items.map((text) => el("li", { text }))
    );
  };

  const traceDisclosure = (steps) => {
    if (!steps.length) return null;
    const details = el("details", { class: "ask-trace" });
    details.append(
      el("summary", { text: `How this result was found (${steps.length} steps)` }),
      el(
        "ol",
        {},
        steps.map((s) =>
          el("li", {}, [
            el("span", { class: "k", text: s.label }),
            s.detail ? el("span", { class: "v", text: ` ${s.detail}` }) : null,
          ])
        )
      )
    );
    return details;
  };

  const continuation = () =>
    el("p", { class: "ask-next" }, [
      "Want something like this for your own documentation or knowledge base? ",
      el("a", { href: "/#contact", text: "Discuss your project" }),
      ".",
    ]);

  const renderAnswer = (payload, steps) => {
    const mode = payload.mode || (payload.llmUsed ? "model_answer" : "source_results");
    const sources = payload.sources || [];
    const blocks = [];

    if (payload.notice) {
      blocks.push(el("p", { class: "ask-notice", text: payload.notice }));
    }

    if (mode === "model_answer") {
      blocks.push(el("div", { class: "ask-answer" }, [el("p", { text: payload.text })]));
    } else {
      blocks.push(el("p", { class: "ask-explain", text: payload.text }));
    }

    if (sources.length) {
      blocks.push(
        el("h2", { class: "ask-sources-head", text: mode === "model_answer" ? "Sources used" : "Matching documentation" }),
        el("ol", { class: "ask-sources" }, sources.map(sourceCard))
      );
    }

    const checks = checksRow(mode, payload.checks);
    if (checks) blocks.push(checks);
    if (sources.length) blocks.push(continuation());
    const trace = traceDisclosure(steps);
    if (trace) blocks.push(trace);

    output.replaceChildren(...blocks.filter(Boolean));
  };

  const renderProblem = (heading, detail, options = {}) => {
    const blocks = [
      el("div", { class: "ask-problem" }, [
        el("p", { class: "ask-problem__head", text: heading }),
        el("p", { text: detail }),
      ]),
    ];
    if (options.retry) {
      blocks.push(
        el("p", {}, [
          el("button", {
            class: "btn",
            type: "button",
            text: "Try that question again",
            onclick: () => ask(lastQuestion),
          }),
        ])
      );
    }
    output.replaceChildren(...blocks);
  };

  async function ask(query) {
    const question = (query || "").trim();
    if (!question) return;
    lastQuestion = question;
    input.value = question; // the question stays put, whatever happens next
    results.hidden = false;
    output.replaceChildren();
    say("Searching the ServiceNow documentation…");
    setBusy(true);

    const steps = [];
    let answered = false;

    try {
      await streamResearch(question, {
        stage: (d) => {
          steps.push(d);
          if (d.key === "retrieve") say("Reading the documentation pages that matched…");
        },
        blocked: (d) => {
          answered = true;
          say("");
          renderProblem(
            "That question was turned away before any model call.",
            d.reason ||
              "It matched a pattern used to try to change the tool's instructions. Rephrase it as a plain ServiceNow question."
          );
        },
        answer: (d) => {
          answered = true;
          say("");
          renderAnswer(d, steps);
        },
        error: (d) => {
          answered = true;
          say("");
          renderProblem(
            "The search did not finish.",
            d.message || "Something failed partway through. Your question is still in the box.",
            { retry: true }
          );
        },
        rateLimited: (message) => {
          answered = true;
          say("");
          renderProblem(
            "Too many questions in a short time.",
            message || "Wait a moment and try again. Your question is still in the box.",
            { retry: true }
          );
        },
      });
      if (!answered) {
        say("");
        renderProblem(
          "The search ended without a result.",
          "Nothing came back from the run. Your question is still in the box.",
          { retry: true }
        );
      }
    } catch (err) {
      say("");
      renderProblem(
        "The search service is unreachable.",
        "The request could not be completed from here. Your question is still in the box.",
        { retry: true }
      );
    } finally {
      setBusy(false);
    }
  }

  chipsWrap?.replaceChildren(
    ...CHIPS.map((c) =>
      el("button", {
        class: "chip",
        type: "button",
        text: c,
        onclick: () => ask(c),
      })
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

  showMode();
}

function sourceLabel(source) {
  const kind =
    source.sourceType === "product_documentation"
      ? "ServiceNow product documentation"
      : source.sourceType === "sdk_explain"
        ? "ServiceNow SDK documentation"
        : source.sourceType.replace(/_/g, " ");
  return source.url ? `${kind} · ${hostOf(source.url)}` : kind;
}

function hostOf(url) {
  try {
    return new URL(url).host;
  } catch (e) {
    return "source";
  }
}

initChrome();
initAsk();
