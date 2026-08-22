"""Tracker agent: record outcomes and schedule follow-ups.

Production uses Celery/Redis for durable scheduled retries. Here we keep an
in-process record and expose the hook the orchestrator calls on resolution.
"""
from __future__ import annotations

import time
from typing import Any

from ..memory.db import CaseStatus, store

# Typical first-response times per category (hours) — informs follow-up cadence.
DEFAULT_FOLLOWUP_HOURS = {
    "food_delivery": 4,
    "e_commerce": 24,
    "subscription": 24,
    "default": 12,
}


def schedule_followup(case_id: str, category: str = "default") -> dict[str, Any]:
    hours = DEFAULT_FOLLOWUP_HOURS.get(category, DEFAULT_FOLLOWUP_HOURS["default"])
    due_at = time.time() + hours * 3600
    # In production this enqueues a Celery task; here we just record intent.
    return {"case_id": case_id, "followup_due_at": due_at, "hours": hours}


def record_resolution(case_id: str, outcome: str, refund_amount: float | None = None) -> None:
    store.update(
        case_id,
        status=CaseStatus.resolved.value,
        outcome=outcome,
        refund_amount=refund_amount,
    )
