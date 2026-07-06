"""MockConnector: drives the in-process mock support bot.

This is the safe, default connector used for demos and tests. It never touches
a real service. It gives the executor a realistic multi-turn channel to
negotiate against.
"""
from __future__ import annotations

from typing import Any

from ..mockbot.engine import bot
from .base import BaseConnector


class MockConnector(BaseConnector):
    def __init__(self, platform: str, session_id: str, scenario: str = "cooperative",
                 refund_amount: float = 229.0) -> None:
        self.platform = platform
        self.session_id = session_id
        self.scenario = scenario
        self.refund_amount = refund_amount
        self._last_user_message: str = ""

    async def open_support_channel(self) -> str:
        return bot.start(self.session_id, self.platform, self.scenario, self.refund_amount)

    async def send_message(self, text: str) -> None:
        self._last_user_message = text

    async def read_latest_response(self) -> dict[str, Any]:
        return bot.reply(self.session_id, self._last_user_message)

    async def upload_file(self, path: str) -> None:
        # The mock channel just acknowledges attachments.
        return None
