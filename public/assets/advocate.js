import { mountChrome, $ } from "/assets/app.js";

mountChrome({ active: "tools" });

const STAGES = ["Intake", "Policy", "Drafting", "Engaging", "Escalating", "Resolved"];
const chat = $("chat");
const banner = $("banner");
const timelineEl = $("timeline");
const reduceMotion = window.matchMedia("(prefers-reduced-motion: reduce)");
let reached = new Set();
let escalated = false;
let source = null;

function renderTimeline(active) {
  timelineEl.textContent = "";
  for (const s of STAGES) {
    const d = document.createElement("div");
    const skipped = s === "Escalating" && reached.has("Resolved") && !escalated;
    d.className =
      "stage" +
      (reached.has(s) ? " done" : "") +
      (skipped ? " skipped" : "") +
      (s === active ? " active" : "");
    d.setAttribute("role", "listitem");
    d.textContent = skipped ? `${s} (not needed)` : s;
    if (s === active) d.setAttribute("aria-current", "step");
    timelineEl.append(d);
  }
}

function addMsg(who, text) {
  chat.querySelector("[data-placeholder]")?.remove();
  const wrap = document.createElement("div");
  wrap.className = "msg " + (who === "agent" ? "agent" : who === "bot" ? "bot" : "system");
  if (who === "system") {
    wrap.textContent = text;
  } else {
    const label = document.createElement("div");
    label.className = "who";
    label.textContent = who === "agent" ? "Summaverick" : "Support bot";
    const body = document.createElement("div");
    body.textContent = text;
    wrap.append(label, body);
  }
  chat.append(wrap);
  // The chat grows with the page rather than scrolling itself, so on a phone
  // the newest message would otherwise land below the fold.
  wrap.scrollIntoView({ block: "nearest", behavior: reduceMotion.matches ? "auto" : "smooth" });
}

$("start").addEventListener("click", async () => {
  const scenario = $("scenario").value;
  const btn = $("start");
  btn.disabled = true;
  $("scenario").disabled = true;
  chat.textContent = "";
  banner.className = "banner";
  banner.textContent = "";
  reached = new Set();
  escalated = false;
  renderTimeline("Intake");
  if (source) source.close();

  const finish = () => {
    source.close();
    btn.disabled = false;
    btn.textContent = "Run it again";
    $("scenario").disabled = false;
  };

  source = new EventSource(`/api/advocate/demo/stream?scenario=${encodeURIComponent(scenario)}`);
  source.addEventListener("stage", (e) => {
    const stage = JSON.parse(e.data).stage;
    if (stage === "Escalating") escalated = true;
    else if (stage) reached.add(stage);
    renderTimeline(stage);
  });
  source.addEventListener("agent_sent", (e) => addMsg("agent", JSON.parse(e.data).message));
  source.addEventListener("bot_replied", (e) => addMsg("bot", JSON.parse(e.data).message));
  source.addEventListener("channel_opened", () => addMsg("system", "Connected to support channel"));
  source.addEventListener("escalating", (e) => {
    escalated = true;
    reached.add("Engaging");
    renderTimeline("Escalating");
    const d = JSON.parse(e.data);
    addMsg("system", `Escalating (tone: ${d.tone || "firm"})`);
  });
  source.addEventListener("resolved", (e) => {
    ["Engaging", "Resolved"].forEach((s) => reached.add(s));
    if (escalated) reached.add("Escalating");
    renderTimeline("Resolved");
    const amt = JSON.parse(e.data).refund_amount;
    banner.textContent = `Resolved — the mock support bot approved a ₹${amt} refund without the customer stepping in.`;
    banner.className = "banner show";
  });
  source.addEventListener("needs_human", () => addMsg("system", "Handed off to a human representative"));
  source.addEventListener("done", finish);
  source.onerror = () => {
    finish();
    addMsg("system", "The demo stream closed. Try again.");
  };
});

renderTimeline("");
