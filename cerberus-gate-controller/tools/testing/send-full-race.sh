#!/usr/bin/env bash
# Runs send-one-mouse.sh repeatedly, for extended soak testing.
# Usage: ./send-full-race.sh [count] [gate_id]
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
COUNT="${1:-20}"
GATE_ID="${2:-RACE_SEQUENCE_GATE}"

for ((i = 1; i <= COUNT; i++)); do
  echo "=== race sequence run ${i}/${COUNT} ==="
  "${SCRIPT_DIR}/send-one-mouse.sh" "${GATE_ID}" || echo "  (run ${i} exited with an error, continuing)"
done
