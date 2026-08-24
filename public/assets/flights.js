import { mountChrome, el, $, showError, setHidden } from "/assets/app.js";

mountChrome({ active: "tools" });

const form = document.getElementById("searchForm");
const dep = document.getElementById("departureDate");
const today = new Date().toISOString().slice(0, 10);
dep.min = today;
dep.value = today;

form.addEventListener("submit", async (e) => {
  e.preventDefault();
  const error = $("error");
  const loading = $("loading");
  const results = $("results");
  error.hidden = true;
  results.textContent = "";
  setHidden(loading, false);
  const body = {
    origin: $("origin").value.trim(),
    destination: $("destination").value.trim(),
    departureDate: $("departureDate").value,
    returnDate: $("returnDate").value,
    tripType: $("returnDate").value ? "round-trip" : "one-way",
    cabinClass: $("cabinClass").value,
    priorityMode: $("priorityMode").value,
    passengers: 1,
  };
  try {
    const res = await fetch("/api/flights", {
      method: "POST",
      headers: { "content-type": "application/json" },
      credentials: "same-origin",
      body: JSON.stringify(body),
    });
    const data = await res.json().catch(() => ({}));
    if (!res.ok || data.success === false) {
      throw new Error(data.error || data.message || "Search failed");
    }
    render(data.data || data);
  } catch (err) {
    showError(error, err);
  } finally {
    setHidden(loading, true);
  }
});

function render(data) {
  const root = $("results");
  const summary = data.search_summary || {};
  const rec = data.recommendation || {};
  const advice = data.booking_advice;
  root.append(
    el("p", {
      class: "muted",
      text: `${summary.origin || ""} → ${summary.destination || ""} · ${summary.cabin_class || ""} · ${data.source || ""}`,
    })
  );
  if (rec.explanation) root.append(el("div", { class: "card", text: rec.explanation }));
  if (advice && (advice.headline || advice.summary)) {
    root.append(el("div", { class: "card", text: advice.headline || advice.summary }));
  }
  const list = el("div", { class: "grid", attrs: { style: "margin-top:16px" } });
  for (const f of data.flights || []) {
    const card = el("article", { class: "card flight" });
    const price = f.price || f.total || f.amount;
    const fare = typeof price === "object" ? (price.total || price.amount) : price;
    const cur = (typeof price === "object" && price.currency) || summary.currency || "";
    card.append(
      el("div", { class: "fare", text: fare != null ? `${cur} ${fare}` : "Fare on request" }),
      el("div", { text: f.airline || f.carrier || f.validatingAirline || "Flight" }),
      el("p", { class: "muted", text: f.duration || f.totalDuration || f.route || "" })
    );
    if (f.booking_url) {
      const a = el("a", { class: "btn", href: f.booking_url, text: "Open booking" });
      a.rel = "noopener noreferrer";
      a.target = "_blank";
      card.append(a);
    }
    list.append(card);
  }
  if (!(data.flights || []).length) {
    root.append(el("p", { class: "empty", text: "No flights returned for that search." }));
  } else {
    root.append(list);
  }
}
