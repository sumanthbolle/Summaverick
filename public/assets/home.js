import { mountChrome, apiSoft, el, articleHref, fmtDate, $ } from "/assets/app.js";

mountChrome({ active: "home" });

const PRODUCTS = [
  { href: "/quiz", kicker: "Practice", title: "ServiceNow quiz", body: "1,300+ questions. Answers never ship to the browser.", meta: "CSA · Scripting · GRC" },
  { href: "/research", kicker: "Agent", title: "Research", body: "Three-layer retrieval with a visible reasoning trace.", meta: "Evidence-gated" },
  { href: "/learn", kicker: "Essays", title: "Library", body: "Guides on Fluent, AI agents, ITSM, and platform architecture.", meta: "52 posts" },
  { href: "/interviews", kicker: "Prep", title: "Interviews", body: "Senior-level answers you can actually say out loud.", meta: "57 questions" },
  { href: "/flights", kicker: "SkyFare", title: "Flights", body: "Search inventory, then get booking timing in plain language.", meta: "Amadeus + AI" },
  { href: "/metals", kicker: "Markets", title: "Gold & silver", body: "Live spot with a quiet 30-day history.", meta: "XAU · XAG" },
  { href: "/upsc", kicker: "Notes", title: "UPSC feed", body: "Source-backed daily notes. Unsupported facts never publish.", meta: "Evidence gate" },
  { href: "/advocate", kicker: "Advocacy", title: "Summaverick agent", body: "Describe a missing order. Watch it negotiate a refund.", meta: "Live demo" },
];

const grid = $("productGrid");
for (const p of PRODUCTS) {
  const a = el("a", { class: "tile", href: p.href });
  a.append(
    el("div", { class: "kicker", text: p.kicker }),
    el("h3", { text: p.title }),
    el("p", { text: p.body }),
    el("div", { class: "meta", text: p.meta })
  );
  grid.append(a);
}

async function loadPosts() {
  const box = $("postGrid");
  const data = await apiSoft("/api/content?kind=post");
  const items = data.items || [];
  $("postsLoading")?.remove();
  if (!items.length) {
    box.append(el("p", { class: "empty", text: data.message || "The library will appear here once content is published." }));
    return;
  }
  for (const it of items.slice(0, 4)) {
    const a = el("a", { class: "tile", href: articleHref(it.slug) });
    a.append(
      el("div", { class: "kicker", text: it.category || "Essay" }),
      el("h3", { text: it.title }),
      el("p", { text: it.excerpt || "" }),
      el("div", { class: "meta", text: [it.readTime, fmtDate(it.publishedAt)].filter(Boolean).join(" · ") })
    );
    box.append(a);
  }
}

async function loadPulse() {
  const box = $("pulseGrid");
  const data = await apiSoft("/api/trending?country=IN");
  $("pulseStatus")?.remove();
  const widgets = data.widgets || {};
  const cards = [widgets.news, widgets.worldNews, widgets.market, widgets.tech].filter(Boolean);
  if (!cards.length) {
    box.append(el("p", { class: "empty", text: "Live headlines are unavailable right now. Quiz, research, and the advocacy demo still work." }));
    return;
  }
  for (const w of cards) {
    const a = el(w.url ? "a" : "div", { class: "tile", href: w.url || undefined });
    if (w.url) a.setAttribute("rel", "noopener noreferrer");
    if (w.url) a.setAttribute("target", "_blank");
    a.append(
      el("div", { class: "kicker", text: w.label || w.kind || "Pulse" }),
      el("h3", { text: w.title || w.headline || "Update" }),
      el("p", { text: w.summary || w.source || "" })
    );
    box.append(a);
  }
}

loadPosts();
loadPulse();
