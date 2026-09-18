/*
 * The project-enquiry form on the homepage.
 *
 * It submits to POST /api/leads and reports only what that call actually
 * returned. Validation happens next to the field the visitor is filling in,
 * and the messages say what to do rather than what went wrong. Nothing is
 * cleared until the server has the message, so a failure never costs anyone
 * what they typed.
 */

import { $ } from "./lib/dom.js";

const FIELD_MESSAGES = {
  email: "Enter an email address so we can reply.",
  message: "Tell us a little about the work — a sentence is enough.",
};

function fieldOf(input) {
  return input?.closest(".field") ?? null;
}

function showFieldError(form, name, text) {
  const input = form.elements[name];
  const error = $(`#lead-${name}-error`, form);
  if (!input || !error) return;
  error.textContent = text;
  error.hidden = false;
  input.setAttribute("aria-invalid", "true");
  fieldOf(input)?.setAttribute("data-invalid", "true");
}

function clearFieldError(form, name) {
  const input = form.elements[name];
  const error = $(`#lead-${name}-error`, form);
  if (!input || !error) return;
  error.textContent = "";
  error.hidden = true;
  input.removeAttribute("aria-invalid");
  fieldOf(input)?.removeAttribute("data-invalid");
}

function setStatus(status, text, tone) {
  status.textContent = text;
  if (tone) status.setAttribute("data-tone", tone);
  else status.removeAttribute("data-tone");
}

export function initLeadForm() {
  const form = $("[data-lead-form]");
  const status = $("[data-lead-status]");
  const submit = $("[data-lead-submit]", form ?? document);
  if (!form || !status || !submit) return;

  const label = submit.textContent;
  let sending = false;

  // The markup carries a real action and method so the form works without
  // this script. Now that it has loaded, native validation steps aside for
  // the per-field messages below.
  form.noValidate = true;

  // Clear an error as soon as the visitor starts fixing it.
  for (const name of Object.keys(FIELD_MESSAGES)) {
    form.elements[name]?.addEventListener("input", () => clearFieldError(form, name));
  }

  form.addEventListener("submit", async (event) => {
    event.preventDefault();
    if (sending) return;

    const data = new FormData(form);
    const payload = {
      name: String(data.get("name") || "").trim(),
      email: String(data.get("email") || "").trim(),
      organisation: String(data.get("organisation") || "").trim(),
      intent: String(data.get("intent") || "unsure"),
      message: String(data.get("message") || "").trim(),
      company_url: String(data.get("company_url") || ""),
    };

    // Check both fields before focusing, so the visitor sees every problem at
    // once rather than one per attempt.
    const invalid = [];
    for (const name of Object.keys(FIELD_MESSAGES)) clearFieldError(form, name);
    if (!payload.email || !/^[^\s@]+@[^\s@.]+(\.[^\s@.]+)+$/.test(payload.email)) {
      showFieldError(form, "email", FIELD_MESSAGES.email);
      invalid.push(form.elements.email);
    }
    if (payload.message.length < 10) {
      showFieldError(form, "message", FIELD_MESSAGES.message);
      invalid.push(form.elements.message);
    }
    if (invalid.length) {
      setStatus(
        status,
        invalid.length === 1 ? "One thing to fix above." : "Two things to fix above.",
        "bad"
      );
      invalid[0].focus();
      return;
    }

    sending = true;
    submit.disabled = true;
    submit.textContent = "Sending…";
    setStatus(status, "Sending your message…");

    try {
      const res = await fetch("/api/leads", {
        method: "POST",
        headers: { "content-type": "application/json" },
        credentials: "same-origin",
        body: JSON.stringify(payload),
      });
      const body = await res.json().catch(() => ({}));

      if (res.ok && body.ok !== false) {
        form.reset();
        setStatus(
          status,
          `Message received. Sumanth will reply to ${payload.email}. Your reference is ${body.reference ?? "on file"}.`,
          "ok"
        );
        return;
      }

      // Everything below keeps what the visitor typed, so Send is all that is
      // needed to try again.
      if (res.status === 429) {
        setStatus(status, body.message || "Too many messages for now. Try again shortly.", "bad");
        return;
      }
      if (res.status === 400 && body.message) {
        setStatus(status, body.message, "bad");
        return;
      }
      setStatus(
        status,
        "That did not go through, and your message is still here. Try Send again.",
        "bad"
      );
    } catch {
      setStatus(
        status,
        "We could not reach the server. Your message is still here — try Send again when you are back online.",
        "bad"
      );
    } finally {
      sending = false;
      submit.disabled = false;
      submit.textContent = label;
    }
  });
}
