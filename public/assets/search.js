import { mountChrome, api, el, articleHref, $, showError } from "/assets/app.js";

mountChrome({ active: "learn" });

const form = document.getElementById("form");
const input = document.getElementById("q");
const results = document.getElementById("results");
const params = new URLSearchParams(location.search);
if (params.get("q")) input.value = params.get("q");

async function run(q) {
  document.getElementById("error").hidden = true;
  results.textContent = "";
  const url = new URL(location.href);
  url.searchParams.set("q", q);
  history.replaceState(null, "", url);
  try {
    const data = await api("/api/search?q=" + encodeURIComponent(q));
    const hits = data.hits || [];
    if (!hits.length) {
      results.append(el("p", { class: "empty", text: "No matches." }));
      return;
    }
    for (const h of hits) {
      const a = el("a", { class: "tile", href: articleHref(h.slug) });
      a.append(
        el("div", { class: "kicker", text: h.kind }),
        el("h3", { text: h.title }),
        el("p", { text: h.snippet || "" })
      );
      results.append(a);
    }
  } catch (e) {
    showError(document.getElementById("error"), e);
  }
}

form.addEventListener("submit", (e) => {
  e.preventDefault();
  const q = input.value.trim();
  if (q.length >= 2) run(q);
});
if (input.value.trim().length >= 2) run(input.value.trim());
