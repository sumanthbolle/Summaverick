"""Text-to-speech interface (NVIDIA Riva TTS).

Interface stub for the phone-escalation path. A real implementation streams
synthesized audio from a Riva endpoint.
"""
from __future__ import annotations


class TTS:
    def __init__(self, endpoint: str | None = None) -> None:
        self.endpoint = endpoint

    def synthesize(self, text: str, voice: str = "English-US.Female-1") -> bytes:
        raise NotImplementedError(
            "Voice TTS is not wired up in the demo build. Provide a Riva TTS "
            "client here to enable phone-based escalation."
        )
