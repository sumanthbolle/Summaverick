/*
 * Summaverick Research for ServiceNow. Submits to /api/research/stream and
 * renders the run as it happens:
 *
 *   stage    a step in the run panel (classify, layers, gate, write…)
 *   sources  the numbered sources, shown before the answer finishes
 *   delta    answer text as the model writes it
 *   answer   the final result, its mode, and the checks that ran
 *   done     the run id, which becomes a shareable ?run= link
 *
 * Modes, and what the page is allowed to call each one:
 *
 *   model_answer          prose from an answer model, sources underneath
 *   source_results        matching documentation pages — never called an answer
 *   insufficient_evidence no page matched closely enough to quote
 *   out_of_domain         outside the ServiceNow scope of the tool
 *
 * Checks are listed in the words of the check. Linked sources are not a quality
 * verdict, so answer quality is reported as not evaluated rather than "verified".
 */

import { el, $, prefersReducedMotion } from "./lib/dom.js";
import { streamResearch } from "./lib/agent-stream.js";
import { renderMarkdown } from "./lib/markdown.js";
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
    "Answers are written from the ServiceNow documentation retrieved for your question, and every claim is numbered to its source so you can check it.",
};

const STATE_GLYPH = { ok: "✓", block: "✕", muted: "–", active: "" };

function initResearch() {
  const form = $("[data-ask-form]");
  const input = $("[data-ask-input]");
  const chipsWrap = $("[data-ask-chips]");
  const results = $("[data-ask-results]");
  const statusLine = $("[data-ask-status]");
  const output = $("[data-ask-answer]");
  const sourcesWrap = $("[data-rs-sources]");
  const questionHead = $("[data-rs-question]");
  const runButton = $("[data-ask-run]");
  const modeLine = $("[data-ask-mode]");
  const stepsList = $("[data-rs-steps]");
  const checksList = $("[data-rs-checks]");
  const runMeta = $("[data-rs-runmeta]");
  const runBox = $("[data-rs-run]");
  if (!form || !input || !results || !output) return;

  let lastQuestion = "";
  let runId = null;
  let busy = false;

  // The run panel sits beside the answer on wide screens and below it on narrow
  // ones, where it starts collapsed so the answer comes first.
  const narrow = window.matchMedia("(max-width: 60rem)");
  const syncRunBox = () => { if (runBox) runBox.open = !narrow.matches; };
  syncRunBox();
  narrow.addEventListener?.("change", syncRunBox);

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

  const setBusy = (next) => {
    busy = next;
    if (!runButton) return;
    runButton.disabled = next;
    runButton.textContent = next ? "Researching…" : "Research";
  };

  const say = (text) => {
    if (statusLine) statusLine.textContent = text;
  };

  /* ---- Run panel ------------------------------------------------------- */

  const resetRun = () => {
    stepsList?.replaceChildren();
    checksList?.replaceChildren();
    if (runMeta) runMeta.textContent = "";
  };

  const addStep = (d) => {
    if (!stepsList) return;
    // A new step settles whichever one was still running.
    for (const li of stepsList.querySelectorAll('[data-state="active"]')) {
      li.dataset.state = "ok";
      li.querySelector(".rs-step__icon").textContent = STATE_GLYPH.ok;
    }
    const state = d.kind || "ok";
    stepsList.append(
      el("li", { class: "rs-step", dataset: { state } }, [
        el("span", { class: "rs-step__icon", "aria-hidden": "true", text: STATE_GLYPH[state] ?? "" }),
        el("span", { class: "rs-step__label", text: d.label }),
        d.detail ? el("span", { class: "rs-step__detail", text: d.detail }) : null,
      ])
    );
  };

  const settleSteps = () => {
    for (const li of stepsList?.querySelectorAll('[data-state="active"]') ?? []) {
      li.dataset.state = "ok";
      li.querySelector(".rs-step__icon").textContent = STATE_GLYPH.ok;
    }
  };

  const renderChecks = (payload) => {
    if (!checksList) return;
    const c = payload.checks;
    const mode = payload.mode;
    if (!c) return;
    const items = [];
    items.push(`${c.sourcesFound} source${c.sourcesFound === 1 ? "" : "s"} found`);
    if (mode === "model_answer") {
      items.push(`${c.answerCitations ?? 0} of ${c.sourcesFound} sources cited in the answer`);
      if (c.answerCitationsUnresolved) {
        items.push(`${c.answerCitationsUnresolved} citation number(s) match no source`);
      }
      items.push(`${c.claimsLinkedToSource} of ${c.claimsTotal} evidence claims linked to a source`);
      items.push("answer quality not evaluated");
    } else if (mode === "source_results") {
      items.push("no written answer to check");
    }
    items.push(
      c.promptInjectionPatternInSources
        ? "a retrieved page contained instruction-like text; it was treated as data"
        : "no instruction-like text in retrieved pages"
    );
    items.push("read-only: nothing was written to any instance");
    checksList.replaceChildren(...items.map((text) => el("li", { text })));
  };

  const setRunMeta = (payload) => {
    if (!runMeta) return;
    const parts = [];
    if (payload?.latencyMs) parts.push(`${(payload.latencyMs / 1000).toFixed(1)}s`);
    if (runId) parts.push(runId.replace(/^run_/, "").slice(0, 8));
    runMeta.textContent = parts.join(" · ");
  };

  /* ---- Sources --------------------------------------------------------- */

  const sourceCard = (source, index) => {
    const n = index + 1;
    const heading = source.url
      ? el("a", { class: "src-title", href: source.url, rel: "noopener", target: "_blank", text: source.title })
      : el("span", { class: "src-title", text: source.title });
    return el("li", { class: "src", id: `src-${n}` }, [
      el("span", { class: "src-index", text: String(n) }),
      el("div", {}, [
        heading,
        source.snippet ? el("p", { class: "src-snippet", text: plainText(source.snippet) }) : null,
        el("p", { class: "src-meta", text: sourceLabel(source) }),
      ]),
    ]);
  };

  let sourceCount = 0;
  const renderSources = (sources, heading) => {
    sourceCount = sources.length;
    if (!sourcesWrap) return;
    if (!sources.length) { sourcesWrap.replaceChildren(); return; }
    sourcesWrap.replaceChildren(
      el("h3", { class: "ask-sources-head", text: heading }),
      el("ol", { class: "ask-sources" }, sources.map(sourceCard))
    );
  };

  // Clicking a citation scrolls to its source and marks it briefly.
  output.addEventListener("click", (e) => {
    const a = e.target.closest?.("a.cite");
    if (!a) return;
    const target = document.getElementById(`src-${a.dataset.cite}`);
    if (!target) return;
    e.preventDefault();
    target.scrollIntoView({ behavior: prefersReducedMotion() ? "auto" : "smooth", block: "center" });
    target.classList.remove("src--flash");
    void target.offsetWidth;
    target.classList.add("src--flash");
  });

  /* ---- Answer ---------------------------------------------------------- */

  let streamed = "";
  let answerBox = null;
  let frame = 0;

  const paintStream = () => {
    frame = 0;
    if (!answerBox) return;
    answerBox.replaceChildren(renderMarkdown(streamed, { sourceCount }));
  };

  const onDelta = (d) => {
    if (!answerBox) {
      answerBox = el("div", { class: "ask-answer md is-streaming", "aria-busy": "true" });
      output.replaceChildren(answerBox);
      say("");
    }
    streamed += d.text || "";
    if (!frame) frame = requestAnimationFrame(paintStream);
  };

  const actions = (payload) => {
    const copyAnswer = el("button", {
      class: "btn btn-sm",
      type: "button",
      text: "Copy answer",
      onclick: async (e) => {
        const text = answerWithSources(payload);
        await copy(text, e.currentTarget, "Copied");
      },
    });
    const copyLink = runId
      ? el("button", {
          class: "btn btn-sm",
          type: "button",
          text: "Copy link to this run",
          onclick: async (e) => {
            await copy(runUrl(runId), e.currentTarget, "Link copied");
          },
        })
      : null;
    const again = el("button", {
      class: "btn btn-sm btn-quiet",
      type: "button",
      text: "Ask another question",
      onclick: () => {
        input.value = "";
        input.focus();
        form.scrollIntoView({ behavior: prefersReducedMotion() ? "auto" : "smooth", block: "center" });
      },
    });
    return el("div", { class: "rs-actions", "data-rs-actions": "" }, [
      payload.mode === "model_answer" ? copyAnswer : null,
      copyLink,
      again,
    ]);
  };

  let lastPayload = null;
  const renderAnswer = (payload) => {
    lastPayload = payload;
    if (frame) { cancelAnimationFrame(frame); frame = 0; }
    const mode = payload.mode || (payload.llmUsed ? "model_answer" : "source_results");
    const sources = payload.sources || [];
    renderSources(sources, mode === "model_answer" ? "Sources" : "Matching documentation");

    const blocks = [];
    if (payload.notice) blocks.push(el("p", { class: "ask-notice", text: payload.notice }));

    if (mode === "model_answer") {
      const box = el("div", { class: "ask-answer md" });
      box.append(renderMarkdown(payload.text, { sourceCount: sources.length }));
      blocks.push(box);
    } else {
      blocks.push(el("p", { class: "ask-explain", text: payload.text }));
    }
    blocks.push(actions(payload));
    if (sources.length) {
      blocks.push(
        el("p", { class: "ask-next" }, [
          "Want this on your own runbooks, knowledge base or instance? ",
          el("a", { href: "#rs-deploy", text: "See deployment options" }),
          ".",
        ])
      );
    }

    output.replaceChildren(...blocks.filter(Boolean));
    answerBox = null;
    streamed = "";
    renderChecks({ ...payload, mode });
    setRunMeta(payload);
  };

  const renderProblem = (heading, detail, options = {}) => {
    if (frame) { cancelAnimationFrame(frame); frame = 0; }
    // If the answer was partly written when the run broke off, keep what
    // arrived and say plainly that it is incomplete.
    const partial = streamed.trim()
      ? el("div", { class: "ask-answer md" }, [renderMarkdown(streamed, { sourceCount })])
      : null;
    answerBox = null;
    streamed = "";
    const blocks = [
      partial,
      partial ? el("p", { class: "ask-notice", text: "The answer above stopped before it was finished." }) : null,
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
    output.replaceChildren(...blocks.filter(Boolean));
    settleSteps();
  };

  /* ---- Run ------------------------------------------------------------- */

  const begin = (question) => {
    lastQuestion = question;
    input.value = question; // the question stays put, whatever happens next
    results.hidden = false;
    if (questionHead) questionHead.textContent = question;
    output.replaceChildren();
    sourcesWrap?.replaceChildren();
    resetRun();
    runId = null;
    sourceCount = 0;
    streamed = "";
    answerBox = null;
  };

  async function ask(query) {
    const question = (query || "").trim();
    if (!question || busy) return;
    begin(question);
    say("Classifying the question and reading the documentation…");
    setBusy(true);
    setUrl({ q: question });

    let answered = false;

    try {
      await streamResearch(question, {
        stage: (d) => {
          addStep(d);
          if (d.key === "retrieve") say("Reading the documentation pages that matched…");
          if (d.key === "compose") say("Writing the answer from these sources…");
        },
        sources: (d) => {
          renderSources(d.sources || [], d.composing ? "Sources" : "Matching documentation");
        },
        delta: onDelta,
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
          settleSteps();
          renderAnswer(d);
        },
        done: (d) => {
          if (d.id && d.status === "done") {
            runId = d.id;
            setUrl({ run: d.id });
            setRunMeta(lastPayload);
            // The copy-link button needs the id, which arrives after the answer.
            const bar = output.querySelector("[data-rs-actions]");
            if (bar && lastPayload) bar.replaceWith(actions(lastPayload));
          }
        },
        error: (d) => {
          answered = true;
          say("");
          renderProblem(
            "The research run did not finish.",
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
          "The run ended without a result.",
          "Nothing came back from the run. Your question is still in the box.",
          { retry: true }
        );
      }
    } catch (err) {
      say("");
      renderProblem(
        "The research service is unreachable.",
        "The request could not be completed from here. Your question is still in the box.",
        { retry: true }
      );
    } finally {
      setBusy(false);
    }
  }

  /* A shared ?run= link replays a saved run without asking again. */
  async function loadRun(id) {
    try {
      const res = await fetch(`/api/research/${encodeURIComponent(id)}`);
      if (!res.ok) throw new Error(String(res.status));
      const data = await res.json();
      const payload = data.trace?.result;
      const question = data.run?.query;
      if (!payload || !question) throw new Error("incomplete run");
      begin(question);
      runId = id;
      addStep({ label: "saved run", detail: new Date(Number(data.run.created_at)).toLocaleString(), kind: "ok" });
      renderAnswer(payload);
    } catch (e) {
      begin("");
      if (questionHead) questionHead.textContent = "";
      renderProblem(
        "That run could not be loaded.",
        "The link may be old, or the run was not saved. Ask the question again below."
      );
      setUrl({});
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
    if (e.key === "Enter" && !e.shiftKey && !e.isComposing) {
      e.preventDefault();
      ask(input.value);
    }
  });
  // "/" focuses the question box from anywhere that is not already a field.
  document.addEventListener("keydown", (e) => {
    if (e.key !== "/" || e.metaKey || e.ctrlKey || e.altKey) return;
    const t = e.target;
    if (t instanceof HTMLElement && (t.isContentEditable || /^(INPUT|TEXTAREA|SELECT)$/.test(t.tagName))) return;
    e.preventDefault();
    input.focus();
  });

  showMode();
  loadEvals();

  const params = new URLSearchParams(location.search);
  if (params.get("run")) loadRun(params.get("run"));
  else if (params.get("q")) input.value = params.get("q").slice(0, 500);
}

/* ---- Eval scoreboard ---------------------------------------------------- */

async function loadEvals() {
  const box = $("[data-rs-evals]");
  if (!box) return;
  try {
    const res = await fetch("/api/research/evals");
    if (!res.ok) return;
    const s = await res.json();
    const set = (key, text) => {
      const node = box.querySelector(`[data-eval="${key}"]`);
      if (node) node.textContent = text;
    };
    const regressionTotal = s.total - s.adversarialTotal;
    set("passRate", pct(s.passRate));
    set("passedOf", `${s.passed} of ${s.total} cases, including ${regressionTotal} regression questions`);
    set("adversarialBlockRate", pct(s.adversarialBlockRate));
    set("adversarialOf", `${s.adversarialTotal} adversarial prompts`);
    set("intentAccuracy", pct(s.intentAccuracy));
    set("intentOf", `${s.intentEvaluated} questions with a labelled intent`);
  } catch (e) {
    /* Leave the placeholders; the section still explains what is measured. */
  }
}

/* ---- Helpers ------------------------------------------------------------ */

function pct(x) {
  const n = Number(x);
  if (!Number.isFinite(n)) return "—";
  return `${Math.round((n <= 1 ? n * 100 : n) * 10) / 10}%`;
}

function setUrl(params) {
  const url = new URL(location.href);
  url.search = "";
  for (const [k, v] of Object.entries(params)) url.searchParams.set(k, v);
  history.replaceState(null, "", url);
}

function runUrl(id) {
  const url = new URL("/research", location.origin);
  url.searchParams.set("run", id);
  return url.toString();
}

function answerWithSources(payload) {
  const lines = [payload.text || ""];
  const sources = payload.sources || [];
  if (sources.length) {
    lines.push("", "Sources:");
    sources.forEach((s, i) => lines.push(`[${i + 1}] ${s.title}${s.url ? ` — ${s.url}` : ""}`));
  }
  return lines.join("\n");
}

async function copy(text, button, done) {
  const label = button.textContent;
  try {
    await navigator.clipboard.writeText(text);
    button.textContent = done;
  } catch (e) {
    button.textContent = "Copy failed";
  }
  setTimeout(() => { button.textContent = label; }, 1800);
}

/* Excerpts are documentation text; drop Markdown emphasis marks from them. */
function plainText(text) {
  return String(text).replace(/\*\*|__|`/g, "");
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
initResearch();
