// Shared chrome, API helper, and tiny DOM utils for every summaverick page.

export const LINKS = [
  { href: "/", id: "home", label: "Home", icon: "home" },
  { href: "/learn", id: "learn", label: "Guides", icon: "learn" },
  { href: "/ask-summaverick", id: "ask", label: "Meet Summaverick", icon: "ask" },
  { href: "/research", id: "research", label: "Research", icon: "research" },
];

const CONTACT_HREF = "/#contact";

export const $ = (id) => document.getElementById(id);

export async function api(path, opts = {}) {
  const res = await fetch(path, {
    ...opts,
    headers: { "content-type": "application/json", ...(opts.headers || {}) },
    credentials: "same-origin",
  });
  const data = await res.json().catch(() => ({}));
  if (!res.ok || data.ok === false) {
    const err = new Error(data.message || data.error || `HTTP ${res.status}`);
    err.status = res.status;
    err.body = data;
    throw err;
  }
  return data;
}

export async function apiSoft(path, opts = {}) {
  try {
    return await api(path, opts);
  } catch (e) {
    return e.body && typeof e.body === "object" ? e.body : { ok: false, message: e.message };
  }
}

export function el(tag, opts = {}, ...kids) {
  const n = document.createElement(tag);
  if (opts.class) n.className = opts.class;
  if (opts.text != null) n.textContent = String(opts.text);
  if (opts.html != null) n.innerHTML = opts.html;
  if (opts.href) n.href = opts.href;
  if (opts.type) n.type = opts.type;
  if (opts.attrs) for (const [k, v] of Object.entries(opts.attrs)) n.setAttribute(k, v);
  for (const k of kids) if (k) n.append(k);
  return n;
}

export function esc(s) {
  return String(s ?? "").replace(/[&<>"']/g, (c) =>
    ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c])
  );
}

export function fmtDate(msOrIso) {
  if (msOrIso == null) return "";
  const d = typeof msOrIso === "number" ? new Date(msOrIso) : new Date(msOrIso);
  if (Number.isNaN(d.getTime())) return "";
  return d.toLocaleDateString(undefined, { year: "numeric", month: "short", day: "numeric" });
}

export function articleHref(slug) {
  return "/article/" + encodeURIComponent(slug);
}

function icon(name) {
  const paths = {
    home: "M4 12 L12 4 L20 12 V20 H15 V14 H9 V20 H4 Z",
    learn: "M4 7 L12 4 L20 7 V17 L12 20 L4 17 Z M12 4 V20",
    ask: "M4 5 H20 V16 H10 L6 20 V16 H4 Z",
    research: "M11 11 m-6 0 a6 6 0 1 0 12 0 a6 6 0 1 0 -12 0 M15 15 L20 20",
  };
  const svg = document.createElementNS("http://www.w3.org/2000/svg", "svg");
  svg.setAttribute("viewBox", "0 0 24 24");
  svg.setAttribute("width", "18");
  svg.setAttribute("height", "18");
  svg.setAttribute("fill", "none");
  svg.setAttribute("stroke", "currentColor");
  svg.setAttribute("stroke-width", "1.8");
  svg.setAttribute("stroke-linejoin", "round");
  svg.setAttribute("stroke-linecap", "round");
  svg.setAttribute("aria-hidden", "true");
  const p = document.createElementNS("http://www.w3.org/2000/svg", "path");
  p.setAttribute("d", paths[name] || paths.home);
  svg.append(p);
  return svg;
}

function currentPath() {
  const p = location.pathname.replace(/\/+$/, "") || "/";
  if (p.endsWith(".html")) return p.replace(/\.html$/, "") || "/";
  return p;
}

function isActive(href, path) {
  if (href === "/") return path === "/";
  if (href === "/learn") return path === "/learn" || path.startsWith("/article");
  return path === href || path.startsWith(href + "/");
}

export function mountChrome({ active } = {}) {
  const path = currentPath();
  const header = $("site-header");
  const tabbar = $("tabbar");
  const footer = $("site-footer");

  if (header && !header.dataset.ready) {
    header.className = "nav";
    header.dataset.ready = "1";
    const brand = el("a", { class: "brand", href: "/" });
    brand.setAttribute("aria-label", "Summaverick Group home");
    // Inline so the mark takes the surrounding ink; master lives at
    // public/assets/img/summaverick-mark.svg.
    const mark = document.createElementNS("http://www.w3.org/2000/svg", "svg");
    mark.setAttribute("class", "brand-mark");
    mark.setAttribute("viewBox", "12 12 78 84");
    mark.setAttribute("width", "23");
    mark.setAttribute("height", "25");
    mark.setAttribute("aria-hidden", "true");
    mark.setAttribute("focusable", "false");
    const markPath = document.createElementNS("http://www.w3.org/2000/svg", "path");
    markPath.setAttribute("fill", "currentColor");
    markPath.setAttribute(
      "d",
      "M90 12H38a26 26 0 0 0 0 52h24a6 6 0 0 1 0 12H22l-8 20h48a26 26 0 0 0 0-52H38a6 6 0 0 1 0-12h44Z"
    );
    mark.append(markPath);
    brand.append(mark, document.createTextNode("Summaverick"));
    const links = el("nav", { class: "nav-links", attrs: { "aria-label": "Primary" } });
    for (const l of LINKS) {
      const a = el("a", { href: l.href, text: l.label });
      if (isActive(l.href, path) || active === l.id) a.setAttribute("aria-current", "page");
      links.append(a);
    }
    const end = el("div", { class: "nav-end" });
    end.append(el("a", { class: "btn primary", href: CONTACT_HREF, text: "Build with us" }));
    header.append(brand, links, end);
  }

  if (tabbar && !tabbar.dataset.ready) {
    tabbar.className = "tabbar";
    tabbar.dataset.ready = "1";
    tabbar.setAttribute("aria-label", "Primary");
    for (const t of LINKS) {
      const a = el("a", { href: t.href });
      if (isActive(t.href, path) || active === t.id) a.setAttribute("aria-current", "page");
      a.append(icon(t.icon), el("span", { text: t.id === "ask" ? "Summaverick" : t.label }));
      tabbar.append(a);
    }
  }

  if (footer && !footer.dataset.ready) {
    footer.className = "site-foot";
    footer.dataset.ready = "1";
    const nav = el("nav", { attrs: { "aria-label": "Footer" } });
    for (const l of [...LINKS, { href: CONTACT_HREF, label: "Build with us" }]) {
      nav.append(el("a", { href: l.href, text: l.label }));
    }
    footer.append(
      nav,
      el("p", {
        class: "copy",
        text: "© SUMMAVERICK LLP. Personal assistants and enterprise agents for mobile, web, and business platforms.",
      })
    );
  }
}

export function showError(node, err) {
  if (!node) return;
  node.hidden = false;
  node.textContent = err && err.message ? err.message : String(err || "Something went wrong.");
}

export function setHidden(node, hidden) {
  if (node) node.hidden = hidden;
}
