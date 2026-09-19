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
    for (const page of ["public/index.html", "public/ask.html", "public/learn.html", "public/quiz.html"]) {
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
      "public/assets/img/summaverick-mark.svg",
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

    expect(home).toContain("AI agents for everyday life and work.");
    expect(home).toContain("Personal assistants and enterprise agents");
    expect(home).toContain("from personal assistants on mobile to enterprise workflows");
    // Standalone products and platform work are both explicit.
    expect(home).toContain("We build standalone products and agents that work with ServiceNow");
    expect(home).toContain(">Build with us<");
    expect(home).toContain(">Explore the possibilities<");
    // The hero must not depend on the illustration or on any animation.
    const hero = home.slice(home.indexOf('id="top"'), home.indexOf('id="stage"'));
    expect(hero).toContain("<h1");
    expect(hero).not.toContain("data-reveal");
  });

  it("keeps the renamed sections reachable under their old anchors", () => {
    const home = text("public/index.html");
    for (const id of [
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

  it("pairs a personal and an enterprise walk-through without auto-cycling", () => {
    const home = text("public/index.html");
    const css = text("public/assets/css/site.css");
    const js = text("public/assets/js/agent-stage.js");

    expect(home).toContain("Illustrative experience");
    expect(home).toContain('data-stage-tab="personal"');
    expect(home).toContain('data-stage-tab="enterprise"');
    // Five steps each: task, context, proposal, review, observable next step.
    for (const view of ["personal", "enterprise"]) {
      const start = home.indexOf(`data-stage-view="${view}"`);
      const chunk = home.slice(start, home.indexOf("</ol>", start));
      for (let step = 0; step < 5; step += 1) {
        expect(chunk, `${view} step ${step}`).toContain(`data-stage-step="${step}"`);
      }
    }
    // Play, Pause, Replay, and manual steps when motion is reduced.
    expect(home).toContain("data-stage-play");
    expect(home).toContain("data-stage-prev");
    expect(home).toContain("data-stage-next");
    expect(js).toContain("Pause");
    expect(js).toContain("Replay");
    expect(js).toContain("prefers-reduced-motion: reduce");
    // Playback waits at the review step instead of deciding for the visitor.
    expect(js).toContain("waitForDecision");
    // Nothing on the page loops forever.
    expect(css).not.toContain("infinite");
  });

  it("cannot let an illustration be mistaken for a real action", () => {
    const home = text("public/index.html");
    const js = text("public/assets/js/agent-stage.js");

    expect(home).toContain("sample data");
    expect(home).toContain("Nothing here contacts a\n            merchant");
    expect(home).toContain("Nothing was sent to a merchant.");
    expect(home).toContain("No real record was changed.");
    expect(js).toContain("data-stage-advance");
  });

  it("reads completely with scripting off", () => {
    const home = text("public/index.html");
    const css = text("public/assets/css/site.css");

    // Tabs and playback controls only appear once the page knows JS is running.
    expect(home).toContain('h.setAttribute("data-js", "on")');
    expect(css).toContain(':root[data-js="on"] .stage__tabs { display: flex; }');
    expect(css).toContain(':root[data-js="on"] .stage__controls { display: flex; }');
    expect(css).toContain(".stage__tabs {\n  display: none;");

    // The second view is never hidden by markup alone, or scripting-off
    // visitors would lose it behind a tab strip they cannot see.
    const stage = home.slice(home.indexOf('id="stage"'), home.indexOf('id="build"'));
    expect(stage).not.toMatch(/data-stage-view="[^"]+"[^>]*\shidden/);
    expect(css).toContain(':root[data-js="on"] .stage__view[data-stage-inactive]');

    // A class that sets display must not be able to outrank [hidden].
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
    expect(home).toContain("Hackathon concept · June 2026");
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
    for (const q of qas) expect(interviews, q.question).toContain(esc(q.question));

    const sitemap = text("public/sitemap.xml");
    for (const slug of slugs) {
      // Slugs with & are XML-escaped in the sitemap, as the builder writes.
      expect(sitemap, slug).toContain(`/article/${slug.replace(/&/g, "&amp;")}`);
    }
    expect(text("public/robots.txt")).toContain("Sitemap: https://summaverick.com/sitemap.xml");
  });
});
