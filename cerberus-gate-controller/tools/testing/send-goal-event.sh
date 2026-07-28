#!/usr/bin/env bash
# Sends a dummy GOAL gate event to CERBERUS for manual testing.
# Usage: ./send-goal-event.sh [gate_id]
set -euo pipefail

HOST="${CERBERUS_HOST:-192.168.0.208}"
GATE_ID="${1:-GOAL_GATE}"

curl -i -X POST "http://${HOST}/api/event" \
  -H "Content-Type: application/json" \
  -d "{\"gate_id\":\"${GATE_ID}\",\"event\":\"GOAL\",\"tsf_us\":5000000,\"gate_us\":5000000}"
