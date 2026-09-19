/*
 * The contact form on the homepage.
 *
 * Every message the visitor reads follows the server's actual response:
 *
 *   201            "Thanks—your message has been received."
 *   4xx / 5xx      "Your message wasn't sent. Please try again..."
 *   never answered "We couldn't confirm whether your message was received..."
 *
 * Input is never cleared on a failure, and the retry button reuses the same
 * idempotency key, so retrying an uncertain submission cannot duplicate it.
 */

import { $, $$ } from "./lib/dom.js";

const SENDING = "Sending…";
const SUCCESS = "Thanks—your message has been received.";
const FAILED = "Your message wasn't sent. Please try again. Your details are still here.";
const UNCONFIRMED =
  "We couldn't confirm whether your message was received. Your details are still here.";

export function initLeadForm() {
  const form = $("[data-lead-form]");
  const status = $("[data-lead-status]");
  const submit = $("[data-lead-submit]");
  const retry = $("[data-lead-retry]");
  if (!form || !status || !submit) return;

  const errorFor = (name) => $(`[data-error-for="${name}"]`, form);
  const fieldFor = (name) => form.elements[name];
  let submissionKey = null;
  let inFlight = false;

  const say = (text) => {
    status.textContent = text;
    // Re-trigger the 120ms fade without a layout change.
    status.removeAttribute("data-changed");
    requestAnimationFrame(() => status.setAttribute("data-changed", "true"));
  };

  const clearErrors = () => {
    for (const node of $$("[data-error-for]", form)) node.textContent = "";
    for (const name of ["email", "message"]) {
      fieldFor(name)?.removeAttribute("aria-invalid");
    }
  };

  const showErrors = (fieldErrors) => {
    let first = null;
    for (const [name, text] of Object.entries(fieldErrors || {})) {
      const node = errorFor(name);
      const field = fieldFor(name);
      if (node) node.textContent = text;
      if (field) field.setAttribute("aria-invalid", "true");
      if (!first) first = field;
    }
    first?.focus();
  };

  const setBusy = (busy) => {
    inFlight = busy;
    submit.disabled = busy;
    submit.textContent = busy ? SENDING : "Send your message";
  };

  const showRetry = (show) => {
    if (retry) retry.hidden = !show;
  };

  async function send() {
    if (inFlight) return;
    clearErrors();

    const data = new FormData(form);

    // Honeypot filled: a bot. Answer normally and send nothing.
    if (String(data.get("company_url") || "").trim()) {
      say(SUCCESS);
      return;
    }

    const email = String(data.get("email") || "").trim();
    const message = String(data.get("message") || "").trim();
    const clientErrors = {};
    if (!email) clientErrors.email = "Enter an email address so we can reply.";
    if (!message) clientErrors.message = "Tell us a little about what you need.";
    if (Object.keys(clientErrors).length) {
      showErrors(clientErrors);
      say("");
      return;
    }

    if (!submissionKey) submissionKey = newKey();
    setBusy(true);
    say(SENDING);
    showRetry(false);

    let response;
    try {
      response = await fetch("/api/contact", {
        method: "POST",
        headers: { "content-type": "application/json" },
        credentials: "same-origin",
        body: JSON.stringify({
          name: String(data.get("name") || ""),
          email,
          organisation: String(data.get("organisation") || ""),
          intent: String(data.get("intent") || ""),
          message,
          idempotency_key: submissionKey,
        }),
      });
    } catch (e) {
      // The request never completed, so nothing is known about delivery.
      setBusy(false);
      say(UNCONFIRMED);
      showRetry(true);
      return;
    }

    let body = null;
    try {
      body = await response.json();
    } catch (e) {
      body = null;
    }
    setBusy(false);

    if (response.ok && body && body.ok) {
      say(SUCCESS);
      submissionKey = null;
      form.reset();
      showRetry(false);
      return;
    }

    if (response.status === 422 && body && body.fieldErrors) {
      showErrors(body.fieldErrors);
      say("");
      return;
    }

    if (body && body.message) {
      say(`${body.message} Your details are still here.`);
    } else {
      say(FAILED);
    }
    showRetry(true);
  }

  form.addEventListener("submit", (e) => {
    e.preventDefault();
    send();
  });
  retry?.addEventListener("click", () => send());
}

function newKey() {
  if (crypto?.randomUUID) return crypto.randomUUID();
  return `k${Date.now()}${Math.random().toString(16).slice(2)}`;
}
