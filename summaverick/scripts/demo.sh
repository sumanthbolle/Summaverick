#!/usr/bin/env bash
# End-to-end demo: boots the backend, fires a full-auto Swiggy case, and prints
# the live SSE events to the console. No external services required.
set -euo pipefail
cd "$(dirname "$0")/.."

PORT="${PORT:-8099}"
echo "▶ starting Summaverick backend on :$PORT (offline mode unless NIM_API_KEY is set)"
python3 -m uvicorn backend.main:app --port "$PORT" --log-level warning &
SERVER_PID=$!
trap 'kill $SERVER_PID 2>/dev/null || true' EXIT

# wait for health
for _ in $(seq 1 20); do
  if curl -sf "localhost:$PORT/health" >/dev/null; then break; fi
  sleep 0.3
done

SCENARIO="${1:-cooperative}"
echo "▶ creating full-auto demo case (scenario: $SCENARIO)"
CID=$(curl -s -X POST "localhost:$PORT/demo/run?scenario=$SCENARIO" \
       | python3 -c "import sys,json; print(json.load(sys.stdin)['case_id'])")
echo "▶ case id: $CID — streaming live events:"
echo "--------------------------------------------------------"
# --max-time caps the stream (portable; macOS has no GNU `timeout`). The SSE
# stream also ends itself with a `done` event once the case resolves.
curl --max-time 20 -s -N "localhost:$PORT/case/$CID/stream" || true
echo "--------------------------------------------------------"
echo "▶ done."
