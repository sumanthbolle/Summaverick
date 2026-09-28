/*
 * Summaverick Research for ServiceNow — a chat assistant over the ServiceNow
 * documentation. Each question streams from /api/research/stream:
 *
 *   stage    a research step (classify, layers, gate, write…)
 *   sources  the numbered sources, shown before the answer finishes
 *   delta    answer text as the model writes it
 *   answer   the final result, its mode, and the checks that ran
 *   done     the run id, which makes the answer shareable as ?run=<id>
 *
 * Follow-ups send the earlier turns as `history`, so the thread reads like any
 * chat assistant while every answer is still written only from the sources
 * retrieved for it.
 *
 * Modes, and what the page is allowed to call each one:
 *
 *   model_answer          prose from an answer model, sources above it
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

const SUGGESTIONS = [
  { title: "Query the incident table", q: "How do I use GlideRecord to query the incident table?" },
  { title: "Business Rules in Fluent", q: "How do I define a Business Rule with the Fluent SDK?" },
  { title: "CMDB relationships", q: "What is the CMDB and how do CI relationships work?" },
  { title: "How ACLs evaluate", q: "How do ACLs evaluate on a table?" },
];

const MODE_TEXT = {
  source_results:
    "This deployment returns matching ServiceNow documentation pages with an excerpt and a link. It does not write answers.",
  model_answer:
    "Answers are written from the ServiceNow documentation retrieved for each question, and every claim is numbered to its source.",
};

const STORE_KEY = "sv-research-chat";
const STATE_GLYPH = { ok: "✓", block: "✕", muted: "–", active: "" };

function initResearch() {
  const root = $("[data-chat]");
  const form = $("[data-ask-form]");
  const input = $("[data-ask-input]");
  const sendButton = $("[data-ask-run]");
  const thread = $("[data-chat-thread]");
  const bar = $("[data-chat-bar]");
  const suggest = $("[data-ask-chips]");
  const modeLine = $("[data-ask-mode]");
  if (!root || !form || !input || !thread) return;

  /** @type {{question: string, payload: any, runId: string|null, steps: any[]}[]} */
  let turns = [];
  let controller = null;
  // Bumped by "New chat", so a run still finishing from the old thread does not
  // touch the new one.
  let generation = 0;

  /* ---- Layout: start screen vs conversation ------------------------------ */

  const setChatting = (on) => {
    document.body.classList.toggle("is-chatting", on);
    thread.hidden = !on;
    if (bar) bar.hidden = !on;
    input.placeholder = on ? "Ask a follow-up…" : "Ask anything about ServiceNow…";
  };

  const setBusy = (busy) => {
    root.dataset.busy = busy ? "true" : "false";
    if (sendButton) {
      sendButton.setAttribute("aria-label", busy ? "Stop" : "Send");
      sendButton.dataset.mode = busy ? "stop" : "send";
    }
  };

  // The box grows with the question, up to a limit.
  const autosize = () => {
    input.style.height = "auto";
    input.style.height = `${Math.min(input.scrollHeight, 200)}px`;
  };
  input.addEventListener("input", autosize);

  /* ---- Persistence (this tab only) --------------------------------------- */

  const save = () => {
    try {
      sessionStorage.setItem(
        STORE_KEY,
        JSON.stringify(turns.filter((t) => t.payload).slice(-12))
      );
    } catch (e) {}
  };
  const restore = () => {
    try {
      const raw = sessionStorage.getItem(STORE_KEY);
      return raw ? JSON.parse(raw) : [];
    } catch (e) {
      return [];
    }
  };

  /* ---- Scrolling ---------------------------------------------------------- */

  const nearBottom = () =>
    window.innerHeight + window.scrollY >= document.documentElement.scrollHeight - 160;
  const scrollToEnd = () =>
    window.scrollTo({ top: document.documentElement.scrollHeight, behavior: "auto" });

  /* ---- One turn ------------------------------------------------------------ */

  function buildTurn(index, question) {
    const prefix = `t${index}-src-`;
    const steps = el("ol", { class: "ai-steps__list" });
    const stepsSummary = el("span", { class: "ai-steps__label", text: "Researching…" });
    const stepsBox = el("details", { class: "ai-steps" }, [
      el("summary", {}, [el("span", { class: "ai-steps__icon", "aria-hidden": "true" }), stepsSummary]),
      steps,
    ]);
    const sources = el("div", { class: "ai-sources" });
    const answer = el("div", { class: "ai-answer md" });
    const notice = el("p", { class: "ai-notice", hidden: "" });
    const foot = el("div", { class: "ai-foot" });
    const checks = el("ul", { class: "ai-checks" });
    const node = el("article", { class: "turn", dataset: { turn: String(index) } }, [
      el("div", { class: "turn-user" }, [el("p", { class: "bubble", text: question })]),
      el("div", { class: "turn-ai" }, [
        el("div", { class: "ai-head" }, [
          el("img", { class: "brand-mark", src: "/assets/img/summaverick-mark.png", width: "24", height: "24", alt: "" }),
          el("span", { text: "Summaverick" }),
        ]),
        stepsBox,
        sources,
        notice,
        answer,
        foot,
        checks,
      ]),
    ]);

    // Citations jump to their source card and mark it.
    answer.addEventListener("click", (e) => {
      const a = e.target.closest?.("a.cite");
      if (!a) return;
      const card = node.querySelector(`#${prefix}${a.dataset.cite}`);
      if (!card) return;
      e.preventDefault();
      card.scrollIntoView({ behavior: prefersReducedMotion() ? "auto" : "smooth", block: "nearest", inline: "center" });
      card.classList.remove("src-card--flash");
      void card.offsetWidth;
      card.classList.add("src-card--flash");
    });

    return { node, prefix, steps, stepsBox, stepsSummary, sources, answer, notice, foot, checks };
  }

  function addStep(view, d) {
    for (const li of view.steps.querySelectorAll('[data-state="active"]')) {
      li.dataset.state = "ok";
      li.firstChild.textContent = STATE_GLYPH.ok;
    }
    const state = d.kind || "ok";
    view.steps.append(
      el("li", { class: "ai-step", dataset: { state } }, [
        el("span", { class: "ai-step__icon", "aria-hidden": "true", text: STATE_GLYPH[state] ?? "" }),
        el("span", { class: "ai-step__label", text: d.label }),
        d.detail ? el("span", { class: "ai-step__detail", text: d.detail }) : null,
      ])
    );
    if (state === "active") view.stepsSummary.textContent = `${capitalize(d.label)}…`;
  }

  function finishSteps(view, label) {
    for (const li of view.steps.querySelectorAll('[data-state="active"]')) {
      li.dataset.state = "ok";
      li.firstChild.textContent = STATE_GLYPH.ok;
    }
    view.stepsBox.dataset.done = "true";
    view.stepsSummary.textContent = label;
  }

  function renderSourceCards(view, list) {
    if (!list.length) { view.sources.replaceChildren(); return; }
    view.sources.replaceChildren(
      el("p", { class: "ai-label", text: `Sources · ${list.length}` }),
      el(
        "ol",
        { class: "src-cards" },
        list.map((s, i) => {
          const n = i + 1;
          const inner = [
            el("span", { class: "src-card__meta" }, [
              el("span", { class: "src-card__n", text: String(n) }),
              el("span", { text: hostOf(s.url) || kindOf(s) }),
            ]),
            el("span", { class: "src-card__title", text: s.title }),
          ];
          const body = s.url
            ? el("a", { class: "src-card__link", href: s.url, rel: "noopener", target: "_blank", title: plainText(s.snippet || s.title) }, inner)
            : el("div", { class: "src-card__link", title: plainText(s.snippet || s.title) }, inner);
          return el("li", { class: "src-card", id: `${view.prefix}${n}` }, [body]);
        })
      )
    );
  }

  function renderDocList(view, list) {
    // For documentation-only results the pages are the result, so show them
    // in full, with their excerpts, in place of an answer.
    view.answer.classList.remove("md");
    view.answer.replaceChildren(
      el(
        "ol",
        { class: "ask-sources" },
        list.map((s, i) =>
          el("li", { class: "src" }, [
            el("span", { class: "src-index", text: String(i + 1) }),
            el("div", {}, [
              s.url
                ? el("a", { class: "src-title", href: s.url, rel: "noopener", target: "_blank", text: s.title })
                : el("span", { class: "src-title", text: s.title }),
              s.snippet ? el("p", { class: "src-snippet", text: plainText(s.snippet) }) : null,
              el("p", { class: "src-meta", text: `${kindOf(s)}${s.url ? ` · ${hostOf(s.url)}` : ""}` }),
            ]),
          ])
        )
      )
    );
  }

  function checksFor(payload) {
    const c = payload.checks;
    if (!c) return [];
    const items = [`${c.sourcesFound} source${c.sourcesFound === 1 ? "" : "s"} found`];
    if (payload.mode === "model_answer") {
      items.push(`${c.answerCitations ?? 0} of ${c.sourcesFound} cited in the answer`);
      if (c.answerCitationsUnresolved) items.push(`${c.answerCitationsUnresolved} citation number(s) match no source`);
      items.push("answer quality not evaluated");
    } else if (payload.mode === "source_results") {
      items.push("no written answer to check");
    }
    if (c.promptInjectionPatternInSources) items.push("instruction-like text in a source was treated as data");
    items.push("read-only");
    return items;
  }

  function renderFinal(view, turn) {
    const payload = turn.payload;
    const mode = payload.mode || (payload.llmUsed ? "model_answer" : "source_results");
    payload.mode = mode;
    const sources = payload.sources || [];

    renderSourceCards(view, mode === "model_answer" ? sources : []);
    if (payload.notice) {
      view.notice.hidden = false;
      view.notice.textContent = payload.notice;
    }

    view.answer.classList.remove("is-streaming");
    view.answer.removeAttribute("aria-busy");
    if (mode === "model_answer") {
      view.answer.classList.add("md");
      view.answer.replaceChildren(renderMarkdown(payload.text, { sourceCount: sources.length, idPrefix: view.prefix }));
    } else if (mode === "source_results" && sources.length) {
      const lead = el("p", { class: "ai-explain", text: payload.text });
      renderDocList(view, sources);
      view.answer.prepend(lead);
    } else {
      view.answer.replaceChildren(el("p", { class: "ai-explain", text: payload.text }));
    }

    const stepCount = view.steps.children.length;
    const stepText = stepCount ? ` · ${stepCount} steps` : "";
    finishSteps(
      view,
      sources.length
        ? `Researched ${sources.length} source${sources.length === 1 ? "" : "s"}${stepText}`
        : `Checked the question${stepText}`
    );
    view.stepsBox.hidden = !stepCount;
    view.checks.replaceChildren(...checksFor(payload).map((t) => el("li", { text: t })));
    view.foot.replaceChildren(actionsFor(turn, mode));
  }

  function actionsFor(turn, mode) {
    const isLast = turns[turns.length - 1] === turn;
    const buttons = [];
    if (mode === "model_answer") {
      buttons.push(iconButton("Copy", ICON.copy, async (btn) => copy(answerWithSources(turn.payload), btn, "Copied")));
    }
    if (turn.runId) {
      buttons.push(iconButton("Share", ICON.link, async (btn) => copy(runUrl(turn.runId), btn, "Link copied")));
    }
    if (isLast) {
      buttons.push(iconButton("Retry", ICON.retry, () => retryLast()));
    }
    const meta = turn.payload?.latencyMs ? el("span", { class: "ai-time", text: `${(turn.payload.latencyMs / 1000).toFixed(1)}s` }) : null;
    return el("div", { class: "ai-actions" }, [...buttons, meta]);
  }

  function refreshActions() {
    thread.querySelectorAll(".turn").forEach((node, i) => {
      const turn = turns[i];
      const foot = node.querySelector(".ai-foot");
      if (!foot) return;
      if (turn?.payload) foot.replaceChildren(actionsFor(turn, turn.payload.mode));
      else if (i < turns.length - 1) foot.replaceChildren(); // only the latest failed turn keeps Retry
    });
  }

  function renderProblem(view, heading, detail, partialText) {
    view.answer.classList.remove("is-streaming");
    view.answer.removeAttribute("aria-busy");
    const blocks = [];
    if (partialText && partialText.trim()) {
      const kept = el("div", { class: "md" }, [renderMarkdown(partialText, { sourceCount: view.sources.querySelectorAll(".src-card").length, idPrefix: view.prefix })]);
      blocks.push(kept, el("p", { class: "ai-notice", text: "This answer stopped before it was finished." }));
    }
    blocks.push(
      el("div", { class: "ask-problem" }, [
        el("p", { class: "ask-problem__head", text: heading }),
        el("p", { text: detail }),
      ])
    );
    view.answer.replaceChildren(...blocks);
    finishSteps(view, "Stopped");
  }

  /* ---- Asking ------------------------------------------------------------- */

  function historyBefore(index) {
    const out = [];
    for (const t of turns.slice(0, index)) {
      if (!t.payload) continue;
      out.push({ role: "user", content: t.question });
      out.push({ role: "assistant", content: t.payload.text || "" });
    }
    return out.slice(-6);
  }

  async function ask(raw) {
    const question = (raw || "").trim();
    if (!question) return;
    if (controller) return;

    setChatting(true);
    input.value = "";
    autosize();

    const index = turns.length;
    const turn = { question, payload: null, runId: null };
    turns.push(turn);
    const view = buildTurn(index, question);
    thread.append(view.node);
    refreshActions();
    view.node.scrollIntoView({ behavior: prefersReducedMotion() ? "auto" : "smooth", block: "start" });

    controller = new AbortController();
    const gen = generation;
    setBusy(true);

    let streamed = "";
    let frame = 0;
    let settled = false;
    let sourceCount = 0;

    const paint = () => {
      frame = 0;
      const follow = nearBottom();
      view.answer.replaceChildren(renderMarkdown(streamed, { sourceCount, idPrefix: view.prefix }));
      if (follow) scrollToEnd();
    };

    try {
      await streamResearch(
        question,
        {
          stage: (d) => addStep(view, d),
          sources: (d) => {
            sourceCount = (d.sources || []).length;
            if (d.composing) renderSourceCards(view, d.sources || []);
          },
          delta: (d) => {
            if (!streamed) {
              view.answer.classList.add("md", "is-streaming");
              view.answer.setAttribute("aria-busy", "true");
            }
            streamed += d.text || "";
            if (!frame) frame = requestAnimationFrame(paint);
          },
          blocked: (d) => {
            settled = true;
            renderProblem(
              view,
              "That question was turned away before any model call.",
              d.reason || "It matched a pattern used to try to change the tool's instructions. Rephrase it as a plain ServiceNow question."
            );
          },
          answer: (d) => {
            settled = true;
            if (frame) { cancelAnimationFrame(frame); frame = 0; }
            turn.payload = d;
            renderFinal(view, turn);
            save();
          },
          done: (d) => {
            if (d.id && d.status === "done" && turn.payload) {
              turn.runId = d.id;
              refreshActions();
              save();
            }
          },
          error: (d) => {
            settled = true;
            renderProblem(view, "The research run did not finish.", d.message || "Something failed partway through. Try again.", streamed);
          },
          rateLimited: (message) => {
            settled = true;
            renderProblem(view, "Too many questions in a short time.", message || "Wait a moment and try again.");
          },
        },
        { history: historyBefore(index), signal: controller.signal }
      );
      if (!settled) {
        renderProblem(view, "The run ended without a result.", "Nothing came back from the run. Try the question again.", streamed);
      }
    } catch (err) {
      if (frame) { cancelAnimationFrame(frame); frame = 0; }
      if (err?.name === "AbortError") {
        renderProblem(view, "Stopped.", "You stopped this answer. Ask again or change the question.", streamed);
      } else {
        renderProblem(view, "The research service is unreachable.", "The request could not be completed from here. Try again in a moment.", streamed);
      }
    } finally {
      if (gen === generation) {
        controller = null;
        setBusy(false);
        if (!turn.payload) {
          // Put the question back and offer a retry on the failed turn.
          input.value = input.value || question;
          autosize();
          view.foot.replaceChildren(el("div", { class: "ai-actions" }, [iconButton("Retry", ICON.retry, () => retryLast())]));
        }
        input.focus({ preventScroll: true });
      }
    }
  }

  function retryLast() {
    if (controller || !turns.length) return;
    const last = turns.pop();
    thread.lastElementChild?.remove();
    save();
    input.value = "";
    ask(last.question);
  }

  function newChat() {
    generation++;
    if (controller) controller.abort();
    controller = null;
    setBusy(false);
    turns = [];
    thread.replaceChildren();
    save();
    setChatting(false);
    history.replaceState(null, "", location.pathname);
    window.scrollTo({ top: 0 });
    input.focus();
  }

  /* A shared ?run= link opens that answer as a one-turn conversation. */
  async function loadRun(id) {
    try {
      const res = await fetch(`/api/research/${encodeURIComponent(id)}`);
      if (!res.ok) throw new Error(String(res.status));
      const data = await res.json();
      const payload = data.trace?.result;
      const question = data.run?.query;
      if (!payload || !question) throw new Error("incomplete run");
      showSaved([{ question, payload, runId: id }]);
    } catch (e) {
      setChatting(true);
      const view = buildTurn(0, "Shared answer");
      thread.append(view.node);
      renderProblem(view, "That shared answer could not be loaded.", "The link may be old, or the run was not saved. Ask the question again below.");
    }
  }

  function showSaved(saved) {
    turns = saved;
    setChatting(true);
    saved.forEach((turn, i) => {
      const view = buildTurn(i, turn.question);
      thread.append(view.node);
      renderFinal(view, turn);
    });
    refreshActions();
    save();
    scrollToEnd();
  }

  /* ---- Wiring ------------------------------------------------------------- */

  suggest?.replaceChildren(
    ...SUGGESTIONS.map((s) =>
      el("button", { class: "suggest", type: "button", onclick: () => ask(s.q) }, [
        el("span", { class: "suggest__title", text: s.title }),
        el("span", { class: "suggest__q", text: s.q }),
      ])
    )
  );

  form.addEventListener("submit", (e) => {
    e.preventDefault();
    if (controller) { controller.abort(); return; }
    ask(input.value);
  });
  input.addEventListener("keydown", (e) => {
    if (e.key === "Enter" && !e.shiftKey && !e.isComposing) {
      e.preventDefault();
      if (!controller) ask(input.value);
    }
  });
  $("[data-chat-new]")?.addEventListener("click", newChat);
  document.addEventListener("keydown", (e) => {
    if (e.key !== "/" || e.metaKey || e.ctrlKey || e.altKey) return;
    const t = e.target;
    if (t instanceof HTMLElement && (t.isContentEditable || /^(INPUT|TEXTAREA|SELECT)$/.test(t.tagName))) return;
    e.preventDefault();
    input.focus();
  });

  (async () => {
    if (!modeLine) return;
    try {
      const res = await fetch("/api/research/mode");
      if (!res.ok) return;
      const text = MODE_TEXT[(await res.json()).mode];
      if (text) modeLine.textContent = text;
    } catch (e) {}
  })();
  loadEvals();
  setBusy(false);

  const params = new URLSearchParams(location.search);
  if (params.get("run")) loadRun(params.get("run"));
  else if (params.get("q")) {
    input.value = params.get("q").slice(0, 500);
    autosize();
  } else {
    const saved = restore();
    if (saved.length) showSaved(saved);
  }
}

/* ---- Eval scoreboard (start screen) --------------------------------------- */

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
  } catch (e) {}
}

/* ---- Helpers ---------------------------------------------------------------- */

const ICON = {
  copy: "M9 9h10v10H9zM5 15V5h10",
  link: "M10 14a4 4 0 0 0 5.66 0l3-3a4 4 0 0 0-5.66-5.66l-1 1M14 10a4 4 0 0 0-5.66 0l-3 3a4 4 0 0 0 5.66 5.66l1-1",
  retry: "M4 4v6h6M20 20v-6h-6M5.5 15a7 7 0 0 0 12.3 2M18.5 9A7 7 0 0 0 6.2 7",
};

function iconButton(label, path, onClick) {
  const svg = document.createElementNS("http://www.w3.org/2000/svg", "svg");
  svg.setAttribute("viewBox", "0 0 24 24");
  svg.setAttribute("width", "16");
  svg.setAttribute("height", "16");
  svg.setAttribute("fill", "none");
  svg.setAttribute("stroke", "currentColor");
  svg.setAttribute("stroke-width", "1.8");
  svg.setAttribute("stroke-linecap", "round");
  svg.setAttribute("stroke-linejoin", "round");
  svg.setAttribute("aria-hidden", "true");
  const p = document.createElementNS("http://www.w3.org/2000/svg", "path");
  p.setAttribute("d", path);
  svg.append(p);
  const text = el("span", { text: label });
  const btn = el("button", { class: "ai-action", type: "button" }, [svg, text]);
  btn.addEventListener("click", () => onClick(text));
  return btn;
}

function pct(x) {
  const n = Number(x);
  if (!Number.isFinite(n)) return "—";
  return `${Math.round((n <= 1 ? n * 100 : n) * 10) / 10}%`;
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

async function copy(text, label, done) {
  const before = label.textContent;
  try {
    await navigator.clipboard.writeText(text);
    label.textContent = done;
  } catch (e) {
    label.textContent = "Copy failed";
  }
  setTimeout(() => { label.textContent = before; }, 1800);
}

/* Excerpts are documentation text; drop Markdown emphasis marks from them. */
function plainText(text) {
  return String(text || "").replace(/\*\*|__|`/g, "");
}

function capitalize(s) {
  return s ? s[0].toUpperCase() + s.slice(1) : s;
}

function kindOf(source) {
  return source.sourceType === "product_documentation"
    ? "ServiceNow product documentation"
    : source.sourceType === "sdk_explain"
      ? "ServiceNow SDK documentation"
      : String(source.sourceType || "source").replace(/_/g, " ");
}

function hostOf(url) {
  if (!url) return "";
  try {
    return new URL(url).host.replace(/^www\./, "");
  } catch (e) {
    return "";
  }
}

initChrome();
initResearch();
