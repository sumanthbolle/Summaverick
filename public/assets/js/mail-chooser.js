/*
 * Mail chooser: "open in Outlook, or whichever is possible".
 *
 * A mailto: link hands the message to whatever mail app the computer has
 * registered. Phones nearly always have one. Many laptops do not (people read
 * Outlook or Gmail in a browser), and there the click does nothing at all, with
 * no error. A page cannot tell which case it is in, so on a desktop pointer a
 * click on a mailto link opens a small dialog instead, with the ways to write:
 * the computer's own mail app, Outlook on the web, Outlook.com, Gmail, Yahoo
 * Mail, or the address to copy.
 *
 * The link itself is never changed. With no script, on a phone, with a modifier
 * key held, or in a browser without <dialog>, it is a plain mailto link and
 * behaves exactly as before.
 *
 * The compose URLs below are the services' own public ones, and they are the
 * part to re-test first if a provider changes how it takes a prefilled message:
 *   Outlook   to, subject, body
 *   Gmail     to, su, body        (view=cm opens the compose window)
 *   Yahoo     to, subject, body
 */

import { el } from "./lib/dom.js";

const STORAGE_KEY = "sv-mail-provider";
const STYLESHEET = "/assets/css/mail-chooser.css";
const RECIPIENT = /^[^\s@,;<>()]+@[^\s@,;<>()]+\.[^\s@,;<>()]+$/;

/**
 * The parts of a mailto: link a webmail compose window can take, or null when
 * the link is not a mailto: link or names a recipient that is not an address.
 * @param {string} href
 * @returns {{ to: string, subject: string, body: string } | null}
 */
export function parseMailto(href) {
  let url;
  try {
    url = new URL(href);
  } catch {
    return null;
  }
  if (url.protocol !== "mailto:") return null;

  let list;
  try {
    list = decodeURIComponent(url.pathname);
  } catch {
    return null;
  }
  const recipients = list.split(",").map((r) => r.trim()).filter(Boolean);
  if (!recipients.length || !recipients.every((r) => RECIPIENT.test(r))) return null;

  return {
    to: recipients.join(","),
    subject: url.searchParams.get("subject") ?? "",
    body: url.searchParams.get("body") ?? "",
  };
}

const query = (pairs) =>
  pairs
    .filter(([, value]) => value)
    .map(([key, value]) => `${key}=${encodeURIComponent(value)}`)
    .join("&");

/**
 * The webmail compose links for one message, in the order they are offered.
 * @param {{ to: string, subject?: string, body?: string }} message
 * @returns {{ id: string, label: string, note: string, href: string }[]}
 */
export function webmailLinks({ to, subject = "", body = "" }) {
  const outlook = query([["to", to], ["subject", subject], ["body", body]]);
  return [
    {
      id: "outlook",
      label: "Outlook",
      note: "Microsoft 365, work or school",
      href: `https://outlook.office.com/mail/deeplink/compose?${outlook}`,
    },
    {
      id: "outlook-live",
      label: "Outlook.com",
      note: "Personal Microsoft account",
      href: `https://outlook.live.com/mail/0/deeplink/compose?${outlook}`,
    },
    {
      id: "gmail",
      label: "Gmail",
      note: "Google account",
      href: `https://mail.google.com/mail/?${query([["view", "cm"], ["fs", "1"], ["to", to], ["su", subject], ["body", body]])}`,
    },
    {
      id: "yahoo",
      label: "Yahoo Mail",
      note: "Yahoo account",
      href: `https://compose.mail.yahoo.com/?${query([["to", to], ["subject", subject], ["body", body]])}`,
    },
  ];
}

/* ---- The dialog -------------------------------------------------------- */

const remembered = () => {
  try {
    return localStorage.getItem(STORAGE_KEY) || "";
  } catch {
    return "";
  }
};
const remember = (id) => {
  try {
    localStorage.setItem(STORAGE_KEY, id);
  } catch {
    /* private mode: the choice is simply not kept */
  }
};

/** A touch-first device: leave the link alone, its mail app is the right answer. */
const touchFirst = () => window.matchMedia?.("(pointer: coarse)").matches === true;

function loadStylesheet() {
  if (document.querySelector(`link[href="${STYLESHEET}"]`)) return;
  document.head.append(el("link", { rel: "stylesheet", href: STYLESHEET }));
}

async function copyText(text, fallbackTarget) {
  try {
    await navigator.clipboard.writeText(text);
    return true;
  } catch {
    // No clipboard permission or an insecure origin: select the visible address
    // and ask the browser to copy it, which works from a click.
    const range = document.createRange();
    range.selectNodeContents(fallbackTarget);
    const selection = window.getSelection();
    selection.removeAllRanges();
    selection.addRange(range);
    try {
      return document.execCommand("copy");
    } catch {
      return false;
    }
  }
}

function buildDialog() {
  const address = el("strong", { class: "mailbox__addr" });
  const list = el("ul", { class: "mailbox__list" });
  const status = el("p", { class: "mailbox__status", role: "status" });
  const copy = el("button", { class: "btn btn-quiet", type: "button", text: "Copy address" });
  const close = el("button", { class: "btn", type: "button", text: "Close" });

  const dialog = el("dialog", { class: "mailbox", "aria-labelledby": "mailbox-title" }, [
    el("div", { class: "mailbox__panel" }, [
      el("h2", { class: "mailbox__title", id: "mailbox-title", text: "Send an email" }),
      el("p", { class: "mailbox__lead" }, [
        "Choose the mail app you use, and a new message to ",
        address,
        " opens there.",
      ]),
      list,
      el("div", { class: "mailbox__foot" }, [copy, close]),
      status,
    ]),
  ]);

  close.addEventListener("click", () => dialog.close());
  // The panel fills the dialog, so a click that lands on the dialog itself is
  // a click on the backdrop.
  dialog.addEventListener("click", (event) => {
    if (event.target === dialog) dialog.close();
  });
  copy.addEventListener("click", async () => {
    const ok = await copyText(address.textContent, address);
    status.textContent = ok
      ? `Copied ${address.textContent}`
      : "Could not copy. Select the address above and copy it.";
  });

  document.body.append(dialog);
  return { dialog, address, list, status };
}

export function initMailChooser() {
  if (typeof HTMLDialogElement === "undefined") return;
  loadStylesheet();

  let parts = null;

  function show(message, original) {
    parts ??= buildDialog();
    const { dialog, address, list, status } = parts;

    address.textContent = message.to;
    status.textContent = "";

    const last = remembered();
    const options = [
      {
        id: "native",
        label: "Your email app",
        note: "Whatever is set as default",
        href: original,
        native: true,
      },
      ...webmailLinks(message),
    ];
    options.sort((a, b) => (b.id === last) - (a.id === last));

    list.replaceChildren(
      ...options.map((option) => {
        const link = el(
          "a",
          {
            class: "mailbox__option",
            href: option.href,
            // The first option is the original link; it must not be caught
            // by the handler below and reopen this dialog.
            "data-mailto-native": option.native ? "" : null,
            target: option.native ? null : "_blank",
            rel: option.native ? null : "noopener noreferrer",
          },
          [
            el("strong", { text: option.label }),
            el("span", { text: option.id === last ? "Last used" : option.note }),
          ]
        );
        link.addEventListener("click", () => {
          remember(option.id);
          // Let the browser follow the link first, then put the dialog away.
          setTimeout(() => dialog.close(), 0);
        });
        return el("li", {}, link);
      })
    );

    if (!dialog.open) dialog.showModal();
  }

  document.addEventListener("click", (event) => {
    if (event.defaultPrevented || event.button !== 0) return;
    if (event.metaKey || event.ctrlKey || event.shiftKey || event.altKey) return;
    if (!(event.target instanceof Element)) return;

    const link = event.target.closest('a[href^="mailto:" i]');
    if (!link || link.hasAttribute("data-mailto-native") || link.closest(".mailbox")) return;
    if (touchFirst()) return;

    const message = parseMailto(link.href);
    if (!message) return;

    event.preventDefault();
    show(message, link.getAttribute("href"));
  });
}
