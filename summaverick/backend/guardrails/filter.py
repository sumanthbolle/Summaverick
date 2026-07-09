"""Lightweight content guardrails.

A rule-based stand-in for NeMo Guardrails. Its jobs:
  * redact PII (phone, email, UPI id, card-like numbers) from anything we log;
  * block a message from being *sent* if it leaks PII the user has not
    explicitly approved for that case.

Order / reference / transaction numbers are needed for the complaint, so a
numeric run immediately preceded by an "order"/"ref"/"txn"/"booking" hint is
NOT treated as a phone or card number. This keeps us from silently stripping
the order ID out of an outbound complaint.
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

# Patterns whose matches may legitimately be identifiers, not PII.
_HINT_EXEMPT = ("phone", "card")
_ALLOWED_HINTS = ("order", "ref", "txn", "transaction", "booking", "invoice")


def _preceded_by_hint(text: str, start: int) -> bool:
    window = text[max(0, start - 24):start].lower()
    return any(h in window for h in _ALLOWED_HINTS)


def redact(text: str) -> str:
    """Return ``text`` with PII replaced by typed placeholders."""
    if not text:
        return text
    out = text
    # UPI before email so the @handle form is caught first.
    out = _PATTERNS["upi"].sub("[UPI_REDACTED]", out)
    out = _PATTERNS["email"].sub("[EMAIL_REDACTED]", out)
    for name in _HINT_EXEMPT:
        placeholder = f"[{name.upper()}_REDACTED]"
        out = _PATTERNS[name].sub(
            lambda m: m.group(0) if _preceded_by_hint(m.string, m.start()) else placeholder,
            out,
        )
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
        if name in _HINT_EXEMPT:
            if any(not _preceded_by_hint(text, m.start()) for m in pat.finditer(text)):
                findings.append(name)
        elif pat.search(text):
            findings.append(name)

    if findings and not approved_pii:
        return GuardResult(
            allowed=False,
            reason=f"Message contains unapproved PII: {', '.join(sorted(set(findings)))}",
            findings=sorted(set(findings)),
        )
    return GuardResult(allowed=True)
