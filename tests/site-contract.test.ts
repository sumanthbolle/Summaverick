import { createHash } from "node:crypto";
import { existsSync, readFileSync } from "node:fs";
import { resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { describe, expect, it } from "vitest";

const root = fileURLToPath(new URL("../", import.meta.url));
const text = (path: string) => readFileSync(resolve(root, path), "utf8");
const sha256 = (value: string) => createHash("sha256").update(value).digest("hex");

describe("migrated library source", () => {
  it("keeps the exact committed post and interview snapshots", () => {
    const posts = text("scripts/data/posts.json");
    const interviews = text("scripts/data/interviews.json");

    expect(JSON.parse(posts)).toHaveLength(52);
    expect(JSON.parse(interviews)).toHaveLength(57);
    expect(sha256(posts)).toBe("7c4de8c1949f041e367b8786b2fabaeec6b7368b916e2b344f37deefc92ef6c1");
    expect(sha256(interviews)).toBe("61d36961009076a88021849668fe99c7302afe1b911cca774e1c340ff53c28b5");
  });
});

describe("Homepage public contract", () => {
  it("ships the approved brand assets and accessible motion fallback", () => {
    const home = text("public/index.html");
    const css = text("public/assets/css/site.css");

    // The mark is inline SVG so it takes the theme's ink in both appearances.
    expect(home).toContain('class="brand-mark"');
    expect(home).not.toContain("summaverick-uncontained-sum.svg");
    expect(home).not.toContain("sumanth-reveal-v1.png");
    expect(css).toContain("@media (prefers-reduced-motion: reduce)");
  });

  it("points every icon slot at the current mark", () => {
    for (const page of ["public/index.html", "public/research.html", "public/learn.html", "public/quiz.html"]) {
      const html = text(page);
      expect(html, `${page} favicon`).toContain('href="/assets/favicon.svg"');
      expect(html, `${page} apple touch icon`).toContain('href="/assets/apple-touch-icon.png"');
      expect(html, `${page} manifest`).toContain('href="/assets/site.webmanifest"');
    }
    for (const asset of [
      "public/assets/favicon.svg",
      "public/assets/favicon.ico",
      "public/assets/apple-touch-icon.png",
      "public/assets/android-chrome-192x192.png",
      "public/assets/android-chrome-512x512.png",
      "public/assets/android-chrome-maskable-512x512.png",
      "public/assets/site.webmanifest",
      "public/assets/img/summaverick-mark.png",
      "public/assets/img/summaverick-logo.png",
      "public/assets/img/summaverick-group-logo.webp",
    ]) {
      expect(existsSync(resolve(root, asset)), asset).toBe(true);
    }
    // The tab icon is the silhouette, never the wordmark composition.
    expect(text("public/assets/favicon.svg")).not.toContain("Summaverick Group");
  });

  it("ships favicon.ico at the three sizes docs/brand.md promises", () => {
    // Pillow silently drops any requested size larger than the source image,
    // so assert the directory rather than trusting the generator.
    const ico = readFileSync(resolve(root, "public/assets/favicon.ico"));
    const count = ico.readUInt16LE(4);
    const sizes = Array.from({ length: count }, (_, i) => ico[6 + 16 * i] || 256);

    expect(sizes.sort((a, b) => a - b)).toEqual([16, 32, 48]);
  });

  it("gives the maskable icon its own padded file", () => {
    const manifest = JSON.parse(text("public/assets/site.webmanifest"));
    const maskable = manifest.icons.filter((icon: { purpose?: string }) =>
      (icon.purpose ?? "").split(" ").includes("maskable"));

    expect(maskable).toHaveLength(1);
    // A launcher may clip the rounded tile to a circle, which cuts the S's
    // tails, so the maskable entry must not reuse the plain icon.
    expect(maskable[0].src).toBe("/assets/android-chrome-maskable-512x512.png");
    for (const icon of manifest.icons) {
      expect(existsSync(resolve(root, `public${icon.src}`)), icon.src).toBe(true);
    }
  });

  it("states the offer, both audiences and mobile in the first screen", () => {
    const home = text("public/index.html");

    const hero = home.slice(home.indexOf('id="top"'), home.indexOf('id="rag"'));
    expect(hero).toContain("A software studio for AI agents.");
    // Both audiences, and mobile, are named before the first scroll.
    expect(hero).toContain("a personal assistant on someone's");
    expect(hero).toContain("phone, a mobile experience");
    expect(hero).toContain("an enterprise agent wired into ServiceNow");
    expect(hero).toContain(">Build with us<");
    expect(hero).toContain('href="#rag">Explore RAG assistants<');
    expect(home).toContain("Personal assistants and enterprise agents");
    // The hero must not depend on the illustration or on any animation.
    expect(hero).toContain("<h1");
    expect(hero).not.toContain("data-reveal");
  });

  it("alternates the headline between Super Intelligence and the brand, resting on the brand", () => {
    const home = text("public/index.html");
    const css = text("public/assets/css/site.css");
    const boot = text("public/assets/js/boot.js");
    const cycle = text("public/assets/js/hero-headline.js");

    const h1 = home.slice(home.indexOf("<h1"), home.indexOf("</h1>"));
    expect(h1).toContain("Meet");
    expect(h1).toContain('class="supermark__word text-spectrum">Super<');
    expect(h1).toContain("Intelligence");
    expect(h1).toContain("Summaverick");
    // The spectrum face is decorative; assistive tech reads "Meet Summaverick."
    expect(h1).toMatch(/data-face-super aria-hidden="true"/);
    expect(h1).not.toMatch(/data-face-brand[^>]*aria-hidden/);
    // The brand face is the stylesheet default; only a script sets the other.
    expect(css).toContain(".hero-headline__face--super,\n.hero-headline__mark[data-face=\"super\"] .hero-headline__face--brand {\n  opacity: 0;");
    expect(boot).toContain("initHeroHeadline(initGlassmark())");
    expect(cycle).toContain('reduce.addEventListener("change"');
    expect(cycle).toContain("visibilitychange");
  });

  it("gives every greeting script room and a face of its own", () => {
    const css = text("public/assets/css/site.css");
    const greeting = text("public/assets/js/greeting.js");

    // The spectrum paints only inside the box, so the box must cover the
    // marks that Telugu, Devanagari and Arabic put above and below the line.
    const word = css.slice(css.indexOf(".hello__word {"), css.indexOf(".hello__word:lang(te)"));
    expect(word).toMatch(/padding: 0\.\d+em 0\.\d+em;/);
    expect(word).toMatch(/margin: -0\.\d+em -0\.\d+em;/);
    for (const lang of ["te", "hi", "ar", "ja", "ko"]) {
      expect(css, `no font for :lang(${lang})`).toContain(`.hello__word:lang(${lang}) {`);
      expect(greeting, `greeting without lang ${lang}`).toContain(`lang: "${lang}"`);
    }
    expect(greeting).toContain('el.setAttribute("lang", g.lang)');
  });

  it("offers next-generation RAG assistants to founders and CTOs", () => {
    const home = text("public/index.html");
    const accepted = text("src/routes/contact.ts");

    const rag = home.slice(home.indexOf('id="rag"'), home.indexOf('id="build"'));
    expect(rag).toContain("Discover and implement next-generation");
    expect(rag).toContain("RAG assistants");
    expect(rag).toContain("for founders and CTOs");
    expect(rag).toContain("lightweight");
    expect(rag).toContain("one-week path from proof of concept to");
    expect(rag).toContain("wherever your data lives");
    expect(rag).toContain("startup founders");
    expect(rag).toContain("enterprise CTOs");
    // The one-week claim states what it depends on.
    expect(rag).toContain("the week assumes that access is ready");
    // The board is an illustration, labelled as such and hidden from readers.
    expect(rag).toContain("sample data");
    expect(rag).toMatch(/class="rag__board band-ink"[^>]*aria-hidden="true"/);
    // Reachable from the header, the phone menu and the footer.
    expect(home.match(/href="#rag"/g)?.length ?? 0).toBeGreaterThanOrEqual(3);
    // The form offers it and the server keeps it.
    expect(home).toContain('<option value="rag_assistant">RAG assistant</option>');
    expect(accepted).toContain('"rag_assistant"');
  });

  it("keeps the renamed sections reachable under their old anchors", () => {
    const home = text("public/index.html");
    for (const id of [
      "rag",
      "build",
      "stage",
      "examples",
      "how-we-work",
      "platforms",
      "resources",
      "about",
      "contact",
      // Anchors that existed before the sections were renamed.
      "workflow",
      "work",
      "expertise",
      "company",
    ]) {
      expect(home, `#${id} is missing`).toContain(`id="${id}"`);
    }
  });

  it("takes visitors from the introduction directly to the RAG offer", () => {
    const home = text("public/index.html");
    const sections = [...home.matchAll(/<section\b[^>]*\bid="([^"]+)"/g)].map(match => match[1]);
    expect(sections.slice(0, 3)).toEqual(["top", "rag", "build"]);
    expect(home).not.toContain("data-hero-panel");
    expect(home).not.toContain("data-stage-view");
    expect(text("public/assets/js/boot.js")).not.toContain("agent-stage");
    expect(home).toContain('class="offer__link" href="/ask-summaverick"');
    const examples = home.slice(home.indexOf('id="examples"'), home.indexOf('id="how-we-work"'));
    expect(examples).toContain('id="stage"');
    expect(examples).toContain('id="workflow"');
  });

  it("labels the remaining examples as illustrative instead of real actions", () => {
    const home = text("public/index.html");
    expect(home).toContain("Illustrative examples with sample data");
    expect(home).toContain("not a guarantee");
    for (const title of ["Customer questions", "Employee requests", "Company knowledge", "Your own systems"]) {
      expect(home).toContain(title);
    }
    // Only active research progress indicators may animate indefinitely.
    const loops = text("public/assets/css/site.css").split("\n").filter(line => line.includes("infinite"));
    for (const line of loops) expect(line, line.trim()).toMatch(/\b(rs-caret|chat-spin)\b/);
  });

  it("reads completely with scripting off", () => {
    const home = text("public/index.html");
    const css = text("public/assets/css/site.css");

    // All four examples remain in the document without requiring their player.
    expect(home.match(/data-demo-panel/g)).toHaveLength(4);
    expect(home).not.toMatch(/data-demo-panel[^>]*\shidden/);
    expect(home).toContain('href="#rag">Explore RAG assistants');

    // The retry button ships hidden and is shown only when it applies. It also
    // carries .btn, whose `display` outranks the browser's rule for [hidden],
    // so the sheet has to override it.
    const retry = home.slice(home.indexOf("data-lead-retry"));
    expect(retry.slice(0, retry.indexOf(">")), "retry should ship hidden").toContain(
      "hidden"
    );
    expect(css).toContain("[hidden] { display: none !important; }");
  });

  it("keeps entrance motion an enhancement rather than a requirement", () => {
    const css = text("public/assets/css/site.css");
    const chrome = text("public/assets/js/chrome.js");

    // The hidden pre-animation states only apply once the script has run AND
    // the visitor has not asked for reduced motion.
    expect(css).toContain(':root[data-motion="on"] [data-reveal]');
    expect(chrome).toContain('setAttribute("data-motion", "on")');
    expect(chrome).toContain('removeAttribute("data-motion")');
  });

  it("describes the research example in the mode the deployment actually runs", () => {
    const home = text("public/index.html");
    const research = text("public/research.html");

    // The homepage points at the one research product, not a retired page.
    expect(home).toContain('href="/research"');
    expect(home).not.toContain('href="/ask"');
    // No claim of a live answer service on the homepage.
    expect(home).not.toContain("Useful live answers");
    // And the tool itself states its mode before a question is asked.
    expect(research).toContain("data-ask-mode");
  });

  it("never shows a blanket verification badge on the research result", () => {
    const js = text("public/assets/js/research.js");
    expect(js).toContain("answer quality not evaluated");
    expect(js).not.toMatch(/citation\(s\).*verified/);
    expect(text("public/research.html")).not.toMatch(/\bverified\b/i);
  });

  it("renders model text without assigning HTML", () => {
    const md = text("public/assets/js/lib/markdown.js");
    const js = text("public/assets/js/research.js");
    for (const [name, src] of [["markdown.js", md], ["research.js", js]] as const) {
      expect(src, name).not.toMatch(/innerHTML|outerHTML|insertAdjacentHTML|html:/);
    }
    // Only http(s) links are ever built from model text.
    expect(md).toContain("https?:\\/\\/");
  });

  it("lists Research in the primary menu on every page with the company header", () => {
    for (const page of ["public/index.html", "public/research.html", "public/learn.html", "public/interviews.html"]) {
      const nav = text(page).match(/<nav class="nav-links"[\s\S]*?<\/nav>/)?.[0] ?? "";
      expect(nav, page).toMatch(/href="\/research"/);
    }
    expect(text("scripts/build-library.mjs")).toContain('link("/research", "research", "Research")');
  });

  it("retires /ask in favour of /research", () => {
    const worker = text("src/index.ts");
    expect(worker).toContain('"/ask": "/research"');
    expect(existsSync(resolve(root, "public/ask.html"))).toBe(false);
  });

  it("wires the contact form to the server and matches its responses", () => {
    const home = text("public/index.html");
    const form = text("public/assets/js/lead-form.js");

    for (const label of [
      "Your name",
      "Email",
      "Organization",
      "What are you building?",
      "Tell us about it",
      "Send your message",
    ]) {
      expect(home, label).toContain(label);
    }
    for (const option of [
      "Personal assistant",
      "Mobile agent experience",
      "Enterprise agent",
      "RAG assistant",
      "ServiceNow solution",
      "Not sure yet",
    ]) {
      expect(home, option).toContain(option);
    }
    // Every choice the form offers is one the server will store.
    const accepted = text("src/routes/contact.ts");
    for (const value of [...home.matchAll(/<option value="([^"]+)"/g)].map((m) => m[1] as string)) {
      expect(accepted, `server rejects intent ${value}`).toContain(`"${value}"`);
    }
    // The honeypot stays, and so does the hidden field it relies on.
    expect(home).toContain('name="company_url"');

    expect(form).toContain("/api/contact");
    expect(form).toContain("Thanks—your message has been received.");
    expect(form).toContain("Your message wasn't sent.");
    expect(form).toContain("We couldn't confirm whether your message was received.");
    expect(form).toContain("idempotency_key");
  });

  it("aligns page metadata and the footer with the stated offer", () => {
    const home = text("public/index.html");

    expect(home).toContain(
      "<title>Summaverick | Personal &amp; Enterprise AI Agents</title>"
    );
    expect(home).toContain(
      "We develop personal assistants, mobile AI experiences, and enterprise agents, including standalone products and solutions for ServiceNow and other platforms."
    );
    expect(home).toContain(
      "Personal assistants and enterprise agents, built for mobile, web, and business platforms."
    );
    // Claims the review flagged as unsupported must not come back.
    for (const claim of ["Store apps", "fine-tuning", "model training", "private deployment"]) {
      expect(home.toLowerCase(), claim).not.toContain(claim.toLowerCase());
    }
  });

  it("names the registered entity without overstating it", () => {
    const home = text("public/index.html");

    // Brand and legal operator stay distinguishable.
    expect(home).toContain("Summaverick Group is the public-facing brand of SUMMAVERICK LLP");
    expect(home).toContain("© <span data-year>2026</span> SUMMAVERICK LLP. Summaverick Group · LLPIN ADC-1832.");
    expect(home).toContain("ADC-1832");
    expect(home).toContain("14 September 2026");
    expect(home).toContain("Company details are based on the LLP agreement dated 14 September 2026.");

    // Structured data carries the same two names and no invented identifiers.
    const ld = JSON.parse(
      home.slice(
        home.indexOf("{", home.indexOf('type="application/ld+json"')),
        home.lastIndexOf("}") + 1
      )
    );
    expect(ld.name).toBe("Summaverick Group");
    expect(ld.legalName).toBe("SUMMAVERICK LLP");
    expect(ld.identifier.name).toBe("LLPIN");
    expect(ld.address.streetAddress).toBeUndefined();
    expect(ld["@type"]).toBe("Organization");

    // The June concept keeps its own date, and nothing backdates the LLP.
    expect(home).toContain("Our early hackathon concept");
    for (const claim of ["government-verified", "certificate of incorporation", "GSTIN", "CIN", "PAN"]) {
      expect(home, claim).not.toMatch(new RegExp(`\\b${claim}\\b`, "i"));
    }
  });

  it("only links to homepage sections that exist", () => {
    const home = text("public/index.html");
    const ids = new Set(
      [...home.matchAll(/id="([^"]+)"/g)].map((m) => m[1] as string)
    );

    const articleSample = "public/article/ai-agents-servicenow-beginner.html";
    for (const page of ["public/index.html", "public/research.html", "public/learn.html", "public/interviews.html", articleSample]) {
      const anchors = [...text(page).matchAll(/href="\/?#([^"]+)"/g)].map(
        (m) => m[1] as string
      );
      for (const anchor of anchors) {
        expect(ids, `${page} links to #${anchor}`).toContain(anchor);
      }
    }
  });

  it("keeps every colour in the token layer", () => {
    for (const sheet of ["public/assets/css/site.css", "public/assets/css/type.css"]) {
      const css = text(sheet);
      expect(css, `${sheet} hard-codes a hex colour`).not.toMatch(/#[0-9a-f]{3,8}\b/i);
      expect(css, `${sheet} hard-codes an rgb colour`).not.toMatch(/\brgba?\(/i);
    }
  });

  it("stays monochrome — no blue anywhere in the stylesheets", () => {
    const blues = ["#0066cc", "#0058b0", "#0071e3", "#0077ed", "#0a6fd6", "#2997ff", "#55aeff", "#0a84ff", "#409cff"];
    for (const sheet of ["public/assets/css/tokens.css", "public/assets/app.css"]) {
      const css = text(sheet);
      for (const blue of blues) {
        expect(css, `${sheet} still contains ${blue}`).not.toContain(blue);
      }
    }
  });

  it("lets the visitor override the system appearance", () => {
    const tokens = text("public/assets/css/tokens.css");

    // OS default plus an explicit choice that always wins.
    expect(tokens).toContain("@media (prefers-color-scheme: dark)");
    expect(tokens).toContain(':root[data-theme="dark"]');
    expect(tokens).toContain(':root[data-theme="light"]');

    for (const page of ["public/index.html", "public/research.html", "public/learn.html"]) {
      const html = text(page);
      expect(html, `${page} has no appearance toggle`).toContain('id="theme-toggle"');
      expect(html, `${page} flashes before paint`).toContain("sv-theme");
    }
  });

  it("keeps blog and interview routes in the shared studio library", () => {
    const chrome = text("public/assets/app.js");

    expect(chrome).toContain("brand-mark");
    expect(chrome).not.toContain("summaverick-uncontained-sum.svg");
  });

  it("ships the full writing library as static pages", () => {
    // Slug scheme mirrors scripts/build-library.mjs (which itself mirrors
    // scripts/seed-content.ts): posts use uniqueId minus trailing timestamp,
    // interviews use the slugified question, -<id> breaks clashes.
    const slugify = (s: string) =>
      String(s ?? "")
        .toLowerCase()
        .replace(/[^a-z0-9]+/g, "-")
        .replace(/^-+|-+$/g, "")
        .slice(0, 80)
        .replace(/-+$/g, "");
    const used = new Set<string>();
    const unique = (base: string, id: string) => {
      let slug = base || `item-${id}`;
      if (used.has(slug)) slug = `${slug}-${id}`;
      used.add(slug);
      return slug;
    };

    const posts = JSON.parse(text("scripts/data/posts.json")) as Array<{
      id: number | string;
      uniqueId?: string;
      title: string;
    }>;
    const qas = JSON.parse(text("scripts/data/interviews.json")) as Array<{
      id: number | string;
      question: string;
    }>;
    const slugs = [
      ...posts.map((p) => {
        const flat = String(p.uniqueId ?? "").replace(/\//g, "-");
        return unique(flat ? flat.replace(/-\d{10,}$/, "") : slugify(p.title), String(p.id));
      }),
      ...qas.map((q) => unique(slugify(q.question), String(q.id))),
    ];

    expect(posts).toHaveLength(52);
    expect(qas).toHaveLength(57);
    for (const slug of slugs) {
      expect(existsSync(resolve(root, `public/article/${slug}.html`)), slug).toBe(true);
    }

    // Listing pages carry every title and need no API to render. Titles
    // are HTML-escaped in the markup, so compare the escaped form.
    const esc = (s: string) =>
      String(s ?? "")
        .replace(/&/g, "&amp;")
        .replace(/</g, "&lt;")
        .replace(/>/g, "&gt;")
        .replace(/"/g, "&quot;");
    const learn = text("public/learn.html");
    const interviews = text("public/interviews.html");
    expect(learn).not.toContain("/api/content");
    expect(interviews).not.toContain("/api/content");
    for (const p of posts) expect(learn, p.title).toContain(esc(p.title));
    // Interview questions carry inline <strong> markup; listings show the
    // words, never the tags.
    const plain = (s: string) => s.replace(/<\/?(strong|em|b|i|code|pre)>/gi, "").replace(/\s+/g, " ").trim();
    for (const q of qas) {
      const opening = plain(q.question).split(/(?<=[.?!])\s+/)[0] ?? "";
      expect(interviews, q.question).toContain(esc(opening.slice(0, 60)));
    }
    expect(interviews).not.toMatch(/&lt;\/?strong&gt;/);

    const sitemap = text("public/sitemap.xml");
    for (const slug of slugs) {
      // Slugs with & are XML-escaped in the sitemap, as the builder writes.
      expect(sitemap, slug).toContain(`/article/${slug.replace(/&/g, "&amp;")}`);
    }
    expect(text("public/robots.txt")).toContain("Sitemap: https://summaverick.com/sitemap.xml");
  });
});
