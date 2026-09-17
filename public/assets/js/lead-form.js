/*
 * The contact form on the homepage. Validation happens next to the field the
 * visitor is filling in, and the messages say what to do rather than what
 * went wrong.
 */

import { $ } from "./lib/dom.js";

export function initLeadForm() {
  const form = $("[data-lead-form]");
  const status = $("[data-lead-status]");
  if (!form || !status) return;

  form.addEventListener("submit", (e) => {
    e.preventDefault();
    const data = new FormData(form);

    // Honeypot: a bot filled the hidden field, so answer normally and stop.
    if (data.get("company_url")) {
      status.textContent = "Thanks. Your message is on its way.";
      form.reset();
      return;
    }

    const email = String(data.get("email") || "").trim();
    if (!email) {
      status.textContent = "Enter an email address so we can reply.";
      form.querySelector("#lead-email")?.focus();
      return;
    }

    status.textContent = "Thanks. Your message is on its way.";
    form.reset();
  });
}
