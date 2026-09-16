import { createHash } from "node:crypto";
import { readFileSync } from "node:fs";
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
    expect(home).toContain("sumanth-reveal-v1.png");
    expect(home).toContain("We turn product ambition into working software.");
    expect(home).toContain('id="expertise"');
    expect(home).toContain('id="work"');
    expect(home).toContain('id="contact"');
    expect(css).toContain(".studio-hero");
    expect(css).toContain("@media (prefers-reduced-motion: reduce)");
  });
});
