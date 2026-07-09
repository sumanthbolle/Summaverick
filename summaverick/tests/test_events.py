"""Event hub: buffered replay, fan-out, cleanup."""
import asyncio
import os
import tempfile

os.environ["SUMMAVERICK_DB"] = os.path.join(tempfile.mkdtemp(), "ev.db")

import pytest  # noqa: E402

from backend.utils import events  # noqa: E402


async def _collect(case_id, out):
    async for evt in events.subscribe(case_id):
        out.append(evt["event"])


@pytest.mark.asyncio
async def test_late_subscriber_gets_full_replay():
    # Publish and finish BEFORE anyone subscribes (the demo-race scenario).
    cid = "case-late"
    await events.publish(cid, "a")
    await events.publish(cid, "b")
    await events.finish(cid)
    got = []
    await _collect(cid, got)
    assert got == ["a", "b"]


@pytest.mark.asyncio
async def test_two_subscribers_each_get_all_events():
    cid = "case-fanout"
    a, b = [], []
    ta = asyncio.create_task(_collect(cid, a))
    tb = asyncio.create_task(_collect(cid, b))
    await asyncio.sleep(0.01)          # let both attach
    await events.publish(cid, "x")
    await events.publish(cid, "y")
    await events.finish(cid)
    await asyncio.gather(ta, tb)
    assert a == ["x", "y"] and b == ["x", "y"]   # not split between them


@pytest.mark.asyncio
async def test_cleanup_after_finish_and_detach():
    cid = "case-clean"
    await events.publish(cid, "z")
    await events.finish(cid)
    got = []
    await _collect(cid, got)
    # Once finished and the last subscriber detaches, the hub forgets the case.
    assert not events.has(cid)
