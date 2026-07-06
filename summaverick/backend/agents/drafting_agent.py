"""Drafting agent: compose the complaint message via the reasoning NIM model."""
from __future__ import annotations

from typing import Any

from ..guardrails.filter import redact
from ..llm.nim_client import REASONING_MODEL, client

TONES = ("polite", "firm", "legal")

_SYSTEM = (
    "You are Summaverick, an autonomous customer-advocacy agent acting for a "
    "consumer. Write a concise, factual complaint to a support channel. Include "
    "the order id and amount if present. Never invent facts. Do not include the "
    "user's phone, email, or payment identifiers. Match the requested tone."
)


def run(
    *,
    platform: str,
    issue_type: str,
    entities: dict[str, Any],
    desired_outcome: str = "a full refund",
    tone: str = "polite",
    language: str = "English",
    policy_notes: str = "",
) -> str:
    tone = tone if tone in TONES else "polite"
    # Compact context block; the offline engine parses these `key: value` lines,
    # and the real model reads them as grounding facts.
    ctx_lines = [
        f"platform: {platform}",
        f"issue_type: {issue_type}",
        f"desired_outcome: {desired_outcome}",
        f"tone: {tone}",
        f"language: {language}",
    ]
    if entities.get("order_id"):
        ctx_lines.append(f"order_id: {entities['order_id']}")
    if entities.get("amount"):
        ctx_lines.append(f"amount: {entities['amount']}")
    if policy_notes:
        ctx_lines.append(f"policy_notes: {policy_notes}")

    user_msg = (
        f"Write the complaint message in {language} with a {tone} tone.\n"
        f"Use only these facts:\n" + "\n".join(ctx_lines)
    )
    draft = client.chat_completion(
        [{"role": "system", "content": _SYSTEM}, {"role": "user", "content": user_msg}],
        model=REASONING_MODEL,
        temperature=0.3,
    )
    # Defense in depth: strip any PII the model may have echoed.
    return redact(draft)
