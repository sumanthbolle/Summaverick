"""Policy agent: load a platform policy and judge refund/claim eligibility."""
from __future__ import annotations

import functools
import os
from datetime import datetime
from typing import Any

import yaml

PLATFORMS_DIR = os.getenv(
    "SUMMAVERICK_PLATFORMS",
    os.path.join(os.path.dirname(__file__), "..", "..", "platforms"),
)


@functools.lru_cache(maxsize=32)
def load_policy(platform: str) -> dict[str, Any]:
    path = os.path.join(PLATFORMS_DIR, f"{platform.lower()}.yaml")
    if not os.path.exists(path):
        return {}
    with open(path) as fh:
        return yaml.safe_load(fh) or {}


def _within_window(date_str: str | None, window_hours: int) -> bool | None:
    """True/False if we can parse the date, None if unknown."""
    if not date_str:
        return None
    for fmt in ("%Y-%m-%d", "%d/%m/%Y", "%d-%m-%Y", "%d/%m/%y", "%d-%m-%y"):
        try:
            dt = datetime.strptime(date_str, fmt)
            return (datetime.now() - dt).total_seconds() <= window_hours * 3600
        except ValueError:
            continue
    return None


def run(*, platform: str, issue_type: str, entities: dict[str, Any]) -> dict[str, Any]:
    policy_doc = load_policy(platform)
    policy = policy_doc.get("policy", {})
    eligible_reasons = policy.get("eligible_reasons", [])
    ineligible_reasons = policy.get("ineligible_reasons", [])
    window = policy.get("refund_window_hours", 24)

    reasons: list[str] = []
    eligible = True
    confidence = 0.5

    if not policy_doc:
        return {
            "eligible": None,
            "confidence": 0.0,
            "reasons": [f"No policy on file for '{platform}'."],
            "policy_notes": "",
            "max_auto_refund": None,
        }

    if issue_type in eligible_reasons:
        reasons.append(f"'{issue_type}' is a covered reason under {platform} policy.")
        confidence = 0.85
    elif issue_type in ineligible_reasons:
        eligible = False
        reasons.append(f"'{issue_type}' is explicitly not covered.")
        confidence = 0.8
    else:
        reasons.append(f"'{issue_type}' is not listed as covered; needs human judgement.")
        confidence = 0.5

    in_window = _within_window(entities.get("date"), window)
    if in_window is False:
        eligible = False
        reasons.append(f"Outside the {window}h refund window.")
        confidence = max(confidence, 0.75)
    elif in_window is True:
        reasons.append(f"Within the {window}h refund window.")

    return {
        "eligible": eligible,
        "confidence": round(confidence, 2),
        "reasons": reasons,
        "policy_notes": policy.get("notes", ""),
        "max_auto_refund": policy.get("max_auto_refund"),
    }
