"""Summaverick orchestrator.

A small async state machine wiring the agents together:

    intake -> policy_check -> proof_gap -> drafting
            -> (suggest: await approval | auto: engaging)
            -> engaging (executor negotiation) -> resolved / escalated / failed

Autonomy levels:
  * suggest    : produce a draft and stop; user approves, then a refund offer
                 is paused for the user to accept.
  * auto_send  : auto-approve the draft and engage; a refund offer is still
                 paused for the user to accept.
  * full_auto  : engage and auto-accept refund offers that meet the threshold
                 (offers below the threshold are paused for the user).
"""
from __future__ import annotations

from typing import Any

from ..connectors.base import BaseConnector
from ..connectors.mock import MockConnector
from ..memory.db import AutonomyLevel, CaseStatus, store
from ..utils import events
from . import (
    drafting_agent,
    executor_agent,
    intake_agent,
    policy_agent,
    proof_gap_agent,
    tracker_agent,
)

CATEGORY_BY_PLATFORM = {
    "zomato": "food_delivery", "swiggy": "food_delivery",
    "amazon": "e_commerce", "flipkart": "e_commerce",
    "netflix": "subscription", "hotstar": "subscription",
}


def _make_connector(case: dict[str, Any], demo_mode: bool) -> BaseConnector:
    """Return the connector for a case.

    Demo/default is the MockConnector. Real-platform connectors are not built
    here (see connectors/README.md); until one exists we always use the mock
    channel so the agent never touches a real account.
    """
    scenario = case.get("entities", {}).get("scenario", "cooperative")
    refund = case.get("entities", {}).get("amount") or 229.0
    return MockConnector(case["platform"], session_id=case["id"],
                         scenario=scenario, refund_amount=float(refund))


async def process_intake(
    *,
    user_id: str,
    platform: str,
    text: str | None = None,
    screenshot_path: str | None = None,
    supplied_proof: list[str] | None = None,
    autonomy_level: str = AutonomyLevel.suggest.value,
    desired_outcome: str = "a full refund",
    threshold: float | None = None,
    scenario: str | None = None,
) -> dict[str, Any]:
    """Run intake -> policy -> proof_gap -> drafting. Returns the case dict."""
    case = store.create_case(user_id, platform, autonomy_level,
                             screenshot_url=screenshot_path, threshold=threshold)
    cid = case["id"]
    store.update(cid, status=CaseStatus.processing.value)

    # 1. Intake
    intake = intake_agent.run(text=text, screenshot_path=screenshot_path, platform=platform)
    entities = intake["entities"]
    if scenario:
        entities["scenario"] = scenario
    entities["desired_outcome"] = desired_outcome  # remembered for re-drafts
    store.update(cid, issue_type=intake["issue_type"], entities=entities)

    # 2. Proof gap — if evidence is missing, stop and ask for it.
    gaps = proof_gap_agent.run(
        platform=platform, issue_type=intake["issue_type"], supplied_proof=supplied_proof or []
    )
    if gaps:
        store.update(cid, status=CaseStatus.awaiting_proof.value, proof_gaps=gaps)
        return _with_meta(cid, proof_gaps=gaps)

    # 3. Policy + draft
    return _draft_case(cid)


def _required_proof(platform: str, issue_type: str) -> list[str]:
    return policy_agent.load_policy(platform).get("required_proof", {}).get(issue_type, [])


def _with_meta(cid: str, proof_gaps: list[str] | None = None) -> dict[str, Any]:
    """Attach policy assessment + proof metadata to a case dict for the API."""
    case = store.get_case(cid)
    policy = policy_agent.run(platform=case["platform"], issue_type=case["issue_type"],
                              entities=case["entities"])
    return {
        **case,
        "policy": policy,
        "proof_gaps": proof_gaps if proof_gaps is not None else case.get("proof_gaps", []),
        "required_proof": _required_proof(case["platform"], case["issue_type"]),
    }


def _draft_case(cid: str) -> dict[str, Any]:
    """Generate the complaint draft and move the case to awaiting_approval."""
    case = store.get_case(cid)
    policy = policy_agent.run(platform=case["platform"], issue_type=case["issue_type"],
                              entities=case["entities"])
    draft = drafting_agent.run(
        platform=case["platform"],
        issue_type=case["issue_type"],
        entities=case["entities"],
        desired_outcome=case["entities"].get("desired_outcome", "a full refund"),
        policy_notes=policy.get("policy_notes", ""),
    )
    store.update(cid, draft=draft, status=CaseStatus.awaiting_approval.value, proof_gaps=[])
    return _with_meta(cid, proof_gaps=[])


async def add_proof(case_id: str, supplied_proof: list[str]) -> dict[str, Any]:
    """User supplied more evidence — re-check gaps and draft if satisfied."""
    case = store.get_case(case_id)
    if not case:
        raise ValueError(f"unknown case {case_id}")
    gaps = proof_gap_agent.run(platform=case["platform"], issue_type=case["issue_type"],
                               supplied_proof=supplied_proof)
    if gaps:
        store.update(case_id, status=CaseStatus.awaiting_proof.value, proof_gaps=gaps)
        return _with_meta(case_id, proof_gaps=gaps)
    return _draft_case(case_id)


async def accept_offer(case_id: str) -> dict[str, Any]:
    """User accepts a paused refund offer."""
    case = store.get_case(case_id)
    if not case:
        raise ValueError(f"unknown case {case_id}")
    if case["status"] != CaseStatus.awaiting_acceptance.value:
        return case  # nothing pending
    tracker_agent.record_resolution(case_id, "refund_accepted", case.get("refund_amount"))
    return store.get_case(case_id)


async def engage(case_id: str, demo_mode: bool = True) -> str:
    """Move an approved case into live negotiation. Streams SSE events.

    Idempotent: only a case in awaiting_approval may start; duplicate/concurrent
    calls (e.g. double approve, or approve racing the auto-engage task) are no-ops.
    """
    case = store.get_case(case_id)
    if not case:
        raise ValueError(f"unknown case {case_id}")
    if not case.get("draft"):
        raise ValueError("case has no approved draft to send")
    if case["status"] != CaseStatus.awaiting_approval.value:
        return case["status"]  # already engaging / terminal — ignore duplicate
    store.update(case_id, status=CaseStatus.engaging.value)  # claim it synchronously
    case = store.get_case(case_id)

    connector = _make_connector(case, demo_mode)
    try:
        result = await executor_agent.run(case, case["draft"], connector)
        category = CATEGORY_BY_PLATFORM.get(case["platform"], "default")
        tracker_note = f"followup scheduled ({category})"
        await events.publish(case_id, "tracker", {"note": tracker_note})
        return result
    finally:
        await connector.close()
        await events.finish(case_id)


async def process_and_engage(**kwargs: Any) -> dict[str, Any]:
    """Full run used by demo/auto modes: intake through resolution."""
    case = await process_intake(**kwargs)
    if case["status"] == CaseStatus.awaiting_proof.value:
        return case
    if kwargs.get("autonomy_level", AutonomyLevel.suggest.value) != AutonomyLevel.suggest.value:
        await engage(case["id"])
        return store.get_case(case["id"])
    return case
