"""Per-case async event bus for SSE streaming.

Each case gets an asyncio.Queue. Producers (orchestrator/executor) publish
events; the SSE endpoint drains them. A sentinel ends the stream.
"""
from __future__ import annotations

import asyncio
from typing import Any

_queues: dict[str, asyncio.Queue] = {}
DONE = object()


def _queue(case_id: str) -> asyncio.Queue:
    if case_id not in _queues:
        _queues[case_id] = asyncio.Queue()
    return _queues[case_id]


async def publish(case_id: str, event: str, data: dict[str, Any] | None = None) -> None:
    await _queue(case_id).put({"event": event, "data": data or {}})


async def subscribe(case_id: str):
    """Async generator yielding events until a DONE sentinel is published."""
    q = _queue(case_id)
    while True:
        item = await q.get()
        if item is DONE:
            break
        yield item


async def finish(case_id: str) -> None:
    await _queue(case_id).put(DONE)
