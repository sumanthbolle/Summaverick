"""Lightweight content guardrails.

A rule-based stand-in for NeMo Guardrails. Its jobs:
  * redact PII (phone, email, UPI id, card-like numbers) from anything we log;
  * block a message from being *sent* if it leaks PII the user has not
    explicitly approved for that case.

This is deliberately conservative: we would rather over-redact a log line than
leak a phone number into telemetry.
"""
from __future__ import annotations

import re
from dataclasses import dataclass, field

_PATTERNS = {
    "email": re.compile(r"\b[\w.+-]+@[\w-]+\.[\w.-]+\b"),
    "upi": re.compile(r"\b[\w.-]{2,}@(?:oksbi|okhdfcbank|okicici|okaxis|ybl|paytm|apl|ibl)\b", re.I),
    "phone": re.compile(r"(?<!\d)(?:\+?91[\-\s]?)?[6-9]\d{9}(?!\d)"),
    "card": re.compile(r"(?<!\d)(?:\d[ -]?){13,16}(?!\d)"),
}

# Order/reference ids are needed for the complaint, so we never treat them as PII.
_ALLOWED_HINTS = ("order", "ref", "txn", "booking")


def redact(text: str) -> str:
    """Return ``text`` with PII replaced by typed placeholders."""
    if not text:
        return text
    out = text
    # UPI before email so the @handle form is caught first.
    out = _PATTERNS["upi"].sub("[UPI_REDACTED]", out)
    out = _PATTERNS["email"].sub("[EMAIL_REDACTED]", out)
    out = _PATTERNS["phone"].sub("[PHONE_REDACTED]", out)
    out = _PATTERNS["card"].sub("[CARD_REDACTED]", out)
    return out


@dataclass
class GuardResult:
    allowed: bool
    reason: str = ""
    findings: list[str] = field(default_factory=list)


def check_outbound(text: str, approved_pii: bool = False) -> GuardResult:
    """Decide whether ``text`` is safe to send to a support channel."""
    findings: list[str] = []
    for name, pat in _PATTERNS.items():
        if name == "card":
            # Skip matches that are clearly order/reference numbers.
            for m in pat.finditer(text):
                window = text[max(0, m.start() - 24):m.start()].lower()
                if not any(h in window for h in _ALLOWED_HINTS):
                    findings.append(name)
                    break
        elif pat.search(text):
            findings.append(name)

    if findings and not approved_pii:
        return GuardResult(
            allowed=False,
            reason=f"Message contains unapproved PII: {', '.join(sorted(set(findings)))}",
            findings=sorted(set(findings)),
        )
    return GuardResult(allowed=True)
