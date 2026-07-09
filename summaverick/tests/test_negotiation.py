"""Autonomy & acceptance semantics — the guarantees the product is built on."""
import os
import tempfile

os.environ["SUMMAVERICK_DB"] = os.path.join(tempfile.mkdtemp(), "neg.db")

import pytest  # noqa: E402

from backend.agents import orchestrator  # noqa: E402
from backend.memory.db import AutonomyLevel, CaseStatus, store  # noqa: E402


async def _run(autonomy, threshold=None, scenario="cooperative"):
    return await orchestrator.process_and_engage(
        user_id="u", platform="swiggy",
        text="Swiggy order #SWG9F2K31 missing item ₹229",
        supplied_proof=["order_confirmation"],
        autonomy_level=autonomy, threshold=threshold, scenario=scenario,
    )


@pytest.mark.asyncio
async def test_suggest_pauses_on_offer_not_auto_accepted():
    # suggest engages after approval but must NOT auto-accept the refund.
    case = await _run(AutonomyLevel.suggest.value)
    # suggest returns at awaiting_approval (process_and_engage doesn't auto-engage)
    assert case["status"] == CaseStatus.awaiting_approval.value
    # Approve → engage → should pause at the offer, not resolve.
    await orchestrator.engage(case["id"])
    c = store.get_case(case["id"])
    assert c["status"] == CaseStatus.awaiting_acceptance.value
    assert c["refund_amount"] == 229.0
    assert c["outcome"] is None  # NOT accepted yet


@pytest.mark.asyncio
async def test_auto_send_pauses_on_offer():
    case = await _run(AutonomyLevel.auto_send.value)
    assert case["status"] == CaseStatus.awaiting_acceptance.value
    assert case["refund_amount"] == 229.0
    assert case["outcome"] is None


@pytest.mark.asyncio
async def test_full_auto_accepts_above_threshold():
    case = await _run(AutonomyLevel.full_auto.value, threshold=100.0)
    assert case["status"] == CaseStatus.resolved.value
    assert case["refund_amount"] == 229.0


@pytest.mark.asyncio
async def test_full_auto_pauses_lowball_below_threshold():
    # Offer 229 but user requires >= 1000 → must pause, not accept.
    case = await _run(AutonomyLevel.full_auto.value, threshold=1000.0)
    assert case["status"] == CaseStatus.awaiting_acceptance.value
    assert case["outcome"] is None


@pytest.mark.asyncio
async def test_accept_offer_resolves():
    case = await _run(AutonomyLevel.auto_send.value)
    assert case["status"] == CaseStatus.awaiting_acceptance.value
    resolved = await orchestrator.accept_offer(case["id"])
    assert resolved["status"] == CaseStatus.resolved.value
    assert resolved["outcome"] == "refund_accepted"
    assert resolved["refund_amount"] == 229.0


@pytest.mark.asyncio
async def test_double_engage_is_noop():
    case = await orchestrator.process_intake(
        user_id="u", platform="swiggy",
        text="Swiggy order #SWG222 missing item ₹229",
        supplied_proof=["order_confirmation"],
        autonomy_level=AutonomyLevel.suggest.value,
    )
    await orchestrator.engage(case["id"])
    status_after_first = store.get_case(case["id"])["status"]
    # Second engage must be a no-op (already past awaiting_approval).
    result = await orchestrator.engage(case["id"])
    assert result == status_after_first


@pytest.mark.asyncio
async def test_proof_resume_flow():
    case = await orchestrator.process_intake(
        user_id="u", platform="zomato",
        text="Zomato order #ZOM999 missing item ₹150",
        supplied_proof=[],
    )
    assert case["status"] == CaseStatus.awaiting_proof.value
    assert case["required_proof"]  # UI needs the raw keys
    # Supply the required proof → should proceed to a draft.
    resumed = await orchestrator.add_proof(case["id"], case["required_proof"])
    assert resumed["status"] == CaseStatus.awaiting_approval.value
    assert resumed["draft"]
