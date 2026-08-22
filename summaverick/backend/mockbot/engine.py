"""Mock support-bot engine.

A deterministic rule engine that role-plays a platform support bot so the full
autonomous flow can be demonstrated without touching any real service or
account. It is stateful per ``session_id`` and drives a realistic arc:

    greeting -> ask_order_id -> processing -> offer_refund -> close

with a branch to ``deny`` (to exercise escalation) when configured.
"""
from __future__ import annotations

import re
import threading
from dataclasses import dataclass, field
from typing import Any


@dataclass
class _Session:
    platform: str
    scenario: str = "cooperative"   # or "stubborn" to force escalation
    step: int = 0
    order_id: str | None = None
    denials: int = 0
    resolved: bool = False
    refund_amount: float | None = None
    meta: dict[str, Any] = field(default_factory=dict)


class MockBot:
    def __init__(self) -> None:
        self._sessions: dict[str, _Session] = {}
        self._lock = threading.Lock()

    def start(self, session_id: str, platform: str, scenario: str = "cooperative",
              refund_amount: float = 229.0) -> str:
        with self._lock:
            self._sessions[session_id] = _Session(
                platform=platform, scenario=scenario, refund_amount=refund_amount
            )
        return f"Hi! Welcome to {platform.title()} support. How can I help you today?"

    def reply(self, session_id: str, user_message: str) -> dict[str, Any]:
        """Return {"text": ..., "intent": ..., "resolved": bool, "refund_amount": ...}."""
        with self._lock:
            sess = self._sessions.get(session_id)
            if sess is None:
                sess = self._sessions[session_id] = _Session(platform="unknown")

            # Capture an order id whenever the user provides one.
            m = re.search(r"(?:order\s*(?:id)?[:#]?\s*)?([A-Z0-9]{6,})", user_message, re.I)
            if m and any(c.isdigit() for c in m.group(1)):
                sess.order_id = m.group(1)

            wants_human = "human" in user_message.lower() or "representative" in user_message.lower()

            if wants_human:
                sess.step = 99
                return self._out("Sure, I'm connecting you to a human representative who "
                                 "can review this. Please hold.", "transfer_human", sess)

            # Stubborn scenario denies twice, then relents under a firm/legal tone.
            if sess.scenario == "stubborn" and sess.denials < 2 and sess.step >= 1:
                sess.denials += 1
                legalish = any(w in user_message.lower()
                               for w in ("consumer", "law", "formal complaint", "grievance", "escalate"))
                if not legalish:
                    return self._out(
                        "I'm sorry, but based on our records this doesn't appear eligible "
                        "for a refund. Is there anything else?", "deny", sess)

            # Cooperative arc (also the fall-through for stubborn once it relents).
            if sess.step == 0:
                sess.step = 1
                if sess.order_id:
                    return self._processing(sess)
                return self._out("I can help with that. Could you share your order ID, please?",
                                 "ask_order_id", sess)
            if sess.step == 1:
                if not sess.order_id:
                    return self._out("I still need your order ID to proceed.", "ask_order_id", sess)
                return self._processing(sess)
            if sess.step == 2:
                return self._offer(sess)
            return self._out("Your refund is confirmed. Is there anything else I can help with?",
                             "close", sess)

    # -- internal ------------------------------------------------------ #
    def _processing(self, sess: _Session) -> dict[str, Any]:
        sess.step = 2
        return self._out(f"Thank you. I've located order {sess.order_id}. "
                         "Please give me a moment while I check this — processing your request.",
                         "processing", sess)

    def _offer(self, sess: _Session) -> dict[str, Any]:
        sess.step = 3
        sess.resolved = True
        amt = sess.refund_amount or 229.0
        return self._out(f"Good news — I've approved a refund of ₹{amt:.0f} to your original "
                         "payment method. It will reflect in 24-48 hours.",
                         "offer_refund", sess)

    @staticmethod
    def _out(text: str, intent: str, sess: _Session) -> dict[str, Any]:
        return {
            "text": text,
            "intent": intent,
            "resolved": sess.resolved,
            "refund_amount": sess.refund_amount if sess.resolved else None,
        }


bot = MockBot()
