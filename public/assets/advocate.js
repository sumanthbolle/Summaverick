import { mountChrome, $ } from "/assets/app.js";

mountChrome({ active: "tools" });

const STAGES = ["Intake", "Policy", "Drafting", "Engaging", "Escalating", "Resolved"];
const chat = $("chat");
const banner = $("banner");
const timelineEl = $("timeline");
let reached = new Set();
let source = null;

function renderTimeline(active) {
  timelineEl.textContent = "";
  for (const s of STAGES) {
    const d = document.createElement("div");
    d.className = "stage" + (reached.has(s) ? " done" : "") + (s === active ? " active" : "");
    d.setAttribute("role", "listitem");
    d.textContent = s;
    timelineEl.append(d);
  }
}

function addMsg(who, text) {
  const sys = chat.querySelector(".system");
  if (sys) chat.textContent = "";
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
  chat.scrollTop = chat.scrollHeight;
}

$("start").addEventListener("click", async () => {
  const scenario = $("scenario").value;
  const btn = $("start");
  btn.disabled = true;
  chat.textContent = "";
  banner.className = "banner";
  reached = new Set();
  renderTimeline("Intake");
  if (source) source.close();

  source = new EventSource(`/api/advocate/demo/stream?scenario=${encodeURIComponent(scenario)}`);
  source.addEventListener("stage", (e) => {
    const stage = JSON.parse(e.data).stage;
    if (stage && stage !== "Escalating") reached.add(stage);
    renderTimeline(stage);
  });
  source.addEventListener("agent_sent", (e) => addMsg("agent", JSON.parse(e.data).message));
  source.addEventListener("bot_replied", (e) => addMsg("bot", JSON.parse(e.data).message));
  source.addEventListener("channel_opened", () => addMsg("system", "Connected to support channel"));
  source.addEventListener("escalating", (e) => {
    reached.add("Engaging");
    renderTimeline("Escalating");
    const d = JSON.parse(e.data);
    addMsg("system", `Escalating (tone: ${d.tone || "firm"})`);
  });
  source.addEventListener("resolved", (e) => {
    ["Engaging", "Escalating", "Resolved"].forEach((s) => reached.add(s));
    renderTimeline("Resolved");
    const amt = JSON.parse(e.data).refund_amount;
    banner.textContent = `Resolved — ₹${amt} refunded, zero user intervention`;
    banner.className = "banner show";
  });
  source.addEventListener("needs_human", () => addMsg("system", "Handed off to a human representative"));
  source.addEventListener("done", () => {
    source.close();
    btn.disabled = false;
  });
  source.onerror = () => {
    source.close();
    btn.disabled = false;
    addMsg("system", "The demo stream closed. Try again.");
  };
});

renderTimeline("");
