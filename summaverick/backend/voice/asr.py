"""Speech-to-text interface (Gnani.ai ASR).

Interface stub. The phone-escalation path is on the roadmap; this defines the
shape the executor would call. A real implementation posts audio to the Gnani
ASR API and returns the transcript.
"""
from __future__ import annotations


class ASR:
    def __init__(self, api_key: str | None = None) -> None:
        self.api_key = api_key

    def transcribe(self, audio_path: str, language: str = "en-IN") -> str:
        raise NotImplementedError(
            "Voice ASR is not wired up in the demo build. Provide a Gnani.ai "
            "client here to enable phone-based escalation."
        )
