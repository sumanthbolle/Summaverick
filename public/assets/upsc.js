import { mountChrome, apiSoft, el, fmtDate, $, showError } from "/assets/app.js";

mountChrome({ active: "tools" });

const data = await apiSoft("/api/upsc/feed");
$("loading")?.remove();
if (data.ok === false && !(data.notes || []).length) {
  showError($("error"), data);
}
const notes = data.notes || [];
const list = $("list");
if (!notes.length) {
  list.append(
    el("p", {
      class: "empty",
      text: "No published notes yet. The publisher posts here only after the evidence gate passes.",
    })
  );
} else {
  for (const n of notes) {
    const card = el("article", { class: "card" });
    const payload = n.payload || {};
    card.append(
      el("div", { class: "kicker", text: [n.paper, n.band].filter(Boolean).join(" · ") }),
      el("h3", { text: n.anchor || payload.title || "Note" }),
      el("p", { text: payload.summary || payload.headline || payload.text || "" }),
      el("p", { class: "muted", text: fmtDate(n.publishedAt) })
    );
    list.append(card);
  }
}
