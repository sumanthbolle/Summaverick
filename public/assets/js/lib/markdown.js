/*
 * A small Markdown renderer for model-written answers. It builds DOM nodes
 * directly and never assigns HTML, so text from the model can only ever become
 * text, and links only ever point at http(s) URLs.
 *
 * Supported: paragraphs, ### headings, ordered and unordered lists, fenced
 * code blocks, `inline code`, **bold**, *italic*, [text](https://…) links, and
 * [n] citations, which link to #src-n when n names a listed source.
 *
 * Safe to call on a partial stream: an unclosed code fence renders as a code
 * block that is still being written.
 */

import { el } from "./dom.js";

export function renderMarkdown(text, { sourceCount = 0 } = {}) {
  const frag = document.createDocumentFragment();
  const lines = String(text || "").replace(/\r\n?/g, "\n").split("\n");
  let i = 0;

  while (i < lines.length) {
    const line = lines[i];

    // Fenced code block (```lang).
    const fence = line.match(/^\s*```\s*([\w+-]*)\s*$/);
    if (fence) {
      const body = [];
      i++;
      while (i < lines.length && !/^\s*```\s*$/.test(lines[i])) body.push(lines[i++]);
      i++; // closing fence (or end of a partial stream)
      const code = el("code", { text: body.join("\n") });
      if (fence[1]) code.dataset.lang = fence[1];
      frag.append(el("pre", { class: "md-code" }, [code]));
      continue;
    }

    if (!line.trim()) { i++; continue; }

    // Headings: anything from # to ###### becomes h3/h4 so the page outline holds.
    const heading = line.match(/^\s*(#{1,6})\s+(.*)$/);
    if (heading) {
      const tag = heading[1].length <= 2 ? "h3" : "h4";
      frag.append(el(tag, { class: "md-h" }, inline(heading[2], sourceCount)));
      i++;
      continue;
    }

    // Lists.
    const ordered = /^\s*\d+[.)]\s+/;
    const unordered = /^\s*[-*•]\s+/;
    if (ordered.test(line) || unordered.test(line)) {
      const isOrdered = ordered.test(line);
      const pattern = isOrdered ? ordered : unordered;
      const list = el(isOrdered ? "ol" : "ul", { class: "md-list" });
      while (i < lines.length && pattern.test(lines[i])) {
        let item = lines[i].replace(pattern, "");
        i++;
        // Continuation lines indented under the item.
        while (i < lines.length && /^\s{2,}\S/.test(lines[i]) && !ordered.test(lines[i]) && !unordered.test(lines[i])) {
          item += " " + lines[i].trim();
          i++;
        }
        list.append(el("li", {}, inline(item, sourceCount)));
      }
      frag.append(list);
      continue;
    }

    // Paragraph: consecutive plain lines.
    const para = [line.trim()];
    i++;
    while (
      i < lines.length &&
      lines[i].trim() &&
      !/^\s*```/.test(lines[i]) &&
      !/^\s*#{1,6}\s+/.test(lines[i]) &&
      !ordered.test(lines[i]) &&
      !unordered.test(lines[i])
    ) {
      para.push(lines[i].trim());
      i++;
    }
    frag.append(el("p", {}, inline(para.join(" "), sourceCount)));
  }

  return frag;
}

const INLINE = /(`[^`]+`)|(\*\*[^*]+\*\*)|(\[(\d{1,2})\](?!\())|(\[([^\]]+)\]\((https?:\/\/[^\s)]+)\))|(\*[^*\s][^*]*\*)/g;

function inline(text, sourceCount) {
  const out = [];
  let last = 0;
  for (const m of text.matchAll(INLINE)) {
    if (m.index > last) out.push(text.slice(last, m.index));
    if (m[1]) {
      out.push(el("code", { text: m[1].slice(1, -1) }));
    } else if (m[2]) {
      out.push(el("strong", {}, inline(m[2].slice(2, -2), sourceCount)));
    } else if (m[3]) {
      const n = Number(m[4]);
      out.push(citation(n, sourceCount));
    } else if (m[5]) {
      out.push(el("a", { href: m[7], rel: "noopener", target: "_blank", text: m[6] }));
    } else if (m[8]) {
      out.push(el("em", {}, inline(m[8].slice(1, -1), sourceCount)));
    }
    last = m.index + m[0].length;
  }
  if (last < text.length) out.push(text.slice(last));
  return out;
}

function citation(n, sourceCount) {
  if (n >= 1 && n <= sourceCount) {
    return el("a", {
      class: "cite",
      href: `#src-${n}`,
      "aria-label": `Source ${n}`,
      dataset: { cite: String(n) },
      text: String(n),
    });
  }
  // A number that points at nothing is shown, but not as a link.
  return el("span", { class: "cite cite--none", title: "No listed source has this number", text: String(n) });
}
