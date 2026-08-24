import { mountChrome, api, fmtDate, $, showError, setHidden } from "/assets/app.js";

mountChrome({ active: "learn" });

function slugFromLocation() {
  const p = location.pathname;
  if (p.startsWith("/article/") && p.length > "/article/".length) {
    return decodeURIComponent(p.slice("/article/".length).replace(/\/+$/, ""));
  }
  return new URLSearchParams(location.search).get("slug") || "";
}

function sanitize(html) {
  const doc = new DOMParser().parseFromString(html || "", "text/html");
  doc.querySelectorAll("script, iframe, object, embed, link, meta").forEach((n) => n.remove());
  for (const n of doc.querySelectorAll("*")) {
    for (const attr of [...n.attributes]) {
      const name = attr.name.toLowerCase();
      const val = attr.value;
      if (name.startsWith("on")) n.removeAttribute(attr.name);
      if ((name === "href" || name === "src") && /^\s*javascript:/i.test(val)) n.removeAttribute(attr.name);
    }
  }
  return doc.body;
}

const slug = slugFromLocation();
if (!slug) {
  setHidden($("loading"), true);
  showError($("error"), { message: "Missing article." });
} else {
  api("/api/content/" + encodeURIComponent(slug))
    .then((data) => {
      const it = data.item;
      if (!it) throw new Error("Not found");
      document.title = it.title + " — summaverick";
      $("kicker").textContent = it.kind === "interview" ? "Interview" : it.category || "Essay";
      $("title").textContent = it.title;
      $("meta").textContent = [it.difficulty, it.company, it.readTime, fmtDate(it.publishedAt)]
        .filter(Boolean)
        .join(" · ");
      $("backLink").href = it.kind === "interview" ? "/interviews" : "/learn";
      $("backLink").textContent = it.kind === "interview" ? "Interviews" : "Library";
      const body = $("body");
      body.textContent = "";
      body.append(...sanitize(it.body || "").childNodes);
      setHidden($("loading"), true);
      setHidden($("article"), false);
    })
    .catch((e) => {
      setHidden($("loading"), true);
      showError($("error"), e);
    });
}
