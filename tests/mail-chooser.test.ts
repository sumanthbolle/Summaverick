/**
 * The mail chooser's parsing and link building. The dialog itself runs in a
 * browser (see the check in the pull request); what is worth pinning here is
 * the part that decides where a message goes: which links it recognises, and
 * that nothing in a subject or body can change a compose URL's other fields.
 */
import { describe, expect, it } from "vitest";
// Plain browser JavaScript, loaded by the site as-is; it has no declaration file.
// @ts-expect-error TS7016
import { parseMailto, webmailLinks } from "../public/assets/js/mail-chooser.js";

type Message = { to: string; subject: string; body: string };
type Link = { id: string; label: string; note: string; href: string };

describe("parseMailto", () => {
  it("reads a plain address", () => {
    expect(parseMailto("mailto:hello@summaverick.com")).toEqual({
      to: "hello@summaverick.com",
      subject: "",
      body: "",
    });
  });

  it("keeps a link's own subject, which the research page sets", () => {
    const m: Message = parseMailto(
      "mailto:hello@summaverick.com?subject=Summaverick%20Research%20for%20ServiceNow"
    );
    expect(m.to).toBe("hello@summaverick.com");
    expect(m.subject).toBe("Summaverick Research for ServiceNow");
  });

  it("decodes a percent-encoded address and a multi-line body", () => {
    const m: Message = parseMailto("mailto:ceo%40summaverick.com?body=Hello%0A%0AThanks");
    expect(m.to).toBe("ceo@summaverick.com");
    expect(m.body).toBe("Hello\n\nThanks");
  });

  it("accepts more than one recipient", () => {
    expect(parseMailto("mailto:a@example.com, b@example.org").to).toBe("a@example.com,b@example.org");
  });

  it("is not fooled by something that is not a mailto link", () => {
    expect(parseMailto("https://summaverick.com/")).toBeNull();
    expect(parseMailto("javascript:alert(1)")).toBeNull();
    expect(parseMailto("tel:+911234567890")).toBeNull();
    expect(parseMailto("")).toBeNull();
    expect(parseMailto("/relative")).toBeNull();
  });

  it("leaves a link alone when its recipient is not an address", () => {
    for (const bad of [
      "mailto:",
      "mailto:nobody",
      "mailto:a@b",
      "mailto:a b@example.com",
      "mailto:ok@example.com,not-an-address",
      "mailto:<x>@example.com",
    ]) {
      expect(parseMailto(bad), bad).toBeNull();
    }
  });
});

describe("webmailLinks", () => {
  const message = { to: "hello@summaverick.com", subject: "Summaverick Research for ServiceNow", body: "" };
  const links: Link[] = webmailLinks(message);
  const byId = (id: string) => new URL(links.find((l) => l.id === id)!.href);

  it("offers Outlook first, then Outlook.com, Gmail and Yahoo", () => {
    expect(links.map((l) => l.id)).toEqual(["outlook", "outlook-live", "gmail", "yahoo"]);
    expect(links.map((l) => l.label)).toEqual(["Outlook", "Outlook.com", "Gmail", "Yahoo Mail"]);
  });

  it("points each at its own compose window", () => {
    expect(byId("outlook").origin + byId("outlook").pathname).toBe(
      "https://outlook.office.com/mail/deeplink/compose"
    );
    expect(byId("outlook-live").origin + byId("outlook-live").pathname).toBe(
      "https://outlook.live.com/mail/0/deeplink/compose"
    );
    expect(byId("gmail").origin + byId("gmail").pathname).toBe("https://mail.google.com/mail/");
    expect(byId("yahoo").origin).toBe("https://compose.mail.yahoo.com");
  });

  it("fills in the recipient and subject under each service's own parameter names", () => {
    for (const id of ["outlook", "outlook-live", "yahoo"]) {
      const p = byId(id).searchParams;
      expect(p.get("to"), id).toBe("hello@summaverick.com");
      expect(p.get("subject"), id).toBe("Summaverick Research for ServiceNow");
    }
    const g = byId("gmail").searchParams;
    expect(g.get("view")).toBe("cm");
    expect(g.get("to")).toBe("hello@summaverick.com");
    expect(g.get("su")).toBe("Summaverick Research for ServiceNow");
  });

  it("leaves out a subject and body that are empty", () => {
    const bare: Link[] = webmailLinks({ to: "ceo@summaverick.com" });
    for (const l of bare) {
      const p = new URL(l.href).searchParams;
      expect(p.has("subject") || p.has("su") || p.has("body"), l.id).toBe(false);
      expect(p.get("to"), l.id).toBe("ceo@summaverick.com");
    }
  });

  it("cannot be made to change another field through a subject or body", () => {
    const hostile = webmailLinks({
      to: "hello@summaverick.com",
      subject: "hi&to=attacker@example.com&bcc=x@example.com",
      body: "line one\nline two #fragment ?q=1",
    }) as Link[];
    for (const l of hostile) {
      const url = new URL(l.href);
      expect(url.searchParams.getAll("to"), l.id).toEqual(["hello@summaverick.com"]);
      expect(url.searchParams.has("bcc"), l.id).toBe(false);
      expect(url.hash, l.id).toBe("");
      const subject = url.searchParams.get("subject") ?? url.searchParams.get("su");
      expect(subject, l.id).toBe("hi&to=attacker@example.com&bcc=x@example.com");
      expect(url.searchParams.get("body"), l.id).toBe("line one\nline two #fragment ?q=1");
    }
  });

  it("only ever links to the four mail services over https", () => {
    for (const l of links) {
      const u = new URL(l.href);
      expect(u.protocol).toBe("https:");
      expect(["outlook.office.com", "outlook.live.com", "mail.google.com", "compose.mail.yahoo.com"]).toContain(u.hostname);
    }
  });
});
