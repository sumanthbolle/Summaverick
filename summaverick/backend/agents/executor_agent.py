"""Executor agent + negotiation loop.

Drives a live (mock) support channel: sends the approved complaint, then runs a
multi-turn negotiation — classifying each bot reply and choosing an action —
until the case resolves, escalates to a human, or hits a turn cap.

All outbound messages pass the guardrail check. Every exchange is transcribed
and streamed as SSE events.
"""
from __future__ import annotations

import asyncio
from typing import Any

from ..connectors.base import BaseConnector
from ..guardrails.filter import check_outbound, redact
from ..llm.nim_client import LIGHT_MODEL, client
from ..memory.db import CaseStatus, store
from ..utils import events
from . import drafting_agent, tracker_agent

BOT_INTENTS = [
    "greeting", "ask_order_id", "ask_issue_description", "ask_proof",
    "processing", "offer_refund", "deny", "loop", "transfer_human", "close",
]

MAX_TURNS = 12
LOOP_THRESHOLD = 3          # identical replies before we demand a human
RESPONSE_TIMEOUT = 30.0     # seconds to wait for a channel reply


class NegotiationLoop:
    def __init__(self, case: dict[str, Any], connector: BaseConnector) -> None:
        self.case = case
        self.case_id = case["id"]
        self.connector = connector
        self.entities = case.get("entities", {})
        self.platform = case["platform"]
        self.issue_type = case.get("issue_type", "issue")
        self.threshold = case.get("threshold")
        self.autonomy = case.get("autonomy_level", "suggest")
        self.tone = "polite"
        self._recent_bot: list[str] = []

    async def _send(self, text: str, approved_pii: bool = False) -> bool:
        guard = check_outbound(text, approved_pii=approved_pii)
        if not guard.allowed:
            # Never send unapproved PII; strip it and continue.
            text = redact(text)
        await self.connector.send_message(text)
        store.append_transcript(self.case_id, "agent", text)
        await events.publish(self.case_id, "agent_sent", {"message": text})
        return True

    async def _read(self) -> dict[str, Any]:
        try:
            resp = await asyncio.wait_for(self.connector.read_latest_response(), RESPONSE_TIMEOUT)
        except asyncio.TimeoutError:
            return {"text": "", "intent": "processing", "resolved": False}
        return resp

    def _classify(self, resp: dict[str, Any]) -> str:
        # Track every reply so loop detection works even when the channel
        # supplies its own intent hint (the mock bot always does).
        text = resp.get("text", "")
        self._recent_bot.append(text)
        if (text and len(self._recent_bot) >= LOOP_THRESHOLD
                and len(set(self._recent_bot[-LOOP_THRESHOLD:])) == 1):
            return "loop"
        # Trust the channel's own intent hint if present, else classify.
        if resp.get("intent") in BOT_INTENTS:
            return resp["intent"]
        return client.classify(text, BOT_INTENTS, model=LIGHT_MODEL)

    def _escalate_tone(self) -> None:
        self.tone = {"polite": "firm", "firm": "legal", "legal": "legal"}[self.tone]

    async def _regenerate(self, desired_outcome: str) -> str:
        return drafting_agent.run(
            platform=self.platform,
            issue_type=self.issue_type,
            entities=self.entities,
            desired_outcome=desired_outcome,
            tone=self.tone,
            policy_notes=self.case.get("policy_notes", ""),
        )

    async def _act(self, intent: str, resp: dict[str, Any]) -> str | None:
        """Return the next message to send, or None to stop the loop."""
        if intent in ("ask_order_id",):
            oid = self.entities.get("order_id")
            return f"My order ID is {oid}." if oid else \
                "I don't have the order ID handy, but here are the order details on file."
        if intent == "ask_issue_description":
            return f"The issue is: {self.issue_type.replace('_', ' ')}."
        if intent == "ask_proof":
            return "The relevant proof is attached to this case."
        if intent == "processing":
            return None  # wait for the next update without spamming
        if intent == "deny":
            self._escalate_tone()
            await events.publish(self.case_id, "escalating", {"tone": self.tone})
            store.update(self.case_id, status=CaseStatus.escalated.value)
            return await self._regenerate("a full refund as required by policy")
        if intent == "loop":
            await events.publish(self.case_id, "escalating", {"reason": "repeated_replies"})
            return "I want to speak to a human representative, please."
        if intent == "transfer_human":
            return await self._regenerate("a clear, human-friendly summary and a refund")
        # offer_refund and close are terminal — handled directly in run().
        # greeting / unknown -> keep momentum
        return None

    def _should_auto_accept(self, amount: float | None) -> bool:
        """Only full_auto accepts on the agent's own authority.

        suggest / auto_send always pause a refund offer for the user. full_auto
        accepts an offer that meets the user's minimum threshold; a lowball below
        the threshold is paused for the user. Unset threshold = accept anything
        (the user opted into full autonomy).
        """
        if self.autonomy != "full_auto":
            return False
        if self.threshold is None or amount is None:
            return True
        return amount >= self.threshold

    async def run(self, draft: str) -> str:
        await events.publish(self.case_id, "channel_opened", {})
        greeting = await self.connector.open_support_channel()
        if greeting:
            store.append_transcript(self.case_id, "bot", greeting, intent="greeting")
            await events.publish(self.case_id, "bot_replied", {"message": greeting, "intent": "greeting"})

        await self._send(draft)

        for _ in range(MAX_TURNS):
            resp = await self._read()
            text = resp.get("text", "")
            intent = self._classify(resp)
            if text:
                store.append_transcript(self.case_id, "bot", text, intent=intent)
                await events.publish(self.case_id, "bot_replied", {"message": text, "intent": intent})

            # --- terminal intents --------------------------------------- #
            if intent == "offer_refund":
                amt = resp.get("refund_amount")
                if self._should_auto_accept(amt):
                    tracker_agent.record_resolution(self.case_id, "refund_accepted", amt)
                    await events.publish(self.case_id, "resolved", {"refund_amount": amt})
                    return "resolved"
                # Not ours to accept — pause and hand the decision to the user.
                store.update(self.case_id, status=CaseStatus.awaiting_acceptance.value,
                             refund_amount=amt)
                await events.publish(self.case_id, "offer", {"refund_amount": amt})
                return CaseStatus.awaiting_acceptance.value
            if intent == "close":
                break  # bot closed without an acceptable offer

            # --- non-terminal: decide the next message ------------------ #
            next_msg = await self._act(intent, resp)
            if next_msg:
                await self._send(next_msg)
            else:
                # Nothing to say (e.g. processing) — brief wait, then poll again.
                await asyncio.sleep(0.1)

        # Loop ended without an accepted refund → hand off to a human.
        current = store.get_case(self.case_id)
        if current and current["status"] not in (
            CaseStatus.resolved.value, CaseStatus.awaiting_acceptance.value
        ):
            store.update(self.case_id, status=CaseStatus.escalated.value)
            await events.publish(self.case_id, "needs_human", {})
        return current["status"] if current else "failed"


async def run(case: dict[str, Any], draft: str, connector: BaseConnector) -> str:
    store.update(case["id"], status=CaseStatus.engaging.value)
    await events.publish(case["id"], "status", {"status": "engaging"})
    loop = NegotiationLoop(case, connector)
    try:
        return await loop.run(draft)
    except Exception as exc:  # keep the case consistent on failure
        store.update(case["id"], status=CaseStatus.failed.value, outcome=f"error: {exc}")
        await events.publish(case["id"], "failed", {"error": str(exc)})
        return "failed"
