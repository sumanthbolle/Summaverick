"""HTTP surface tests via FastAPI TestClient."""
import os
import tempfile

os.environ["SUMMAVERICK_DB"] = os.path.join(tempfile.mkdtemp(), "api.db")

import pytest  # noqa: E402
from fastapi.testclient import TestClient  # noqa: E402

from backend.main import app  # noqa: E402

client = TestClient(app)


def _stream_events(case_id):
    evs, cur = [], None
    with client.stream("GET", f"/case/{case_id}/stream") as r:
        for line in r.iter_lines():
            if line.startswith("event:"):
                cur = line.split(":", 1)[1].strip()
            elif line.startswith("data:"):
                evs.append(cur)
            if cur == "done":
                break
    return evs


def test_health_and_root():
    assert client.get("/health").json()["status"] == "ok"
    r = client.get("/")
    assert r.status_code == 200 and "Summaverick" in r.text


def test_unknown_case_404():
    assert client.get("/case/nope").status_code == 404
    assert client.post("/case/nope/approve").status_code == 404
    assert client.post("/case/nope/accept").status_code == 404


def test_create_suggest_returns_draft():
    r = client.post("/case/create", data={
        "platform": "swiggy",
        "text": "Swiggy order #SWG123 missing item ₹229",
        "supplied_proof": "order_confirmation",
        "autonomy_level": "suggest",
    })
    assert r.status_code == 200
    c = r.json()
    assert c["status"] == "awaiting_approval"
    assert c["draft"] and "229" in c["draft"]
    assert "policy" in c


def test_full_auto_demo_resolves_over_stream():
    case_id = client.post("/demo/run?scenario=cooperative").json()["case_id"]
    evs = _stream_events(case_id)
    assert "resolved" in evs
    assert client.get(f"/case/{case_id}").json()["status"] == "resolved"


def test_suggest_offer_then_accept_endpoint():
    c = client.post("/case/create", data={
        "platform": "swiggy", "text": "Swiggy order #SWG777 missing item ₹229",
        "supplied_proof": "order_confirmation", "autonomy_level": "auto_send",
    }).json()
    _stream_events(c["id"])   # drive engagement to the paused offer
    got = client.get(f"/case/{c['id']}").json()
    assert got["status"] == "awaiting_acceptance"
    accepted = client.post(f"/case/{c['id']}/accept").json()
    assert accepted["status"] == "resolved" and accepted["refund_amount"] == 229.0


def test_malformed_amount_does_not_500():
    r = client.post("/case/create", data={
        "platform": "swiggy", "text": "refund rs. , please",
        "supplied_proof": "order_confirmation", "autonomy_level": "suggest",
    })
    assert r.status_code == 200


def test_invalid_autonomy_rejected():
    r = client.post("/case/create", data={
        "platform": "swiggy", "text": "x", "autonomy_level": "bogus",
    })
    assert r.status_code == 422


def test_proof_gap_then_resume_endpoint():
    c = client.post("/case/create", data={
        "platform": "zomato", "text": "Zomato order #ZOM1 missing item ₹150",
        "autonomy_level": "suggest",
    }).json()
    assert c["status"] == "awaiting_proof"
    resumed = client.post(f"/case/{c['id']}/proof", data={
        "supplied_proof": ",".join(c["required_proof"]),
    }).json()
    assert resumed["status"] == "awaiting_approval" and resumed["draft"]
