# Summaverick — Autonomous Customer-Advocacy Agent

Summaverick takes a description (or screenshot) of a consumer problem — a
missing food-delivery item, a wrongly-rejected refund — reasons about the
platform's policy, drafts a complaint, and then **autonomously negotiates a
support conversation to resolution**, streaming every step live.

This repository is a **runnable MVP of the safe demo path**: the full agentic
pipeline plus an end-to-end negotiation against a built-in **mock support bot**.
It runs with **zero external services** — no API keys, no Postgres, no browser.

```
intake → policy check → proof-gap → drafting → engage → negotiate → resolved
```

## Run in 30 seconds

```bash
git clone https://github.com/sumanthbolle/Summaverick.git
cd Summaverick/summaverick        # the project lives in the summaverick/ subfolder
pip install -r requirements.txt
bash scripts/demo.sh              # watch a case resolve itself in the terminal
```

That's it — no API keys, no database, no browser needed.

**Python:** 3.11+ recommended. On macOS's built-in Python 3.9 use `pip3`/`python3`;
`requirements.txt` pulls in `eval_type_backport` so it still runs on 3.9.

## Quick start

```bash
cd summaverick
pip install -r requirements.txt

# Run the whole thing offline and watch a case resolve itself:
bash scripts/demo.sh            # cooperative bot
bash scripts/demo.sh stubborn   # bot denies first → agent escalates tone

# Or run the API + live UI, then open http://localhost:8099/ and press “Start Demo”:
python -m uvicorn backend.main:app --port 8099
#   the UI is served same-origin at / (no CORS/file:// issues)

# Tests:
pytest -q
```

`GET /health` reports `"nim_online": false` in offline mode — that's expected
and everything still works.

## How it works

| Component | File | Role |
|-----------|------|------|
| Orchestrator | `backend/agents/orchestrator.py` | Async state machine wiring the agents together |
| Intake | `backend/agents/intake_agent.py` | OCR (optional) + issue classification + entity extraction |
| Policy | `backend/agents/policy_agent.py` | Loads `platforms/*.yaml`, judges refund eligibility |
| Proof-gap | `backend/agents/proof_gap_agent.py` | Lists evidence still needed from the user |
| Drafting | `backend/agents/drafting_agent.py` | Composes the complaint (polite → firm → legal) |
| Executor | `backend/agents/executor_agent.py` | `NegotiationLoop`: send → classify bot intent → act → repeat |
| Tracker | `backend/agents/tracker_agent.py` | Records outcomes, schedules follow-ups |
| LLM client | `backend/llm/nim_client.py` | NVIDIA NIM (OpenAI-compatible) **with offline heuristic fallback** |
| Mock bot | `backend/mockbot/engine.py` | Deterministic support-bot simulator for demos/tests |
| Connectors | `backend/connectors/` | Channel abstraction + `MockConnector` |
| Guardrails | `backend/guardrails/filter.py` | PII redaction + outbound-message safety check |
| Memory | `backend/memory/db.py` | SQLite case store (Postgres+pgvector is the prod target) |
| Frontend | `frontend/index.html` | Live chat + timeline via Server-Sent Events |

### Autonomy levels
- `suggest` — produce a draft and stop; user calls `POST /case/{id}/approve` to engage.
- `auto_send` — auto-approve the draft, then engage.
- `full_auto` — also auto-accept refund offers at/above the user's threshold.

### Online (NIM) mode
Set `NIM_API_KEY` (see `.env.example`) and the same code paths call the
Nemotron models for classification, policy reasoning, and drafting. Nothing
else changes. Without a key, a deterministic heuristic engine stands in so the
project is always runnable and testable.

## Key endpoints
- `POST /case/create` — multipart: `platform`, `text`, optional `screenshot`, `autonomy_level`, …
- `GET /case/{id}` / `GET /cases`
- `POST /case/{id}/approve` — approve draft, start live negotiation
- `GET /case/{id}/stream` — SSE: `agent_sent`, `bot_replied`, `escalating`, `resolved`, …
- `POST /demo/run?scenario=cooperative|stubborn` — one-click full-auto demo case
- `POST /simulate/bot/start` · `POST /simulate/bot/reply` — poke the mock bot directly

## Scope & safety — what this build intentionally does NOT include

The original blueprint listed live connectors that log into and operate **real**
Zomato / Swiggy / Amazon / subscription accounts, plus anti-bot-evasion tooling
(browser-fingerprint rotation, residential-proxy rotation, credential scraping).

Those evasion components are **deliberately not built here**. They exist to
circumvent platforms' access controls and bot detection, which this project
does not do. The legitimate consumer-advocacy core is demonstrated against the
built-in mock bot instead.

Real-platform connectors can be added later, but only on the terms spelled out
in [`backend/connectors/README.md`](backend/connectors/README.md): authorised,
authenticated, user-consented sessions that respect each platform's Terms of
Service and prefer official APIs over scraping — never hiding from the platform.

PII (phone, email, UPI id, card-like numbers) is redacted from logs and blocked
from outbound messages unless explicitly approved (`backend/guardrails/filter.py`).

## Roadmap (from the blueprint, not yet built)
- Postgres + pgvector memory with similar-case retrieval (SQLite today).
- Celery/Redis durable follow-up scheduling (in-process stub today).
- Voice escalation (Gnani ASR / Riva TTS) — interface stubs only.
- Full Next.js dashboard (the single-file `frontend/index.html` covers the demo).
- Proactive account scanning — concept only, gated on the connector terms above.

## Project layout
```
summaverick/
├── backend/        FastAPI app, agents, connectors, mock bot, guardrails, memory
├── platforms/      per-platform policy YAML (zomato, swiggy)
├── frontend/       single-file live case UI (SSE)
├── scripts/        demo.sh
├── tests/          pipeline + negotiation tests
├── docker-compose.yml   optional Postgres+Redis for the prod memory path
└── requirements.txt
```
