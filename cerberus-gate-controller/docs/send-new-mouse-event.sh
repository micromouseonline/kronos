#!/usr/bin/env bash
# Sends a dummy NEW_MOUSE gate event to CERBERUS for manual testing.
# Usage: ./send-new-mouse-event.sh [gate_id]
set -euo pipefail

HOST="${CERBERUS_HOST:-192.168.0.208}"
GATE_ID="${1:-NEW_MOUSE_GATE}"

curl -i -X POST "http://${HOST}/api/event" \
  -H "Content-Type: application/json" \
  -d "{\"gate_id\":\"${GATE_ID}\",\"event\":\"NEW_MOUSE\",\"tsf_us\":6000000,\"gate_us\":6000000}"
