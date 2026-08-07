# Gate Event Test Scripts

Manual curl test scripts for the `POST /api/event` endpoint (see
`docs/SYSTEM-DESCRIPTION.md`'s HTTP Server section,
implemented in `src/net/http-server.h` / `src/race/race-command-source.h`).
See the bottom of this file for `ws_send_event.py`, the equivalent tool for
the persistent WebSocket endpoint (`/ws`, NETWORK-TIMING-LOG.md
recommendation 1).

## Endpoint

```
POST http://cerberus.local/api/event
Content-Type: application/json

{
  "gate_id": "START_GATE",
  "event": "START",
  "tsf_us": 4321098765,
  "gate_us": 1098765634
}
```

| Field    | Type   | Meaning |
|----------|--------|---------|
| `gate_id`| string | Free-text label for the sending gate. Not validated against a fixed list -- carried through unchanged into the event queue/payload for logging/display. |
| `event`  | string | One of `ARM`, `START`, `GOAL`, `NEW_MOUSE` (also `RESTART`, accepted but not covered by these scripts). Anything else is rejected. `NEW_MOUSE` is handled identically to `RESTART` internally -- forces a new-mouse selection from any race state, same as an ARM-button long-press or the serial `<98,0>` message. |
| `tsf_us` | uint64 | Gate's local reading of the shared Wi-Fi TSF clock at the moment of the event. |
| `gate_us`| uint64 | Gate's own free-running microsecond timer, used for drift-compensation cross-referencing against `tsf_us`. |

Responses:
- `200 {"status":"ok"}` -- event accepted and queued.
- `400 {"status":"error","reason":"unrecognised event"}` -- `event` didn't match a known command.

## Scripts

- `send-arm-event.sh`
- `send-start-event.sh`
- `send-goal-event.sh`
- `send-new-mouse-event.sh`
- `send-one-mouse.sh` -- scripted multi-event sequence, see below.
- `send-full-race.sh` -- runs `send-one-mouse.sh` repeatedly, see below.

The four single-event scripts each send one event with fixed dummy
`tsf_us`/`gate_us` values (not real TSF/timer readings -- fine for exercising
the HTTP handler and race state machine, not for timing accuracy testing).

### Usage

```
./send-arm-event.sh              # gate_id defaults to ARM_GATE
./send-start-event.sh            # gate_id defaults to START_GATE
./send-goal-event.sh             # gate_id defaults to GOAL_GATE
./send-new-mouse-event.sh        # gate_id defaults to NEW_MOUSE_GATE

./send-start-event.sh MY_GATE_LABEL   # override gate_id

CERBERUS_HOST=192.168.1.50 ./send-arm-event.sh   # override host (default cerberus.local)
```

A typical run sequence to exercise the race state machine end to end:

```
./send-arm-event.sh
./send-start-event.sh
./send-goal-event.sh
```

Then check `http://cerberus.local/leaderboard` to confirm the run was recorded.

## send-one-mouse.sh

Drives a full timed sequence against a live unit, unattended:

```
NEW_MOUSE          (delay 1s)
ARM                (delay 200ms)
START              (delay 4s)
GOAL
-- repeated 3 more times --
ARM                (delay 200ms)
START              (delay random 1-5s)
GOAL
```

Unlike the other scripts, `tsf_us`/`gate_us` are set to the actual current
time (`date +%s%6N`, microseconds since epoch) at the moment each request is
sent, since the delays here are real -- this keeps the dummy timestamps
consistent with wall-clock elapsed time between events.

```
./send-one-mouse.sh                    # gate_id defaults to RACE_SEQUENCE_GATE
./send-one-mouse.sh MY_GATE_LABEL       # override gate_id
CERBERUS_HOST=192.168.1.50 ./send-one-mouse.sh
```

Prints one line per event with a timestamp and the HTTP status returned.
Watch `http://cerberus.local/leaderboard` while it runs to see runs land in
real time.

A dropped/failed request (bad host, timeout, transient network blip) is
logged as `HTTP ERR` rather than aborting the sequence -- the script always
runs all 13 events and prints a `N/13 events failed` summary at the end, so
soak runs survive transient blips instead of dying silently partway through.

## send-full-race.sh

Runs `send-one-mouse.sh` back to back, for soak/stability testing over
many runs. Defaults to 20 repeats (80 total ARM/START/GOAL cycles).

```
./send-full-race.sh                        # 20 repeats, default gate_id
./send-full-race.sh 50                      # 50 repeats
./send-full-race.sh 50 MY_GATE_LABEL        # 50 repeats, custom gate_id
CERBERUS_HOST=192.168.1.50 ./send-full-race.sh
```

At ~20-40s per sequence, 20 repeats takes roughly 7-13 minutes. A failed
sequence (non-zero exit) is logged and the loop continues to the next repeat
rather than stopping the batch.

## ws_send_event.py

Same event schema as the scripts above, but sent over `/ws` (a persistent
WebSocket connection) instead of `POST /api/event` (a fresh HTTP connection
per event) -- the transport recommendation 1 in
`NETWORK-TIMING-LOG.md` introduces. One connection is opened and reused for
every event in a run, matching how a real gate board behaves post-recommendation-1
(unlike the HTTP scripts, which necessarily open a new connection each time).

No response is sent back from cerberus over the socket (fire-and-forget by
design -- see `net/http-server.h`'s `ws_event_handler()` comment), so `sent`
below only confirms the local send succeeded, not that cerberus processed it.
Check cerberus's own debug log for a `[WS] DATA ... body=...` line (with
verbose debug logging on) to confirm that.

Requires the `websocket-client` package: `pip install websocket-client`

```
./ws_send_event.py ARM                          # gate_id defaults to WS_TEST_GATE
./ws_send_event.py START MY_GATE_LABEL           # override gate_id
./ws_send_event.py sequence                      # NEW_MOUSE + 4 ARM/START/GOAL cycles, one connection
./ws_send_event.py sequence --repeats 10         # 1 + 10 cycles instead of 1 + 3
CERBERUS_HOST=192.168.1.50 ./ws_send_event.py GOAL
```
