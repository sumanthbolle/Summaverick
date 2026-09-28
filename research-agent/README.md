# Summaverick Research Agent — ServiceNow Domain Pack

Production-ready **ServiceNow domain intelligence** for the Summaverick research agent.

This package extends Summaverick without replacing:

- LLM gateway (Perplexity Sonar via `api/worker.js`)
- Chat request/response contract (`POST /` → `{ success, result }`)
- Existing citation / source rendering
- Authentication / CORS / tenancy model of the Worker

## Architecture (three knowledge layers)

| Layer | Source | Role |
| --- | --- | --- |
| 1 | `@servicenow/sdk` `explain` (version-matched) | Fluent APIs, `.now.ts`, `now.config.json`, build/auth topics |
| 2 | [ServiceNowDocs](https://github.com/ServiceNow/ServiceNowDocs) `llms.txt` | ITSM, CMDB, ITOM, IRM/GRC, SPM, CSM, HRSD, platform |
| 3 | SDK `query` (optional, off by default) | Read-only allowlisted instance metadata — task-scoped only |

Live instance results are **never** merged into the permanent global knowledge base.

## Layout

```text
research-agent/
  src/
    core/                         # shared types, command runner, tracing
    domains/servicenow/
      config.ts                   # ServiceNowDomainConfig + env loader
      router.ts                   # domain detection + research plan
      policy.ts                   # read-only + deletion safety
      workflow.ts                 # end-to-end research workflow
      providers/                  # SDK explain, SDK query, docs, repository
      retrieval/                  # classifier, expander, ranker, release filter
      security/                   # allowlist, redaction, injection, command policy
      tools/                      # structured tool wrappers (no generic shell)
      schemas/                    # evidence + answer contracts
      prompts/                    # system / research / verifier prompts
      evals/                      # 50+ regression + adversarial cases
      tests/                      # unit + mocked integration tests
api/servicenow-domain.js          # Worker-compatible classifier + prompts
api/worker.js                     # routes ServiceNow chat through domain addon
```

## Quick start

```bash
cd research-agent
npm install
npm test
npm run typecheck
```

Optional SDK dependency (for real Fluent projects):

```json
{
  "devDependencies": {
    "@servicenow/sdk": "^4.9.0"
  }
}
```

Capability gates:

- `explain` requires SDK **4.6.0+**
- `query` requires SDK **4.8.0+**

If an older SDK is pinned, the pack reports the limitation and skips those providers.

## Configuration

Copy `.env.example`. Defaults:

- Domain enabled
- Docs enabled (`australia` release family)
- SDK enabled
- **Instance query disabled**
- Citations required
- `permitWriteOperations: false`
- `persistLiveInstanceResults: false`
- Sensitive-field redaction on

Worker env (optional):

- `SERVICENOW_DOMAIN_ENABLED` (default true)
- `SERVICENOW_RELEASE_FAMILY` (default `australia`)

## Research workflow

```text
question → domain detect → intent classify → SDK version / release family
        → source plan → retrieve (SDK / docs / repo / optional instance)
        → dedupe + rank → draft → verify claims → cited answer
```

## Security controls

- Initial table allowlist: metadata tables only (`sys_dictionary`, `sys_db_object`, …)
- Business tables (`incident`, `sys_user`, …) require explicit opt-in
- Blocked: credentials, OAuth secrets, encryption contexts, sensitive HR, etc.
- Row limit + timeout enforced
- Mutating SDK commands (`build`, `deploy`, `delete`, …) require explicit approval and are blocked in v1
- Fluent definition deletion always requires user approval after impact analysis
- Retrieved content wrapped in `BEGIN_UNTRUSTED_SERVICENOW_EVIDENCE` … `END_…`

## Cursor SDK plugin

For local Fluent work in Cursor IDE (optional, separate from this repo package):

1. Cursor Settings → Plugins
2. Add `https://github.com/ServiceNow/sdk.git`
3. Enable `servicenow-sdk` and reload

## Tests

```bash
npm test                 # unit + mocked integration (no live instance)
npm run test:integration # only when SERVICENOW_LIVE_INTEGRATION=1
```

## Worker integration

`api/worker.js` detects ServiceNow questions and injects the domain system addon.
General (non-ServiceNow) chat behaviour and the JSON contract remain unchanged.
Optional response metadata when routed:

```json
{
  "domain": "servicenow",
  "servicenow": {
    "intent": "fluent_sdk",
    "modules": ["fluent_sdk"],
    "releaseFamily": "australia",
    "liveInstanceEnabled": false
  }
}
```

Full SDK CLI orientation, instance query, and repository inspection run in Node via this package (Cursor agents / CI), not inside Cloudflare Workers.
