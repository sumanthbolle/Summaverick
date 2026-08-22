"""NIM (NVIDIA Inference Microservice) client.

Talks to an OpenAI-compatible NIM endpoint when ``NIM_API_KEY`` is configured.
When no key is present it transparently falls back to a deterministic,
offline heuristic engine so the whole application can be demoed end-to-end
with zero external dependencies.

The offline engine is intentionally simple (keyword rules). It exists so the
pipeline is runnable and testable; in production the NIM endpoints do the
real reasoning.
"""
from __future__ import annotations

import os
import re
from dataclasses import dataclass
from typing import Iterable

# Default model aliases; override via env if your NIM deployment differs.
REASONING_MODEL = os.getenv("NIM_REASONING_MODEL", "nvidia/llama-3.1-nemotron-70b-instruct")
LIGHT_MODEL = os.getenv("NIM_LIGHT_MODEL", "nvidia/nemotron-mini-4b-instruct")


@dataclass
class NimConfig:
    base_url: str = os.getenv("NIM_BASE_URL", "https://integrate.api.nvidia.com/v1")
    api_key: str | None = os.getenv("NIM_API_KEY")

    @property
    def online(self) -> bool:
        return bool(self.api_key)


class NimClient:
    """Thin wrapper over an OpenAI-compatible NIM endpoint with offline fallback."""

    def __init__(self, config: NimConfig | None = None) -> None:
        self.config = config or NimConfig()
        self._client = None
        if self.config.online:
            try:
                from openai import OpenAI  # imported lazily; optional dependency

                self._client = OpenAI(base_url=self.config.base_url, api_key=self.config.api_key)
            except Exception:  # pragma: no cover - defensive; degrade to offline
                self._client = None

    @property
    def online(self) -> bool:
        return self._client is not None

    # ------------------------------------------------------------------ #
    # Public API
    # ------------------------------------------------------------------ #
    def chat_completion(
        self,
        messages: list[dict],
        model: str = REASONING_MODEL,
        temperature: float = 0.3,
        max_tokens: int = 800,
    ) -> str:
        if self._client is not None:
            resp = self._client.chat.completions.create(
                model=model,
                messages=messages,
                temperature=temperature,
                max_tokens=max_tokens,
            )
            return (resp.choices[0].message.content or "").strip()
        return self._offline_chat(messages)

    def classify(self, text: str, labels: Iterable[str], model: str = LIGHT_MODEL) -> str:
        labels = list(labels)
        if self._client is not None:
            prompt = (
                "You are a strict text classifier. Respond with EXACTLY one label "
                f"from this list and nothing else: {labels}.\n\nText:\n{text}"
            )
            out = self.chat_completion(
                [{"role": "user", "content": prompt}], model=model, temperature=0.0, max_tokens=16
            ).lower()
            for label in labels:
                if label.lower() in out:
                    return label
            return labels[0]
        return self._offline_classify(text, labels)

    # ------------------------------------------------------------------ #
    # Offline heuristic engine
    # ------------------------------------------------------------------ #
    _ISSUE_KEYWORDS = {
        "missing_item": ["missing", "not delivered", "item not", "didn't get", "did not receive"],
        "wrong_order": ["wrong", "incorrect order", "different item"],
        "refund_rejected": ["refund rejected", "refund denied", "no refund", "declined refund"],
        "subscription_renewal": ["subscription", "renew", "auto-renew", "charged again", "netflix", "hotstar"],
        "upi_scam": ["upi", "scam", "fraud", "unauthorized", "unauthorised"],
        "splitwise_reminder": ["splitwise", "owes", "settle up", "reminder"],
    }

    _INTENT_KEYWORDS = {
        "ask_order_id": ["order id", "order number", "reference number", "share your order"],
        "ask_proof": ["photo", "screenshot", "proof", "evidence", "image of"],
        "ask_issue_description": ["describe", "what happened", "tell us more", "explain the issue"],
        "processing": ["processing", "please wait", "looking into", "checking", "working on"],
        "offer_refund": ["refund of", "refunded", "credited", "we will refund", "approved your refund"],
        "deny": ["cannot", "unable", "not eligible", "policy does not", "denied", "reject"],
        "transfer_human": ["human", "agent will", "representative", "connecting you"],
        "close": ["anything else", "closing", "resolved", "thank you for contacting", "goodbye"],
        "greeting": ["hi", "hello", "welcome", "how can i help"],
    }

    def _offline_classify(self, text: str, labels: list[str]) -> str:
        low = text.lower()
        # Issue-type classification
        if set(labels) & set(self._ISSUE_KEYWORDS):
            for label in labels:
                for kw in self._ISSUE_KEYWORDS.get(label, []):
                    if kw in low:
                        return label
        # Bot-intent classification
        if set(labels) & set(self._INTENT_KEYWORDS):
            for label in labels:
                for kw in self._INTENT_KEYWORDS.get(label, []):
                    if kw in low:
                        return label
        return labels[0]

    def _offline_chat(self, messages: list[dict]) -> str:
        """Produce a plausible complaint draft without an LLM.

        Reads a compact ``key: value`` context block that the drafting agent
        embeds in the final user message.
        """
        content = messages[-1]["content"] if messages else ""
        ctx = dict(re.findall(r"^([a-z_]+):\s*(.+)$", content, flags=re.MULTILINE))
        platform = ctx.get("platform", "the platform")
        issue = ctx.get("issue_type", "an issue").replace("_", " ")
        amount = ctx.get("amount", "")
        order_id = ctx.get("order_id", "")
        outcome = ctx.get("desired_outcome", "a full refund")
        tone = ctx.get("tone", "polite")

        opener = {
            "polite": "Hello, I'd like to raise an issue with a recent order.",
            "firm": "I am writing to formally raise an unresolved issue with my order.",
            "legal": ("This is a formal complaint. Under applicable consumer-protection "
                      "law, I am entitled to a resolution for the issue below."),
        }.get(tone, "Hello, I'd like to raise an issue with a recent order.")

        lines = [opener, ""]
        lines.append(f"Platform: {platform}")
        if order_id:
            lines.append(f"Order ID: {order_id}")
        lines.append(f"Issue: {issue}")
        if amount:
            lines.append(f"Amount involved: {amount}")
        lines.append("")
        lines.append(f"Requested resolution: {outcome}.")
        if tone == "legal":
            lines.append(
                "If this is not resolved, I will escalate to the relevant consumer "
                "grievance forum."
            )
        lines.append("")
        lines.append("Please confirm the next steps. Thank you.")
        return "\n".join(lines)


# Module-level singleton for convenience.
client = NimClient()
