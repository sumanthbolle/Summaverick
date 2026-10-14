import { initChrome } from "/assets/js/chrome.js";

initChrome();

const $ = (id) => document.getElementById(id);
const STAGES = ["Intake", "Policy", "Drafting", "Engaging", "Escalating", "Resolved"];
const chat = $("chat");
const banner = $("banner");
const timelineEl = $("timeline");
const reduceMotion = window.matchMedia("(prefers-reduced-motion: reduce)");
const CASE_FIELDS = ["order", "issue", "amount", "policy", "tone"];
const title = (word) => word.charAt(0).toUpperCase() + word.slice(1);
let reached = new Set();
let escalated = false;
let source = null;

function renderTimeline(active) {
  timelineEl.textContent = "";
  for (const s of STAGES) {
    const li = document.createElement("li");
    const skipped = s === "Escalating" && reached.has("Resolved") && !escalated;
    li.className =
      "adv-stage" +
      (reached.has(s) ? " is-done" : "") +
      (skipped ? " is-skipped" : "") +
      (s === active ? " is-active" : "");
    li.textContent = skipped ? `${s} (not needed)` : s;
    if (s === active) li.setAttribute("aria-current", "step");
    timelineEl.append(li);
  }
}

function setCase(field, value) {
  const dd = $(`case-${field}`);
  dd.textContent = value;
  dd.classList.add("is-set");
}

function addMsg(who, text) {
  chat.querySelector("[data-placeholder]")?.remove();
  const wrap = document.createElement("div");
  wrap.className = `adv-msg adv-msg--${who === "agent" ? "agent" : who === "bot" ? "bot" : "system"}`;
  if (who === "system") {
    wrap.textContent = text;
  } else {
    const label = document.createElement("p");
    label.className = "adv-msg__who";
    label.textContent = who === "agent" ? "Summaverick agent" : "Support bot";
    const body = document.createElement("p");
    body.textContent = text;
    wrap.append(label, body);
  }
  chat.append(wrap);
  // The chat grows with the page rather than scrolling itself, so on a phone
  // the newest message would otherwise land below the fold.
  wrap.scrollIntoView({ block: "nearest", behavior: reduceMotion.matches ? "auto" : "smooth" });
}

$("start").addEventListener("click", () => {
  const scenario = $("scenario").value;
  const btn = $("start");
  btn.disabled = true;
  $("scenario").disabled = true;
  chat.textContent = "";
  banner.className = "adv-banner";
  banner.textContent = "";
  for (const field of CASE_FIELDS) {
    $(`case-${field}`).textContent = "—";
    $(`case-${field}`).classList.remove("is-set");
  }
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
  source.addEventListener("intake", (e) => {
    const d = JSON.parse(e.data);
    setCase("order", d.orderId);
    setCase("issue", d.issueType === "missing_item" ? "Missing item" : d.issueType);
    setCase("amount", `₹${d.amount}`);
  });
  source.addEventListener("policy", (e) => {
    const d = JSON.parse(e.data);
    setCase("policy", d.eligible ? `Eligible. ${d.notes}` : `Not eligible. ${d.notes}`);
  });
  source.addEventListener("draft", (e) => setCase("tone", title(JSON.parse(e.data).tone || "polite")));
  source.addEventListener("agent_sent", (e) => addMsg("agent", JSON.parse(e.data).message));
  source.addEventListener("bot_replied", (e) => addMsg("bot", JSON.parse(e.data).message));
  source.addEventListener("channel_opened", () => addMsg("system", "Connected to the support chat"));
  source.addEventListener("escalating", (e) => {
    escalated = true;
    reached.add("Engaging");
    renderTimeline("Escalating");
    const tone = JSON.parse(e.data).tone || "firm";
    setCase("tone", title(tone));
    addMsg("system", `Refused, so the agent escalates (tone: ${tone})`);
  });
  source.addEventListener("resolved", (e) => {
    ["Engaging", "Resolved"].forEach((s) => reached.add(s));
    if (escalated) reached.add("Escalating");
    renderTimeline("Resolved");
    const amt = JSON.parse(e.data).refund_amount;
    banner.textContent = `Resolved: the mock support bot approved a ₹${amt} refund without the customer stepping in.`;
    banner.className = "adv-banner is-shown";
  });
  source.addEventListener("needs_human", () => addMsg("system", "Handed off to a human representative"));
  source.addEventListener("done", finish);
  source.onerror = () => {
    finish();
    addMsg("system", "The demo stream closed. Try again.");
  };
});

renderTimeline("");
