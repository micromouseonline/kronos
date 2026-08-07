#!/usr/bin/env python3
"""Manual test client for CERBERUS's persistent WebSocket endpoint (/ws).

Sends the same gate_id/event/tsf_us/gate_us JSON schema the send-*.sh HTTP
scripts in this directory use against /api/event, but over one held-open
WebSocket connection instead of a fresh HTTP connection per event -- this is
the actual behaviour NETWORK-TIMING-LOG.md's recommendation 1 introduces,
so unlike the HTTP scripts this tool deliberately reuses a single connection
across every event in a run rather than reconnecting each time.

No response is expected back from cerberus (see net/http-server.h's
ws_event_handler() comment -- fire-and-forget by design, a retry/reliability
scheme is a separate, not-yet-designed item). "sent"/"ERROR" below reflects
whether the local send() call succeeded, not whether cerberus processed it --
check cerberus's own debug log (a `[WS] DATA ... body=...` line) to confirm
that.

Requires the `websocket-client` package: pip install websocket-client

Usage:
  ./ws_send_event.py ARM
  ./ws_send_event.py START MY_GATE_LABEL
  ./ws_send_event.py sequence
  ./ws_send_event.py sequence --repeats 3 --gate-id MY_GATE_LABEL
  CERBERUS_HOST=192.168.1.50 ./ws_send_event.py GOAL
  ./ws_send_event.py GOAL --tsf-us 123456  # craft a specific/stale tsf_us
"""
import argparse
import json
import os
import random
import sys
import time

import websocket

DEFAULT_HOST = os.environ.get("CERBERUS_HOST", "cerberus.local")
SINGLE_EVENTS = ("ARM", "START", "GOAL", "RESTART", "NEW_MOUSE")


def now_us() -> int:
    return time.time_ns() // 1000


def send_event(ws: websocket.WebSocket, event: str, gate_id: str, tsf_us: int = None) -> None:
    ts = now_us() if tsf_us is None else tsf_us
    payload = json.dumps({"gate_id": gate_id, "event": event, "tsf_us": ts, "gate_us": ts})
    try:
        ws.send(payload)
        status = "sent"
    except (websocket.WebSocketException, OSError) as exc:
        status = f"ERROR ({exc})"
    print(f"{time.strftime('%H:%M:%S')}  {event:<10} {status}")


def run_sequence(ws: websocket.WebSocket, gate_id: str, repeats: int) -> None:
    """Mirrors send-one-mouse.sh's timing: NEW_MOUSE, then one ARM/START/GOAL
    cycle with a fixed 10s run, then `repeats` more with a random 3-10s run."""
    send_event(ws, "NEW_MOUSE", gate_id)
    time.sleep(1)

    send_event(ws, "ARM", gate_id)
    time.sleep(0.2)
    send_event(ws, "START", gate_id)
    time.sleep(10)
    send_event(ws, "GOAL", gate_id)

    for _ in range(repeats):
        send_event(ws, "ARM", gate_id)
        time.sleep(0.2)
        send_event(ws, "START", gate_id)
        time.sleep(round(random.uniform(3.0, 10.0), 1))
        send_event(ws, "GOAL", gate_id)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("event", choices=SINGLE_EVENTS + ("sequence",), help="event to send, or 'sequence' for a full race")
    parser.add_argument("gate_id", nargs="?", default="WS_TEST_GATE", help="gate_id label (default: WS_TEST_GATE)")
    parser.add_argument("--host", default=DEFAULT_HOST, help=f"cerberus host (default: {DEFAULT_HOST}, or $CERBERUS_HOST)")
    parser.add_argument("--repeats", type=int, default=3, help="sequence mode: extra ARM/START/GOAL cycles after the first (default: 3)")
    parser.add_argument(
        "--tsf-us",
        type=int,
        default=None,
        help="override tsf_us instead of stamping 'now' -- e.g. to manually craft a stale GOAL "
        "(a tsf_us before an already-sent START) for NETWORK-TIMING-LOG.md #7's misattribution "
        "test, without needing a real delayed message. Single-event mode only.",
    )
    args = parser.parse_args()

    url = f"ws://{args.host}/ws"
    try:
        ws = websocket.create_connection(url, timeout=5)
    except (websocket.WebSocketException, OSError) as exc:
        print(f"Failed to connect to {url}: {exc}", file=sys.stderr)
        return 1

    print(f"Connected to {url}")
    try:
        if args.event == "sequence":
            run_sequence(ws, args.gate_id, args.repeats)
        else:
            send_event(ws, args.event, args.gate_id, args.tsf_us)
    finally:
        ws.close()

    return 0


if __name__ == "__main__":
    sys.exit(main())
