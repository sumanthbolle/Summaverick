import { mountChrome, apiSoft, el, articleHref, fmtDate, $, showError } from "/assets/app.js";

const kind = location.pathname.includes("interview") ? "interview" : "post";
mountChrome({ active: "learn" });

let page = 0;
const list = $("list");
const more = $("more");

function card(it) {
  const a = el("a", { class: "tile", href: articleHref(it.slug) });
  a.append(
    el("div", { class: "kicker", text: it.company || it.category || (kind === "interview" ? "Interview" : "Essay") }),
    el("h3", { text: it.title }),
    el("p", { text: it.excerpt || "" }),
    el("div", {
      class: "meta",
      text: [it.difficulty, it.readTime, fmtDate(it.publishedAt)].filter(Boolean).join(" · "),
    })
  );
  return a;
}

async function load() {
  more.disabled = true;
  const data = await apiSoft(`/api/content?kind=${kind}&page=${page}`);
  $("loading")?.remove();
  if (data.ok === false && !(data.items || []).length) {
    showError($("error"), data);
    more.hidden = true;
    return;
  }
  const items = data.items || [];
  if (!items.length && page === 0) {
    list.append(el("p", { class: "empty", text: "Nothing published in this collection yet." }));
    more.hidden = true;
    return;
  }
  for (const it of items) list.append(card(it));
  const full = items.length >= (data.pageSize || 20);
  more.hidden = !full;
  more.disabled = false;
  page += 1;
}

more.addEventListener("click", load);
load();
