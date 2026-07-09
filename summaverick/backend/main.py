"""Summaverick FastAPI application.

Endpoints:
  GET  /health
  POST /case/create               intake -> proof_gap -> draft
  GET  /case/{id}                  full case (404 if unknown)
  GET  /cases                      list cases
  POST /case/{id}/proof            add evidence, re-check gaps, draft
  POST /case/{id}/approve          approve draft and start engaging (async)
  POST /case/{id}/engage           alias for approve
  POST /case/{id}/accept           accept a paused refund offer
  GET  /case/{id}/stream           SSE live event stream
  POST /simulate/bot/start         start a mock-bot session (external demos)
  POST /simulate/bot/reply         mock-bot reply (external demos)
  POST /demo/run                   one-click full-auto demo case
"""
from __future__ import annotations

import asyncio
import json
import os
import tempfile
from typing import Any

from fastapi import Body, FastAPI, File, Form, HTTPException, UploadFile
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import FileResponse, StreamingResponse

FRONTEND_DIR = os.path.join(os.path.dirname(__file__), "..", "frontend")

from .agents import orchestrator
from .memory.db import AutonomyLevel, CaseStatus, store
from .mockbot.engine import bot
from .utils import events

app = FastAPI(title="Summaverick", version="0.1.0",
              description="Autonomous customer-advocacy agent (demo build).")
app.add_middleware(
    CORSMiddleware, allow_origins=["*"], allow_methods=["*"], allow_headers=["*"],
)


@app.get("/health")
async def health() -> dict[str, Any]:
    from .llm.nim_client import client as nim
    return {"status": "ok", "nim_online": nim.online}


@app.get("/")
async def index() -> FileResponse:
    """Serve the demo UI same-origin (avoids file:// + CORS issues)."""
    return FileResponse(os.path.join(FRONTEND_DIR, "index.html"))


# --------------------------------------------------------------------- #
# Cases
# --------------------------------------------------------------------- #
@app.post("/case/create")
async def create_case(
    platform: str = Form(...),
    user_id: str = Form("demo-user"),
    text: str | None = Form(None),
    autonomy_level: str = Form(AutonomyLevel.suggest.value),
    desired_outcome: str = Form("a full refund"),
    threshold: float | None = Form(None),
    scenario: str | None = Form(None),
    supplied_proof: str | None = Form(None),  # comma-separated
    screenshot: UploadFile | None = File(None),
) -> dict[str, Any]:
    if autonomy_level not in {a.value for a in AutonomyLevel}:
        raise HTTPException(422, f"invalid autonomy_level: {autonomy_level}")

    screenshot_path = None
    if screenshot is not None:
        suffix = os.path.splitext(screenshot.filename or "")[1] or ".png"
        fd, screenshot_path = tempfile.mkstemp(suffix=suffix)
        with os.fdopen(fd, "wb") as fh:
            fh.write(await screenshot.read())

    proof = [p.strip() for p in supplied_proof.split(",")] if supplied_proof else []
    try:
        case = await orchestrator.process_intake(
            user_id=user_id,
            platform=platform.lower(),
            text=text,
            screenshot_path=screenshot_path,
            supplied_proof=proof,
            autonomy_level=autonomy_level,
            desired_outcome=desired_outcome,
            threshold=threshold,
            scenario=scenario,
        )
    finally:
        if screenshot_path and os.path.exists(screenshot_path):
            os.unlink(screenshot_path)  # don't leak the uploaded file on disk

    # Auto modes proceed to engagement in the background.
    if case["status"] != CaseStatus.awaiting_proof.value and \
            autonomy_level != AutonomyLevel.suggest.value:
        asyncio.create_task(orchestrator.engage(case["id"]))
    return case


@app.get("/case/{case_id}")
async def get_case(case_id: str) -> dict[str, Any]:
    case = store.get_case(case_id)
    if not case:
        raise HTTPException(404, "case not found")
    return case


@app.get("/cases")
async def list_cases(user_id: str | None = None) -> list[dict[str, Any]]:
    return store.list_cases(user_id)


@app.post("/case/{case_id}/proof")
async def add_proof(case_id: str, supplied_proof: str = Form("")) -> dict[str, Any]:
    """User confirms/attaches evidence; re-checks gaps and drafts if satisfied."""
    if not store.get_case(case_id):
        raise HTTPException(404, "case not found")
    proof = [p.strip() for p in supplied_proof.split(",") if p.strip()]
    case = await orchestrator.add_proof(case_id, proof)
    if case["status"] != CaseStatus.awaiting_proof.value and \
            case.get("autonomy_level") != AutonomyLevel.suggest.value:
        asyncio.create_task(orchestrator.engage(case_id))
    return case


@app.post("/case/{case_id}/approve")
async def approve_case(case_id: str) -> dict[str, Any]:
    case = store.get_case(case_id)
    if not case:
        raise HTTPException(404, "case not found")
    if not case.get("draft"):
        raise HTTPException(409, "case has no draft to approve")
    if case["status"] != CaseStatus.awaiting_approval.value:
        raise HTTPException(409, f"case is {case['status']}, cannot approve")
    asyncio.create_task(orchestrator.engage(case_id))
    return {"case_id": case_id, "status": CaseStatus.engaging.value}


@app.post("/case/{case_id}/accept")
async def accept_case(case_id: str) -> dict[str, Any]:
    """Accept a paused refund offer (suggest / auto_send / below-threshold)."""
    if not store.get_case(case_id):
        raise HTTPException(404, "case not found")
    return await orchestrator.accept_offer(case_id)


# alias
app.add_api_route("/case/{case_id}/engage", approve_case, methods=["POST"])


@app.get("/case/{case_id}/stream")
async def stream_case(case_id: str) -> StreamingResponse:
    async def gen():
        case = store.get_case(case_id)
        if not case:
            yield _sse("error", {"detail": "not found"})
            yield _sse("done", {})
            return
        if events.has(case_id):
            # Live or recently finished — stream buffered history + live events.
            async for evt in events.subscribe(case_id):
                yield _sse(evt["event"], evt["data"])
        else:
            # Not tracked by the hub (never engaged / long finished): static replay.
            yield _sse("snapshot", {
                "status": case["status"], "transcript": case["transcript"],
                "refund_amount": case.get("refund_amount"),
            })
        yield _sse("done", {})

    return StreamingResponse(
        gen(),
        media_type="text/event-stream",
        headers={"Cache-Control": "no-cache", "X-Accel-Buffering": "no"},
    )


def _sse(event: str, data: dict[str, Any]) -> str:
    return f"event: {event}\ndata: {json.dumps(data)}\n\n"


# --------------------------------------------------------------------- #
# Mock bot (external demos / manual poking)
# --------------------------------------------------------------------- #
@app.post("/simulate/bot/start")
async def bot_start(platform: str = "zomato", scenario: str = "cooperative",
                    session_id: str = "manual", refund_amount: float = 229.0) -> dict[str, Any]:
    text = bot.start(session_id, platform, scenario, refund_amount)
    return {"session_id": session_id, "text": text}


@app.post("/simulate/bot/reply")
async def bot_reply(session_id: str = Body(...), message: str = Body(...)) -> dict[str, Any]:
    return bot.reply(session_id, message)


# --------------------------------------------------------------------- #
# One-click demo
# --------------------------------------------------------------------- #
@app.post("/demo/run")
async def demo_run(scenario: str = "cooperative") -> dict[str, Any]:
    """Create a pre-baked Swiggy missing-item case and run it full-auto."""
    sample = (
        "Swiggy order #SWG9F2K31 placed on missing item — the paneer roll "
        "was not delivered. Amount ₹229 paid via UPI."
    )
    case = await orchestrator.process_intake(
        user_id="demo-user",
        platform="swiggy",
        text=sample,
        supplied_proof=["order_confirmation"],
        autonomy_level=AutonomyLevel.full_auto.value,
        desired_outcome="a full refund of ₹229",
        threshold=0.0,
        scenario=scenario,
    )
    asyncio.create_task(orchestrator.engage(case["id"]))
    return {"case_id": case["id"], "stream": f"/case/{case['id']}/stream"}
