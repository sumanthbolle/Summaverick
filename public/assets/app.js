// Shared chrome, API helper, and tiny DOM utils for every summaverick page.

export const LINKS = [
  { href: "/", id: "home", label: "Home" },
  { href: "/learn", id: "learn", label: "Learn" },
  { href: "/quiz", id: "quiz", label: "Quiz" },
  { href: "/tools", id: "tools", label: "Tools" },
];

const TOOL_LINKS = [
  { href: "/research", label: "Research" },
  { href: "/flights", label: "SkyFare" },
  { href: "/metals", label: "Metals" },
  { href: "/upsc", label: "UPSC" },
  { href: "/advocate", label: "Advocate" },
];

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
    quiz: "M8 6 H16 V10 H8 Z M8 13 H16 V18 H8 Z",
    tools: "M7 7 H11 V11 H7 Z M13 7 H17 V11 H13 Z M7 13 H11 V17 H7 Z M13 13 H17 V17 H13 Z",
    account: "M12 12 a4 4 0 1 0 0-8 a4 4 0 0 0 0 8 M5 20 c0-4 3-6 7-6 s7 2 7 6",
    search: "M11 11 m-6 0 a6 6 0 1 0 12 0 a6 6 0 1 0 -12 0 M15 15 L20 20",
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
  if (href === "/learn") return path === "/learn" || path === "/interviews" || path.startsWith("/article");
  if (href === "/tools") {
    return [
      "/tools",
      "/research",
      "/flights",
      "/metals",
      "/upsc",
      "/advocate",
    ].includes(path);
  }
  return path === href || path.startsWith(href + "/");
}

export async function currentUser() {
  try {
    const data = await api("/api/me");
    return data.user || null;
  } catch {
    return null;
  }
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
    brand.setAttribute("aria-label", "Summaverick home");
    const mark = el("img", {
      attrs: {
        src: "/assets/img/summaverick-uncontained-sum.svg",
        alt: "",
        width: "24",
        height: "24",
        "aria-hidden": "true",
      },
    });
    brand.append(mark, document.createTextNode("Summaverick"));
    const links = el("nav", { class: "nav-links", attrs: { "aria-label": "Primary" } });
    for (const l of LINKS) {
      const a = el("a", { href: l.href, text: l.label });
      if (isActive(l.href, path) || active === l.id) a.setAttribute("aria-current", "page");
      links.append(a);
    }
    const end = el("div", { class: "nav-end" });
    const search = el("a", { class: "nav-search", href: "/search" });
    search.append(icon("search"), el("span", { text: "Search" }));
    search.setAttribute("aria-label", "Search");
    const searchM = el("a", { class: "icon-btn nav-search-m", href: "/search" });
    searchM.append(icon("search"));
    searchM.setAttribute("aria-label", "Search");
    const account = el("a", { class: "icon-btn", href: "/signin", attrs: { id: "nav-account", "aria-label": "Sign in" } });
    account.append(icon("account"));
    end.append(search, searchM, account);
    header.append(brand, links, end);

    currentUser().then((user) => {
      if (!user) return;
      account.href = "/signin";
      account.setAttribute("aria-label", user.displayName || user.email || "Account");
      account.title = user.displayName || user.email || "Signed in";
    });
  }

  if (tabbar && !tabbar.dataset.ready) {
    tabbar.className = "tabbar";
    tabbar.dataset.ready = "1";
    tabbar.setAttribute("aria-label", "Primary");
    const tabs = [
      { href: "/", id: "home", label: "Home", icon: "home" },
      { href: "/learn", id: "learn", label: "Learn", icon: "learn" },
      { href: "/quiz", id: "quiz", label: "Quiz", icon: "quiz" },
      { href: "/tools", id: "tools", label: "Tools", icon: "tools" },
      { href: "/signin", id: "account", label: "Account", icon: "account" },
    ];
    for (const t of tabs) {
      const a = el("a", { href: t.href });
      if (isActive(t.href, path) || active === t.id) a.setAttribute("aria-current", "page");
      a.append(icon(t.icon), el("span", { text: t.label }));
      tabbar.append(a);
    }
  }

  if (footer && !footer.dataset.ready) {
    footer.className = "site-foot";
    footer.dataset.ready = "1";
    const nav = el("nav", { attrs: { "aria-label": "Footer" } });
    for (const l of [...LINKS, ...TOOL_LINKS, { href: "/signin", label: "Sign in" }]) {
      nav.append(el("a", { href: l.href, text: l.label }));
    }
    footer.append(
      nav,
      el("p", {
        class: "copy",
        text: "Summaverick — ServiceNow products, AI systems, and learning tools.",
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
