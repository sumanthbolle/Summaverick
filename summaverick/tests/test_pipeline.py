"""End-to-end tests for the offline (no-NIM) pipeline and negotiation loop."""
import os
import tempfile

import pytest

# Isolate the DB per test session.
os.environ["SUMMAVERICK_DB"] = os.path.join(tempfile.mkdtemp(), "test.db")

from backend.agents import intake_agent, orchestrator, policy_agent, proof_gap_agent  # noqa: E402
from backend.guardrails.filter import check_outbound, redact  # noqa: E402
from backend.memory.db import AutonomyLevel, CaseStatus, store  # noqa: E402


def test_intake_classifies_and_extracts():
    out = intake_agent.run(
        text="Swiggy order #SWG12345 missing item, ₹229 paid", platform="swiggy"
    )
    assert out["issue_type"] == "missing_item"
    assert out["entities"]["order_id"] == "SWG12345"
    assert out["entities"]["amount"] == 229.0


def test_policy_eligibility():
    res = policy_agent.run(platform="swiggy", issue_type="missing_item", entities={})
    assert res["eligible"] is True
    assert res["max_auto_refund"] == 500


def test_proof_gap_detects_missing():
    gaps = proof_gap_agent.run(platform="zomato", issue_type="missing_item", supplied_proof=[])
    assert len(gaps) == 2
    none_missing = proof_gap_agent.run(
        platform="swiggy", issue_type="missing_item", supplied_proof=["order_confirmation"]
    )
    assert none_missing == []


def test_guardrails_redaction_and_block():
    text = "call me at 9876543210 or pay me at foo@okhdfcbank"
    assert "[PHONE_REDACTED]" in redact(text)
    assert "[UPI_REDACTED]" in redact(text)
    assert check_outbound(text).allowed is False
    assert check_outbound("Order ORD12345678901 issue").allowed is True


@pytest.mark.asyncio
async def test_intake_stops_for_proof_gap():
    case = await orchestrator.process_intake(
        user_id="u", platform="zomato",
        text="Zomato order #ZOM777888 missing item ₹150",
        supplied_proof=[],
    )
    assert case["status"] == CaseStatus.awaiting_proof.value
    assert case["proof_gaps"]


@pytest.mark.asyncio
async def test_full_auto_resolves():
    case = await orchestrator.process_and_engage(
        user_id="u", platform="swiggy",
        text="Swiggy order #SWG9F2K31 missing item ₹229",
        supplied_proof=["order_confirmation"],
        autonomy_level=AutonomyLevel.full_auto.value,
        threshold=0.0,
        scenario="cooperative",
    )
    assert case["status"] == CaseStatus.resolved.value
    assert case["refund_amount"] == 229.0
    roles = [t["role"] for t in case["transcript"]]
    assert "agent" in roles and "bot" in roles


@pytest.mark.asyncio
async def test_stubborn_triggers_escalation_then_resolves():
    case = await orchestrator.process_and_engage(
        user_id="u", platform="swiggy",
        text="Swiggy order #SWG55512 missing item ₹300",
        supplied_proof=["order_confirmation"],
        autonomy_level=AutonomyLevel.full_auto.value,
        threshold=0.0,
        scenario="stubborn",
    )
    # A stubborn bot denies first; the loop escalates tone and should still close.
    assert case["status"] in (CaseStatus.resolved.value, CaseStatus.escalated.value)
    intents = [t.get("intent") for t in case["transcript"]]
    assert "deny" in intents
