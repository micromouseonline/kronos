#!/usr/bin/env bash
# Sends a dummy ARM gate event to CERBERUS for manual testing.
# Usage: ./send-arm-event.sh [gate_id]
set -euo pipefail

HOST="${CERBERUS_HOST:-192.168.0.73}"
GATE_ID="${1:-ARM_GATE}"

curl -i -X POST "http://${HOST}/api/event" \
  -H "Content-Type: application/json" \
  -d "{\"gate_id\":\"${GATE_ID}\",\"event\":\"ARM\",\"tsf_us\":1000000,\"gate_us\":1000000}"
