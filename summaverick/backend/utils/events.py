"""Per-case event hub for SSE streaming.

Design goals (learned from earlier bugs):
  * A late subscriber still sees the whole story — every event is buffered in a
    per-case log and replayed on subscribe. This is what lets the browser open
    the stream *after* a fast offline run has already finished.
  * Multiple concurrent subscribers each get their own queue (fan-out), so two
    browser tabs / an SSE auto-reconnect don't steal each other's events.
  * Memory is bounded — finished cases with no active subscribers are evicted
    (LRU) so the hub can't grow without limit.

Everything runs on the single asyncio event loop; subscribe() sets up its queue
without awaiting, so no event can slip in between "replay log" and "go live".
"""
from __future__ import annotations

import asyncio
from collections import OrderedDict
from typing import Any

DONE = object()
_MAX_RETAINED = 200


class _Hub:
    def __init__(self) -> None:
        self._cases: "OrderedDict[str, dict]" = OrderedDict()

    def _case(self, case_id: str, create: bool = True) -> dict | None:
        c = self._cases.get(case_id)
        if c is None and create:
            c = self._cases[case_id] = {"log": [], "finished": False, "subs": set()}
            self._cases.move_to_end(case_id)
            self._evict()
        return c

    def _evict(self) -> None:
        while len(self._cases) > _MAX_RETAINED:
            for cid, c in list(self._cases.items()):
                if c["finished"] and not c["subs"]:
                    del self._cases[cid]
                    break
            else:
                break  # nothing evictable

    def has(self, case_id: str) -> bool:
        return case_id in self._cases

    async def publish(self, case_id: str, event: str, data: dict[str, Any] | None = None) -> None:
        c = self._case(case_id)
        evt = {"event": event, "data": data or {}}
        c["log"].append(evt)
        for q in c["subs"]:
            q.put_nowait(evt)

    async def finish(self, case_id: str) -> None:
        c = self._case(case_id)
        c["finished"] = True
        for q in c["subs"]:
            q.put_nowait(DONE)

    async def subscribe(self, case_id: str):
        """Yield events for a case: buffered history first, then live updates."""
        c = self._case(case_id)
        q: asyncio.Queue = asyncio.Queue()
        # Atomic setup (no await): preload history, then register for live events.
        for evt in c["log"]:
            q.put_nowait(evt)
        if c["finished"]:
            q.put_nowait(DONE)
        c["subs"].add(q)
        try:
            while True:
                item = await q.get()
                if item is DONE:
                    break
                yield item
        finally:
            c["subs"].discard(q)
            if c["finished"] and not c["subs"]:
                self._cases.pop(case_id, None)


_hub = _Hub()

# Module-level API (kept stable for callers).
publish = _hub.publish
finish = _hub.finish
subscribe = _hub.subscribe
has = _hub.has
