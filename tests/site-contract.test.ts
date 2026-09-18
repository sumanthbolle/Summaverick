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

describe("Classic Studio public contract", () => {
  it("ships the approved brand assets and accessible motion fallback", () => {
    const home = text("public/index.html");
    const css = text("public/assets/css/site.css");

    expect(home).toContain("summaverick-uncontained-sum.svg");
    expect(home).not.toContain("sumanth-reveal-v1.png");
    expect(home).toContain("Software that makes everyday work easier.");
    expect(home).toContain('id="expertise"');
    expect(home).toContain('id="work"');
    expect(home).toContain('id="contact"');
    expect(css).toContain(".hero__inner");
    expect(css).toContain("@media (prefers-reduced-motion: reduce)");
  });

  it("lets the hidden attribute win, so the library filters can hide a card", () => {
    const css = text("public/assets/css/site.css");

    // .lib-card sets display: grid, which beats the browser's own [hidden]
    // rule at equal specificity. Without this the filters set the attribute
    // and every card stays on screen.
    expect(css).toMatch(/\[hidden\]\s*\{\s*display:\s*none\s*!important/);
    expect(css).toMatch(/\.lib-card\s*\{[^}]*display:\s*grid/);
  });

  it("names the specialties, the buyer and the next step in the first screen", () => {
    const home = text("public/index.html");
    const hero = home.slice(
      home.indexOf('<section class="hero"'),
      home.indexOf('<section class="proof"')
    );

    // Specialties, before a visitor has to scroll or interpret anything.
    expect(hero).toContain("ServiceNow applications");
    expect(hero).toContain("Integrations");
    expect(hero).toContain("AI tools");
    // One primary action and one route to the evidence.
    expect(hero).toContain('href="#contact"');
    expect(hero).toContain('href="#services"');
    // A real interface, with its box reserved so the copy does not shift.
    expect(hero).toMatch(/<img[^>]+shot-research-answer\.png/);
    expect(hero).toMatch(/width="\d+" height="\d+"/);
  });

  it("keeps every homepage image dimensioned, so nothing reflows on load", () => {
    const home = text("public/index.html");
    for (const tag of home.match(/<img[^>]*>/g) ?? []) {
      expect(tag, tag).toMatch(/\swidth="\d+"/);
      expect(tag, tag).toMatch(/\sheight="\d+"/);
    }
  });

  it("does not claim a client project it cannot attribute", () => {
    const home = text("public/index.html");

    // Every item in Selected work carries an honest status label.
    const work = home.slice(home.indexOf('id="work"'), home.indexOf('id="expertise"'));
    expect(work).toContain("Internal tool");
    expect(work).toContain("No client project is published here yet.");
  });

  it("only links to homepage sections that exist", () => {
    const home = text("public/index.html");
    const ids = new Set(
      [...home.matchAll(/id="([^"]+)"/g)].map((m) => m[1] as string)
    );

    for (const page of ["public/index.html", "public/ask.html"]) {
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

  it("follows the system appearance instead of an in-page override", () => {
    const tokens = text("public/assets/css/tokens.css");

    expect(tokens).toContain("@media (prefers-color-scheme: dark)");
    for (const page of ["public/index.html", "public/ask.html"]) {
      expect(text(page), `${page} pins an appearance`).not.toContain("data-theme");
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
      expect(sitemap, slug).toContain(`/article/${slug}`);
    }
    expect(text("public/robots.txt")).toContain("Sitemap: https://summaverick.com/sitemap.xml");
  });
});
