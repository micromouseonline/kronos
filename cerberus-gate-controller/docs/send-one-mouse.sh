#!/usr/bin/env bash
# Drives CERBERUS through a NEW_MOUSE reset followed by 4 ARM/START/GOAL race
# cycles, for manual end-to-end testing of the race state machine.
# Usage: ./send-one-mouse.sh [gate_id]
set -euo pipefail

HOST="${CERBERUS_HOST:-192.168.0.73}"
GATE_ID="${1:-RACE_SEQUENCE_GATE}"

FAILED_EVENTS=0
TOTAL_EVENTS=0

send_event() {
  local event="$1"
  local now_us
  now_us=$(date +%s%6N)
  local status
  TOTAL_EVENTS=$((TOTAL_EVENTS + 1))
  # -S alongside -s so a transport failure (dropped Wi-Fi, mDNS hiccup,
  # timeout) still prints curl's own error; "|| status=ERR" stops that
  # failure from aborting the whole sequence under set -e -- a soak test
  # should log a dropped request and keep going, not die silently.
  status=$(curl -s -S --max-time 5 -o /dev/null -w "%{http_code}" -X POST "http://${HOST}/api/event" \
    -H "Content-Type: application/json" \
    -d "{\"gate_id\":\"${GATE_ID}\",\"event\":\"${event}\",\"tsf_us\":${now_us},\"gate_us\":${now_us}}") || status="ERR"
  if [[ "$status" != "200" ]]; then
    FAILED_EVENTS=$((FAILED_EVENTS + 1))
  fi
  printf '%s  %-10s HTTP %s\n' "$(date +%H:%M:%S)" "$event" "$status"
}

random_delay_seconds() {
  # Random integer from 0 to 70
  local offset=$(( RANDOM % 71 ))
  # 30 + offset gives range 30 to 100 tenths (3.0 to 10.0s)
  local tenths=$(( 30 + offset ))
  
  # Format as float (e.g., 34 -> 3.4, 100 -> 10.0)
  printf '%d.%d\n' $(( tenths / 10 )) $(( tenths % 10 ))
}

send_event NEW_MOUSE
sleep 1

send_event ARM
sleep 0.2
send_event START
sleep 10
send_event GOAL

for i in 1 2 3; do
  send_event ARM
  sleep 0.2
  send_event START
  sleep "$(random_delay_seconds)"
  send_event GOAL
done

printf '%d/%d events failed\n' "$FAILED_EVENTS" "$TOTAL_EVENTS"
