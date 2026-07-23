# Gate Event Test Scripts

Manual curl test scripts for the `POST /api/event` endpoint (see
`DESIGN-REQUIREMENT.md` section on the Asynchronous HTTP Listener,
implemented in `src/net/http-server.h` / `src/race/race-command-source.h`).

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
| `event`  | string | One of `ARM`, `START`, `GOAL` (also `RESTART`, `NEW_MOUSE`, accepted but not covered by these scripts). Anything else is rejected. |
| `tsf_us` | uint64 | Gate's local reading of the shared Wi-Fi TSF clock at the moment of the event. |
| `gate_us`| uint64 | Gate's own free-running microsecond timer, used for drift-compensation cross-referencing against `tsf_us`. |

Responses:
- `200 {"status":"ok"}` -- event accepted and queued.
- `400 {"status":"error","reason":"unrecognised event"}` -- `event` didn't match a known command.

## Scripts

- `send-arm-event.sh`
- `send-start-event.sh`
- `send-goal-event.sh`

Each sends one event with dummy `tsf_us`/`gate_us` values (not real TSF/timer
readings -- fine for exercising the HTTP handler and race state machine, not
for timing accuracy testing).

### Usage

```
./send-arm-event.sh              # gate_id defaults to ARM_GATE
./send-start-event.sh            # gate_id defaults to START_GATE
./send-goal-event.sh             # gate_id defaults to GOAL_GATE

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
