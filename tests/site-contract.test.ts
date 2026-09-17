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
    expect(home).toContain("We build software people are glad to use.");
    expect(home).toContain('id="expertise"');
    expect(home).toContain('id="work"');
    expect(home).toContain('id="contact"');
    expect(css).toContain(".studio-hero");
    expect(css).toContain("@media (prefers-reduced-motion: reduce)");
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
