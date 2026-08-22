"""Connector abstraction for support channels.

A connector is the boundary between the agent and a support channel. The demo
ships a MockConnector (backed by the in-process mock bot). Real-platform
connectors are intentionally NOT implemented here — see connectors/README for
the authorization, ToS, and safety requirements that gate them.
"""
from __future__ import annotations

import abc
from typing import Any


class BaseConnector(abc.ABC):
    """Abstract support-channel connector."""

    platform: str

    @abc.abstractmethod
    async def open_support_channel(self) -> str:
        """Open the channel; return the initial greeting text (may be empty)."""

    @abc.abstractmethod
    async def send_message(self, text: str) -> None:
        """Send a text message on the channel."""

    @abc.abstractmethod
    async def read_latest_response(self) -> dict[str, Any]:
        """Return the latest channel response as {"text", "intent"?, ...}."""

    async def upload_file(self, path: str) -> None:  # optional
        """Attach a file if the channel supports it. No-op by default."""
        return None

    async def close(self) -> None:  # optional
        return None
