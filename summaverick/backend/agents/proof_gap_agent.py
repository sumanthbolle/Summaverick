"""Proof-gap agent: compare required proof against what the user supplied."""
from __future__ import annotations

from typing import Any

from .policy_agent import load_policy

# Human-readable prompts for the evidence we might still need.
_PROMPTS = {
    "order_confirmation": "the order confirmation (email or in-app receipt)",
    "photo_of_received_items": "a photo of the items you actually received",
    "photo_of_wrong_item": "a photo of the wrong item you received",
    "photo_of_food": "a photo of the food showing the quality issue",
}


def run(*, platform: str, issue_type: str, supplied_proof: list[str]) -> list[str]:
    """Return a list of human-readable requests for missing evidence."""
    policy_doc = load_policy(platform)
    required = policy_doc.get("required_proof", {}).get(issue_type, [])
    supplied = set(supplied_proof or [])
    missing = [item for item in required if item not in supplied]
    return [f"Please provide {_PROMPTS.get(item, item.replace('_', ' '))}." for item in missing]
