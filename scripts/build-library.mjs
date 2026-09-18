/**
 * Static library builder.
 *
 * Reads scripts/data/posts.json (52 essays, copied from the portfolio site)
 * and scripts/data/interviews.json (57 tech interviews) and pre-renders the
 * Writing section into public/:
 *
 *   public/learn.html            essay index with search + topic filter
 *   public/interviews.html       interview index with search + topic filter
 *   public/article/<slug>.html   one full page per essay and interview
 *   public/sitemap.xml           every public URL, articles included
 *   public/robots.txt            allow-all + sitemap pointer
 *
 * Slugs follow scripts/seed-content.ts exactly (uniqueId minus trailing
 * timestamp for posts, slugified question for interviews, -<id> on clash),
 * so the static files and the /api/content rows address the same slugs and
 * the Worker can serve the static file first and fall back to the API.
 *
 * Run:  node scripts/build-library.mjs   (or pnpm run build:library)
 * Re-run after editing scripts/data/*.json and commit the result — deploys
 * ship public/ as-is, so generated files are checked in.
 */
import { mkdirSync, readFileSync, writeFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const HERE = dirname(fileURLToPath(import.meta.url));
const ROOT = join(HERE, "..");
const DATA = join(HERE, "data");
const ARTICLE_DIR = join(ROOT, "public", "article");
const ORIGIN = "https://summaverick.com";

// --- slug scheme (mirrors seed-content.ts) --------------------------------
function slugify(s) {
  return String(s ?? "")
    .toLowerCase()
    .replace(/[^a-z0-9]+/g, "-")
    .replace(/^-+|-+$/g, "")
    .slice(0, 80)
    .replace(/-+$/g, "");
}

function postSlug(p) {
  // uniqueIds predate the site; flatten stray slashes so every slug is one
  // path segment, then strip the trailing timestamp like the seed script.
  const flat = String(p.uniqueId ?? "").replace(/\//g, "-");
  if (flat) return flat.replace(/-\d{10,}$/, "");
  return slugify(p.title);
}

// --- helpers ---------------------------------------------------------------
function esc(s) {
  return String(s ?? "")
    .replace(/&/g, "&amp;")
    .replace(/</g, "&lt;")
    .replace(/>/g, "&gt;")
    .replace(/"/g, "&quot;");
}

function stripTags(html) {
  return String(html ?? "")
    .replace(/<script[\s\S]*?<\/script>/gi, " ")
    .replace(/<style[\s\S]*?<\/style>/gi, " ")
    .replace(/<[^>]+>/g, " ")
    .replace(/&nbsp;/g, " ")
    .replace(/&amp;/g, "&")
    .replace(/&lt;/g, "<")
    .replace(/&gt;/g, ">")
    .replace(/\s+/g, " ")
    .trim();
}

/** Bodies are our own published prose; still, no scripts or handlers ship. */
function sanitizeBody(html) {
  return String(html ?? "")
    .replace(/<script[\s\S]*?<\/script>/gi, "")
    .replace(/<(iframe|object|embed|link|meta)\b[^>]*>(?:[\s\S]*?<\/\1>)?/gi, "")
    .replace(/\son\w+\s*=\s*("[^"]*"|'[^']*'|[^\s>]+)/gi, "")
    .replace(/(href|src)\s*=\s*("?)\s*javascript:[^"'>\s]*/gi, '$1=$2#');
}

function fmtDate(iso) {
  if (!iso) return "";
  const d = new Date(iso);
  if (Number.isNaN(d.getTime())) return "";
  return d.toLocaleDateString("en-US", { month: "short", year: "numeric" });
}

function metaLine(parts) {
  return parts.filter(Boolean).join(" · ");
}

/** Cut prose to length without breaking mid-word. */
function snip(s, max = 200) {
  const t = stripTags(s).trim();
  if (t.length <= max) return t;
  const cut = t.lastIndexOf(" ", max);
  return (cut > max * 0.6 ? t.slice(0, cut) : t.slice(0, max)).trimEnd() + "…";
}

// --- shared chrome (matches index.html / ask.html) --------------------------
function head({ title, description, canonical }) {
  return `<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8" />
  <meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover" />
  <title>${esc(title)}</title>
  <meta name="description" content="${esc(description)}" />
  <link rel="canonical" href="${esc(canonical)}" />
  <link rel="icon" href="/assets/favicon.ico" sizes="any" />
  <link rel="icon" type="image/png" sizes="32x32" href="/assets/favicon-32x32.png" />
  <link rel="icon" type="image/png" sizes="16x16" href="/assets/favicon-16x16.png" />
  <link rel="apple-touch-icon" href="/assets/apple-touch-icon.png" />
  <meta property="og:title" content="${esc(title)}" />
  <meta property="og:description" content="${esc(description)}" />
  <meta property="og:url" content="${esc(canonical)}" />
  <meta property="og:type" content="article" />
  <meta name="twitter:card" content="summary" />
  <link rel="preload" href="/assets/fonts/geist-latin.woff2" as="font" type="font/woff2" crossorigin />
  <link rel="stylesheet" href="/assets/css/tokens.css" />
  <link rel="stylesheet" href="/assets/css/type.css" />
  <link rel="stylesheet" href="/assets/css/site.css" />
</head>`;
}

function header(active) {
  const link = (href, id, label) =>
    `<a href="${href}"${active === id ? ' aria-current="page"' : ""}>${label}</a>`;
  return `<body>
  <a class="skip" href="#main">Skip to content</a>

  <header class="nav" id="nav">
    <a class="nav-brand" href="/" aria-label="Summaverick — home">
      <img src="/assets/img/summaverick-uncontained-sum.svg" alt="" width="30" height="30" aria-hidden="true" />
      <span>Summaverick</span>
    </a>
    <nav class="nav-links" id="nav-links" aria-label="Primary">
      ${link("/#services", "", "What we build")}
      ${link("/#work", "", "Work")}
      ${link("/#company", "", "Team")}
      ${link("/learn", "learn", "Resources")}
    </nav>
    <div class="nav-actions">
      <a class="btn btn-primary" href="/#contact">Discuss your project</a>
      <button class="nav-toggle" id="nav-toggle" type="button" aria-expanded="false" aria-controls="nav-panel">
        <span class="nav-toggle-bars" aria-hidden="true"></span><span class="visually-hidden">Menu</span>
      </button>
    </div>
  </header>

  <div class="nav-panel" id="nav-panel" hidden>
    <nav aria-label="Mobile">
      <a href="/#services">What we build</a>
      <a href="/#work">Work</a>
      <a href="/#expertise">How delivery works</a>
      <a href="/#company">Team</a>
      <a href="/learn">Writing library</a>
      <a href="/interviews">Interview questions</a>
      <a href="/ask">Research agent</a>
      <a href="/#contact">Discuss your project</a>
    </nav>
  </div>`;
}

function footer() {
  return `  <footer class="footer"><div class="container footer-cols"><div class="footer-brand"><div class="footer-brand__mark"><img src="/assets/img/summaverick-uncontained-sum.svg" alt="" width="38" height="38" aria-hidden="true" /><span>Summaverick</span></div><p>ServiceNow applications, business-system integrations, and AI tools people actually use.</p></div><div><p class="footer-head">Company</p><ul><li><a href="/#services">What we build</a></li><li><a href="/#work">Work</a></li><li><a href="/#expertise">How delivery works</a></li><li><a href="/#company">Team</a></li></ul></div><div><p class="footer-head">Try</p><ul><li><a href="/ask">Research agent</a></li><li><a href="/quiz">ServiceNow quiz</a></li><li><a href="/tools">Tools</a></li></ul></div><div><p class="footer-head">Resources</p><ul><li><a href="/learn">Writing library</a></li><li><a href="/interviews">Interview questions</a></li><li><a href="/#contact">Discuss your project</a></li><li><a href="/signin">Sign in</a></li></ul><p class="small text-mute footer-copy">© <span data-year></span> Summaverick</p></div></div></footer>

  <script type="module" src="/assets/js/boot.js"></script>
</body>
</html>`;
}

// --- listing pages -----------------------------------------------------------
const FILTER_SCRIPT = `<script>
(function () {
  var input = document.querySelector("[data-filter-search]");
  var pills = Array.prototype.slice.call(document.querySelectorAll("[data-filter-pill]"));
  var cards = Array.prototype.slice.call(document.querySelectorAll("[data-card]"));
  var count = document.querySelector("[data-filter-count]");
  var active = "all";
  function apply() {
    var q = (input.value || "").toLowerCase().trim();
    var n = 0;
    cards.forEach(function (card) {
      var okCat = active === "all" || card.getAttribute("data-cat") === active;
      var okQ = !q || (card.getAttribute("data-search") || "").indexOf(q) !== -1;
      var show = okCat && okQ;
      card.hidden = !show;
      if (show) n += 1;
    });
    var total = cards.length;
    count.textContent = n === total
      ? "Showing all " + total + " pieces."
      : "Showing " + n + " of " + total + " pieces.";
    document.querySelector("[data-empty]").hidden = n !== 0;
  }
  pills.forEach(function (pill) {
    pill.addEventListener("click", function () {
      active = pill.getAttribute("data-filter-pill");
      pills.forEach(function (p) { p.setAttribute("aria-pressed", String(p === pill)); });
      apply();
    });
  });
  input.addEventListener("input", apply);
  apply();
})();
</script>`;

function card(item) {
  const search = [item.title, item.excerpt, item.categoryLabel, item.meta]
    .filter(Boolean)
    .join(" ")
    .toLowerCase()
    .replace(/"/g, "");
  return `      <a class="lib-card" data-card data-cat="${esc(item.category)}" data-search="${esc(search)}" href="/article/${esc(item.slug)}">
        <p class="studio-kicker" style="margin:0">${esc(item.kicker)}</p>
        <h2>${esc(item.title)}</h2>
        <p>${esc(item.excerpt)}</p>
        <p class="lib-meta">${esc(item.meta)}</p>
      </a>`;
}

function listingPage({ kicker, title, lead, stats, placeholder, filters, cards, canonical, description, crumb }) {
  const pills = [`<button class="chip" type="button" data-filter-pill="all" aria-pressed="true">Everything</button>`]
    .concat(filters.map((f) => `<button class="chip" type="button" data-filter-pill="${esc(f.value)}" aria-pressed="false">${esc(f.label)}</button>`))
    .join("\n          ");
  return `${head({ title, description, canonical })}
${header("learn")}
  <main id="main" tabindex="-1">
    <section class="library-hero" aria-labelledby="library-title">
      <div class="container">
        <p class="studio-kicker">${esc(kicker)}</p>
        <h1 id="library-title">${esc(title)}</h1>
        <p class="lead">${esc(lead)}</p>
        <div class="library-stats">
${stats.map((s) => `          <div><strong>${esc(s.n)}</strong><span>${esc(s.label)}</span></div>`).join("\n")}
        </div>
      </div>
    </section>
    <div class="library-toolbar">
      <div class="container">
        <div class="library-toolbar__row">
          <div class="library-search"><input type="search" data-filter-search placeholder="${esc(placeholder)}" aria-label="Search this collection" /></div>
          <div class="library-filters">
          ${pills}
          </div>
        </div>
      </div>
    </div>
    <div class="container">
      <p class="library-count" data-filter-count role="status"></p>
      <div class="lib-grid">
${cards.map(card).join("\n")}
      </div>
      <p class="lib-empty" data-empty hidden>Nothing matches that search. Try fewer words, or a different topic.</p>
      <p class="lib-empty">${esc(crumb)}</p>
    </div>
  </main>
${footer()}
${FILTER_SCRIPT}`;
}

// --- article pages -------------------------------------------------------------
const PROGRESS_SCRIPT = `<script>
(function () {
  var bar = document.querySelector("[data-progress]");
  if (!bar) return;
  var tick = function () {
    var h = document.documentElement;
    var max = h.scrollHeight - h.clientHeight;
    var p = max > 0 ? Math.min(1, Math.max(0, h.scrollTop / max)) : 0;
    bar.style.transform = "scaleX(" + p + ")";
  };
  document.addEventListener("scroll", tick, { passive: true });
  tick();
})();
</script>`;

function articlePage({ item, prev, next }) {
  const jsonLd = {
    "@context": "https://schema.org",
    "@type": "Article",
    headline: item.title,
    description: item.excerpt,
    datePublished: item.dateISO || undefined,
    author: { "@type": "Organization", name: "Summaverick", url: ORIGIN },
  };
  const pager = (rel, other, caption) =>
    other
      ? `<a href="/article/${esc(other.slug)}" rel="${rel}"><small>${caption}</small><strong>${esc(other.title)}</strong></a>`
      : `<a href="${item.kind === "interview" ? "/interviews" : "/learn"}"><small>${caption}</small><strong>Back to the collection</strong></a>`;
  return `${head({
    title: `${item.title} — Summaverick`,
    description: item.excerpt,
    canonical: `${ORIGIN}/article/${item.slug}`,
  })}
${header("learn")}
  <main id="main" tabindex="-1">
    <div class="reading-progress" aria-hidden="true"><span data-progress></span></div>
    <div class="container article-wrap">
      <article class="article-inner">
        <p><a class="article-back" href="${item.kind === "interview" ? "/interviews" : "/learn"}">‹ ${item.kind === "interview" ? "All interview questions" : "All essays"}</a></p>
        <p class="kicker">${esc(item.kicker)}</p>
        <h1>${esc(item.title)}</h1>
        <p class="article-meta">${esc(item.meta)}</p>
        <div class="prose">
${item.body}
        </div>
        <nav class="article-pager" aria-label="More in this collection">
          ${pager("prev", prev, "‹ Previous")}
          ${pager("next", next, "Next ›")}
        </nav>
      </article>
    </div>
  </main>
${footer()}
  <script type="application/ld+json">
${JSON.stringify(jsonLd, null, 2)}
  </script>
${PROGRESS_SCRIPT}`;
}

// --- sitemap + robots ------------------------------------------------------------
function sitemapXml(urls) {
  const today = new Date().toISOString().slice(0, 10);
  return `<?xml version="1.0" encoding="UTF-8"?>\n<urlset xmlns="http://www.sitemaps.org/schemas/sitemap/0.9">\n${urls
    .map((u) => `  <url><loc>${ORIGIN}${u}</loc><lastmod>${today}</lastmod></url>`)
    .join("\n")}\n</urlset>\n`;
}

const ROBOTS = `# summaverick.com
User-agent: *
Allow: /

Sitemap: ${ORIGIN}/sitemap.xml
`;

// --- main --------------------------------------------------------------------------
function main() {
  const posts = JSON.parse(readFileSync(join(DATA, "posts.json"), "utf8"));
  const interviews = JSON.parse(readFileSync(join(DATA, "interviews.json"), "utf8"));

  const usedSlugs = new Set();
  const uniqueSlug = (base, id) => {
    let slug = base || `item-${id}`;
    if (usedSlugs.has(slug)) slug = `${slug}-${id}`;
    usedSlugs.add(slug);
    return slug;
  };

  const essays = posts.map((p) => {
    const numId = String(p.id);
    const excerpt = p.excerpt || snip(p.content);
    return {
      kind: "post",
      numId,
      slug: uniqueSlug(postSlug(p), numId),
      title: p.title,
      kicker: label(p.category) || "Essay",
      category: p.category || "essay",
      categoryLabel: label(p.category),
      excerpt,
      meta: metaLine([p.difficulty, p.readTime, fmtDate(p.dateISO)]),
      body: sanitizeBody(p.content ?? ""),
      dateISO: p.dateISO || "",
    };
  });

  const qas = interviews.map((iv) => {
    const numId = String(iv.id);
    return {
      kind: "interview",
      numId,
      slug: uniqueSlug(slugify(iv.question), numId),
      title: iv.question,
      kicker: iv.category || "Interview",
      category: iv.category || "interview",
      categoryLabel: iv.category || "Interview",
      excerpt: snip(iv.answer ?? ""),
      meta: metaLine([iv.difficulty, iv.company, fmtDate(iv.dateISO)]),
      body: sanitizeBody(iv.answer ?? ""),
      dateISO: iv.dateISO || "",
    };
  });

  mkdirSync(ARTICLE_DIR, { recursive: true });
  const writeArticle = (item, i, arr) => {
    const prev = i > 0 ? arr[i - 1] : null;
    const next = i < arr.length - 1 ? arr[i + 1] : null;
    writeFileSync(join(ARTICLE_DIR, `${item.slug}.html`), articlePage({ item, prev, next }), "utf8");
  };
  essays.forEach(writeArticle);
  qas.forEach(writeArticle);

  const topicCount = (arr) => new Set(arr.map((i) => i.category)).size;
  const essayFilters = [...new Set(essays.map((e) => e.category))].map((c) => ({
    value: c,
    label: label(c),
  }));
  const qaFilters = [...new Set(qas.map((q) => q.category))].map((c) => ({ value: c, label: c }));

  writeFileSync(
    join(ROOT, "public", "learn.html"),
    listingPage({
      kicker: "Writing · Essays",
      title: "Essays and guides.",
      lead: "Longer pieces on ServiceNow, AI systems, architecture, and the career around them. Everything below is on this page — search it or filter by topic.",
      stats: [
        { n: String(essays.length), label: "Essays and guides" },
        { n: String(topicCount(essays)), label: "Topics" },
        { n: "57", label: "Interview questions" },
      ],
      placeholder: "Search essays — try “Fluent” or “CSDM”…",
      filters: essayFilters,
      cards: essays,
      canonical: `${ORIGIN}/learn`,
      description: "Essays and guides on ServiceNow, AI systems, architecture, and career — the Summaverick writing library.",
      crumb: "Looking for interview prep? There are 57 answered questions next door.",
    }),
    "utf8"
  );

  writeFileSync(
    join(ROOT, "public", "interviews.html"),
    listingPage({
      kicker: "Writing · Interviews",
      title: "Interview questions, answered.",
      lead: "Real ServiceNow interview questions with senior-level answers — what the interviewer is listening for, and the detail that shows you have done the work.",
      stats: [
        { n: String(qas.length), label: "Answered questions" },
        { n: String(topicCount(qas)), label: "Topics" },
        { n: "52", label: "Essays and guides" },
      ],
      placeholder: "Search questions — try “ACL” or “GlideRecord”…",
      filters: qaFilters,
      cards: qas,
      canonical: `${ORIGIN}/interviews`,
      description: "ServiceNow interview questions with senior-level answers — the Summaverick interview library.",
      crumb: "Want the longer version? The essay library holds 52 pieces behind these answers.",
    }),
    "utf8"
  );

  const urls = ["/", "/ask", "/learn", "/interviews", "/quiz", "/tools", "/search", "/flights", "/metals", "/upsc", "/advocate", "/signin"]
    .concat(essays.map((e) => `/article/${e.slug}`))
    .concat(qas.map((q) => `/article/${q.slug}`));
  writeFileSync(join(ROOT, "public", "sitemap.xml"), sitemapXml(urls), "utf8");
  writeFileSync(join(ROOT, "public", "robots.txt"), ROBOTS, "utf8");

  console.log(`library: ${essays.length} essays + ${qas.length} interviews -> public/article/*.html (${essays.length + qas.length} pages)`);
  console.log(`library: learn.html, interviews.html, sitemap.xml (${urls.length} urls), robots.txt`);
}

function label(category) {
  const map = { servicenow: "ServiceNow", ai: "AI", career: "Career", architecture: "Architecture" };
  return map[category] || category;
}

main();
