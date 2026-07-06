"""Case persistence.

Production target is Postgres + pgvector (see README). For a self-contained,
zero-dependency demo we use SQLite with a thread lock. The public API is a
small synchronous DAO; FastAPI handlers call it directly (writes are tiny).

Case entities and the interaction transcript are stored as JSON columns.
"""
from __future__ import annotations

import json
import os
import sqlite3
import threading
import time
import uuid
from enum import Enum
from typing import Any

DB_PATH = os.getenv("SUMMAVERICK_DB", os.path.join(os.path.dirname(__file__), "summaverick.db"))


class CaseStatus(str, Enum):
    created = "created"
    processing = "processing"
    awaiting_proof = "awaiting_proof"
    awaiting_approval = "awaiting_approval"
    engaging = "engaging"
    escalated = "escalated"
    resolved = "resolved"
    failed = "failed"


class AutonomyLevel(str, Enum):
    suggest = "suggest"       # draft only; user must approve to engage
    auto_send = "auto_send"   # auto-approve draft, then engage
    full_auto = "full_auto"   # also auto-accept refund offers >= threshold


_SCHEMA = """
CREATE TABLE IF NOT EXISTS cases (
    id TEXT PRIMARY KEY,
    user_id TEXT,
    platform TEXT,
    issue_type TEXT,
    status TEXT,
    autonomy_level TEXT,
    outcome TEXT,
    refund_amount REAL,
    threshold REAL,
    screenshot_url TEXT,
    entities TEXT,        -- JSON
    draft TEXT,
    transcript TEXT,      -- JSON list of {role, text, ts, intent}
    proof_gaps TEXT,      -- JSON list
    created_at REAL,
    updated_at REAL
);
"""


class Store:
    def __init__(self, path: str = DB_PATH) -> None:
        self._lock = threading.Lock()
        self._conn = sqlite3.connect(path, check_same_thread=False)
        self._conn.row_factory = sqlite3.Row
        self._conn.execute(_SCHEMA)
        self._conn.commit()

    # -- helpers ------------------------------------------------------- #
    @staticmethod
    def _row_to_dict(row: sqlite3.Row) -> dict[str, Any]:
        d = dict(row)
        for jcol in ("entities", "transcript", "proof_gaps"):
            d[jcol] = json.loads(d[jcol]) if d[jcol] else ([] if jcol != "entities" else {})
        return d

    # -- CRUD ---------------------------------------------------------- #
    def create_case(
        self,
        user_id: str,
        platform: str,
        autonomy_level: str = AutonomyLevel.suggest.value,
        screenshot_url: str | None = None,
        threshold: float | None = None,
    ) -> dict[str, Any]:
        now = time.time()
        case_id = uuid.uuid4().hex[:12]
        with self._lock:
            self._conn.execute(
                """INSERT INTO cases (id, user_id, platform, status, autonomy_level,
                       screenshot_url, entities, transcript, proof_gaps, threshold,
                       created_at, updated_at)
                   VALUES (?,?,?,?,?,?,?,?,?,?,?,?)""",
                (case_id, user_id, platform, CaseStatus.created.value, autonomy_level,
                 screenshot_url, "{}", "[]", "[]", threshold, now, now),
            )
            self._conn.commit()
        return self.get_case(case_id)

    def get_case(self, case_id: str) -> dict[str, Any] | None:
        with self._lock:
            row = self._conn.execute("SELECT * FROM cases WHERE id=?", (case_id,)).fetchone()
        return self._row_to_dict(row) if row else None

    def list_cases(self, user_id: str | None = None, limit: int = 50) -> list[dict[str, Any]]:
        with self._lock:
            if user_id:
                rows = self._conn.execute(
                    "SELECT * FROM cases WHERE user_id=? ORDER BY created_at DESC LIMIT ?",
                    (user_id, limit),
                ).fetchall()
            else:
                rows = self._conn.execute(
                    "SELECT * FROM cases ORDER BY created_at DESC LIMIT ?", (limit,)
                ).fetchall()
        return [self._row_to_dict(r) for r in rows]

    def update(self, case_id: str, **fields: Any) -> dict[str, Any] | None:
        if not fields:
            return self.get_case(case_id)
        for jcol in ("entities", "transcript", "proof_gaps"):
            if jcol in fields and not isinstance(fields[jcol], str):
                fields[jcol] = json.dumps(fields[jcol])
        fields["updated_at"] = time.time()
        cols = ", ".join(f"{k}=?" for k in fields)
        with self._lock:
            self._conn.execute(f"UPDATE cases SET {cols} WHERE id=?", (*fields.values(), case_id))
            self._conn.commit()
        return self.get_case(case_id)

    def append_transcript(self, case_id: str, role: str, text: str, intent: str | None = None) -> None:
        case = self.get_case(case_id)
        if not case:
            return
        transcript = case["transcript"]
        transcript.append({"role": role, "text": text, "intent": intent, "ts": time.time()})
        self.update(case_id, transcript=transcript)


store = Store()
