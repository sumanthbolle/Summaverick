import { mountChrome, el, $, showError, setHidden } from "/assets/app.js";

mountChrome({ active: "tools" });

function spark(history, color) {
  const pts = (history || [])
    .map((p) => Number(p.usdPerOunce))
    .filter(Number.isFinite);
  const svg = document.createElementNS("http://www.w3.org/2000/svg", "svg");
  svg.setAttribute("class", "spark");
  svg.setAttribute("viewBox", "0 0 200 72");
  svg.setAttribute("preserveAspectRatio", "none");
  svg.setAttribute("aria-hidden", "true");
  if (pts.length < 2) return svg;
  const min = Math.min(...pts);
  const max = Math.max(...pts);
  const span = max - min || 1;
  const d = pts
    .map((v, i) => {
      const x = (i / (pts.length - 1)) * 200;
      const y = 68 - ((v - min) / span) * 60;
      return `${i === 0 ? "M" : "L"}${x.toFixed(1)} ${y.toFixed(1)}`;
    })
    .join(" ");
  const path = document.createElementNS("http://www.w3.org/2000/svg", "path");
  path.setAttribute("d", d);
  path.setAttribute("fill", "none");
  path.setAttribute("stroke", color);
  path.setAttribute("stroke-width", "2");
  path.setAttribute("stroke-linejoin", "round");
  svg.append(path);
  return svg;
}

async function load() {
  const cur = $("currency").value;
  setHidden($("loading"), false);
  $("error").hidden = true;
  $("cards").textContent = "";
  try {
    const res = await fetch("/api/metals?currency=" + encodeURIComponent(cur), {
      credentials: "same-origin",
    });
    const data = await res.json().catch(() => ({}));
    if (!res.ok || data.success === false) {
      throw new Error(data.error || data.message || "Prices unavailable");
    }
    const fx = data.conversionAvailable ? data.fxRate : 1;
    const unit = data.currency || "USD";
    const gold = data.metals?.gold;
    const silver = data.metals?.silver;
    const paint = (metal, name, color) => {
      const card = el("article", { class: "card" });
      const usd = Number(metal.usdPerOunce);
      const shown = usd * (Number(fx) || 1);
      card.append(
        el("div", { class: "kicker", text: metal.symbol }),
        el("h2", { text: name }),
        el("div", { class: "price", text: `${unit} ${shown.toLocaleString(undefined, { maximumFractionDigits: 2 })}` }),
        el("p", { class: "muted", text: "per troy ounce" }),
        spark(metal.history, color)
      );
      return card;
    };
    if (gold) $("cards").append(paint(gold, "Gold", "#c9a227"));
    if (silver) $("cards").append(paint(silver, "Silver", "#8e8e93"));
    $("src").textContent = [
      data.freshness === "live" ? "Live" : "Delayed",
      data.sources?.live?.name,
      data.sources?.historyAndFx?.name,
    ]
      .filter(Boolean)
      .join(" · ");
  } catch (e) {
    showError($("error"), e);
  } finally {
    setHidden($("loading"), true);
  }
}

$("currency").addEventListener("change", load);
load();
