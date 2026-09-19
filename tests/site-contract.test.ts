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

    expect(home).toContain("summaverick-uncontained-sum.svg");
    expect(home).not.toContain("sumanth-reveal-v1.png");
    expect(css).toContain("@media (prefers-reduced-motion: reduce)");
  });

  it("states the offer, the specialties and the next step in the first screen", () => {
    const home = text("public/index.html");

    expect(home).toContain("Make everyday work easier for your team.");
    expect(home).toContain("ServiceNow applications · Integrations · AI tools");
    expect(home).toContain(
      "We build ServiceNow applications, connect business systems, and"
    );
    expect(home).toContain(">Discuss your project<");
    expect(home).toContain(">Explore what we build<");
    // The hero must not depend on the illustration or on any animation.
    const hero = home.slice(home.indexOf('id="top"'), home.indexOf('id="build"'));
    expect(hero).toContain("<h1");
    expect(hero).not.toContain("data-reveal");
  });

  it("keeps the renamed sections reachable under their old anchors", () => {
    const home = text("public/index.html");
    for (const id of [
      "build",
      "workflow",
      "examples",
      "how-we-work",
      "resources",
      "about",
      "contact",
      // Anchors that existed before the sections were renamed.
      "work",
      "expertise",
      "company",
    ]) {
      expect(home, `#${id} is missing`).toContain(`id="${id}"`);
    }
  });

  it("labels the workflow illustration as an illustration and lets it be replayed", () => {
    const home = text("public/index.html");
    const css = text("public/assets/css/site.css");
    const js = text("public/assets/js/workflow-demo.js");

    expect(home).toContain("Illustrative example");
    expect(home).toContain("See how a request could move through a workflow.");
    for (const label of ["Request received", "Information gathered", "Ready for review"]) {
      expect(home, label).toContain(label);
    }
    expect(home).toContain("data-flow-play");
    expect(home).toContain("data-flow-prev");
    expect(home).toContain("data-flow-next");
    // Play, Pause, Replay, and manual steps when motion is reduced.
    expect(js).toContain("Pause");
    expect(js).toContain("Replay example");
    expect(js).toContain("prefers-reduced-motion: reduce");
    // Nothing on the page loops forever.
    expect(css).not.toContain("infinite");
  });

  it("actually hides the elements scripts hide with the hidden attribute", () => {
    const home = text("public/index.html");
    const css = text("public/assets/css/site.css");

    // The manual step controls and the retry button ship hidden and are shown
    // only when they apply. They also carry .btn, whose `display` outranks the
    // browser's rule for [hidden], so the sheet has to override it.
    for (const marker of ["data-flow-prev", "data-flow-next", "data-lead-retry"]) {
      const tag = home.slice(home.indexOf(marker));
      expect(tag.slice(0, tag.indexOf(">")), `${marker} should ship hidden`).toContain(
        "hidden"
      );
    }
    expect(css).toContain("[hidden] { display: none !important; }");
  });

  it("keeps entrance motion an enhancement rather than a requirement", () => {
    const css = text("public/assets/css/site.css");
    const chrome = text("public/assets/js/chrome.js");

    // The hidden pre-animation states only apply once the script has run AND
    // the visitor has not asked for reduced motion.
    expect(css).toContain(':root[data-motion="on"] [data-reveal]');
    expect(css).toContain(':root[data-motion="on"] .hero-panel');
    expect(chrome).toContain('setAttribute("data-motion", "on")');
    expect(chrome).toContain('removeAttribute("data-motion")');
  });

  it("describes the research example in the mode the deployment actually runs", () => {
    const home = text("public/index.html");
    const ask = text("public/ask.html");

    expect(home).toContain("ServiceNow reference search");
    expect(home).toContain("Internal demo");
    expect(home).toContain("Search documentation");
    // No claim of a live answer service on the homepage.
    expect(home).not.toContain("Useful live answers");
    // And the tool itself states its mode before a question is asked.
    expect(ask).toContain("data-ask-mode");
  });

  it("never shows a blanket verification badge on the research result", () => {
    const js = text("public/assets/js/ask.js");
    expect(js).toContain("answer quality not evaluated");
    expect(js).not.toMatch(/citation\(s\).*verified/);
  });

  it("wires the contact form to the server and matches its responses", () => {
    const home = text("public/index.html");
    const form = text("public/assets/js/lead-form.js");

    for (const label of [
      "Your name",
      "Email",
      "Organization",
      "What do you need help with?",
      "Tell us about it",
      "Send your message",
    ]) {
      expect(home, label).toContain(label);
    }
    for (const option of [
      "ServiceNow application",
      "System integration",
      "AI tool",
      "Not sure yet",
    ]) {
      expect(home, option).toContain(option);
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
      "<title>Summaverick | ServiceNow applications, integrations &amp; AI</title>"
    );
    expect(home).toContain(
      "Summaverick builds ServiceNow applications, connects business systems, and develops AI tools for everyday work."
    );
    expect(home).toContain(
      "ServiceNow applications, integrations, and AI tools for everyday work."
    );
    // Claims the review flagged as unsupported must not come back.
    for (const claim of ["Store apps", "fine-tuning", "model training", "private deployment"]) {
      expect(home.toLowerCase(), claim).not.toContain(claim.toLowerCase());
    }
  });

  it("only links to homepage sections that exist", () => {
    const home = text("public/index.html");
    const ids = new Set(
      [...home.matchAll(/id="([^"]+)"/g)].map((m) => m[1] as string)
    );

    const articleSample = "public/article/ai-agents-servicenow-beginner.html";
    for (const page of ["public/index.html", "public/ask.html", "public/learn.html", "public/interviews.html", articleSample]) {
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

    for (const page of ["public/index.html", "public/ask.html", "public/learn.html"]) {
      const html = text(page);
      expect(html, `${page} has no appearance toggle`).toContain('id="theme-toggle"');
      expect(html, `${page} flashes before paint`).toContain("sv-theme");
    }
  });

  it("keeps blog and interview routes in the shared studio library", () => {
    const chrome = text("public/assets/app.js");

    expect(chrome).toContain("summaverick-uncontained-sum.svg");
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
    for (const q of qas) expect(interviews, q.question).toContain(esc(q.question));

    const sitemap = text("public/sitemap.xml");
    for (const slug of slugs) {
      // Slugs with & are XML-escaped in the sitemap, as the builder writes.
      expect(sitemap, slug).toContain(`/article/${slug.replace(/&/g, "&amp;")}`);
    }
    expect(text("public/robots.txt")).toContain("Sitemap: https://summaverick.com/sitemap.xml");
  });
});
