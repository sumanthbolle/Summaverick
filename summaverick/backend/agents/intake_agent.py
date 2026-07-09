"""Intake agent: turn a screenshot/voice-note/text into a structured issue.

OCR is optional (pytesseract). When it isn't installed we work from the text
the user supplied directly, so the pipeline still runs. Classification and
entity extraction go through the light NIM model (with offline fallback).
"""
from __future__ import annotations

import re
from typing import Any

from ..guardrails.filter import redact
from ..llm.nim_client import LIGHT_MODEL, client

ISSUE_LABELS = [
    "missing_item",
    "wrong_order",
    "refund_rejected",
    "subscription_renewal",
    "upi_scam",
    "splitwise_reminder",
    "not_delivered",
]


def ocr_image(path: str) -> str:
    """Extract text from an image; returns "" if OCR is unavailable."""
    try:
        import pytesseract  # optional
        from PIL import Image

        return pytesseract.image_to_string(Image.open(path))
    except Exception:
        return ""


def _extract_entities(text: str) -> dict[str, Any]:
    entities: dict[str, Any] = {}
    m = re.search(r"(?:order|ref(?:erence)?|booking)\s*(?:id|no\.?|number|#)?\s*[:#]?\s*([A-Z0-9]{5,})", text, re.I)
    if m:
        entities["order_id"] = m.group(1)
    # Require at least one digit so a run of commas/spaces can't reach float().
    m = re.search(r"(?:₹|rs\.?|inr)\s*([\d,]*\d[\d,]*(?:\.\d{1,2})?)", text, re.I)
    if m:
        try:
            entities["amount"] = float(m.group(1).replace(",", ""))
        except ValueError:
            pass
    m = re.search(r"\b(\d{1,2}[/-]\d{1,2}[/-]\d{2,4}|\d{4}-\d{2}-\d{2})\b", text)
    if m:
        entities["date"] = m.group(1)
    return entities


def run(*, text: str | None = None, screenshot_path: str | None = None,
        platform: str | None = None) -> dict[str, Any]:
    """Return {issue_type, entities, extracted_text}."""
    raw = text or ""
    if screenshot_path:
        raw = (raw + "\n" + ocr_image(screenshot_path)).strip()

    issue_type = client.classify(raw, ISSUE_LABELS, model=LIGHT_MODEL) if raw else ISSUE_LABELS[0]
    entities = _extract_entities(raw)
    if platform:
        entities["platform"] = platform

    return {
        "issue_type": issue_type,
        "entities": entities,
        "extracted_text": redact(raw),
    }
