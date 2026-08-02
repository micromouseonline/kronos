# Test Tooling Reference

An inventory of every test/bench tool in this workspace: what it is, what
it tells you, and how to invoke it. Factual reference only — for the
investigation these tools were built to support (network latency, event
reliability, race-timing correctness), see `NETWORK-TIMING-ISSUE.md`, which
cross-references specific tools by name from within each issue's
Confirmation/Verification sections.

Two categories: bench/hardware tools (need real gate hardware, exercise
real Wi-Fi/TSF timing) and native unit tests (deterministic, no hardware,
run in CI-style fashion).

## Bench/hardware tools

These require a real `cerberus-gate-controller` board, plus, depending on
the tool, either real `hesperus-timing-gate` boards or `ares-pulse-generator`
standing in for one.

### `ares-pulse-generator` — GPIO trigger simulator

A PlatformIO/Arduino firmware project (`ares-pulse-generator/src/main.cpp`)
that drives three active-low GPIO outputs (`TRG_ARM`=7, `TRG_START`=6,
`TRG_GOAL`=5) to simulate photodiode/beam-break triggers, wired into a real
`hesperus-timing-gate` board's trigger inputs — bench-tests the gate/
cerberus system without needing a physical robot to run the course
repeatedly.

Build/flash: `pio run -e <env> -t upload` from inside `ares-pulse-generator/`,
one of `ares-pulser-s3-zero`, `ares-pulser-s3-super-mini`,
`ares-pulser-c3-super-mini`, `ares-pulser-c3-xiao`.

**Scenario selection is a runtime serial CLI** (115200 baud, USB serial).
Connect and type `help` (alias: `?`) to see the available commands
(`list`, `run`, `arm`, `status`) with a one-line description of each, or
`list` to see available trial names with a one-line description of each.
`run <name> [count] [interval_ms]` fires a trial immediately (blocks until
done, printing one `RUN <name>` line per rep then `OK <name>`); `arm <name>
[count] [interval_ms]` sets which trial the physical `BTN_IN` (pin 4)
button fires on its next press (`BTN <name>` per rep, `OK <name>` when the
repeats finish); `status` reports what's currently armed and with what
count/interval. `count` and `interval_ms` are both optional, defaulting to
1 rep and 1000ms between reps — pass `count` to reproduce a statistical run
(e.g. the old 100-rep pattern, or several `full_run`s back-to-back, e.g.
`run full_run 4`) without the host having to loop the command itself.
Unknown trial names print `ERR unknown trial: <name>` instead of silently
doing nothing.

| Trial name | Scenario | Key constants |
|---|---|---|
| `arm_pulse` | Single 100ms pulse on `TRG_ARM` — smoke test/baseline. | — |
| `goal_pulse` | Single 100ms pulse on `TRG_GOAL` — smoke test/baseline, and the single-gate `GOAL`-only steady-state traffic for the proposed WS-jitter characterization test (`run goal_pulse 10000 250`, ~42 minutes; see the "Unexplained minor WS jitter" issue in `NETWORK-TIMING-ISSUE.md`). | — |
| `arm_then_start` | `TRG_ARM` then `TRG_START`, edges 200ms apart — one board serving both ARM and START gates, robot crossing both in quick succession. | `ARM_TO_START_GAP_MS` (200) |
| `burst` | `BURST_COUNT` pulses on `TRG_GOAL`, `BURST_INTERVAL_MS` apart, each `BURST_PULSE_MS` wide — rapid-fire triggering, checks whether the send-side queue overflows/drops. Interval is chosen above the 50ms debounce but well under a ~250-270ms send cycle, so a queue backlog is expected. | `BURST_COUNT` (40), `BURST_INTERVAL_MS` (90ms), `BURST_PULSE_MS` (10ms) |
| `double_trigger` | Two `TRG_GOAL` pulses on the same pin, `DOUBLE_TRIGGER_GAP_MS` apart — a robot with a gapped/slotted structure breaking one gate's beam twice for what should count as one crossing. | `DOUBLE_TRIGGER_GAP_MS` (150ms) |
| `full_run` | One full ARM→START→GOAL sequence, `RUN_DURATION_MS` START-to-GOAL — end-to-end/leaderboard-facing checks (e.g. does the committed time come out exact) rather than a single stress dimension. Fire several with `run full_run <count>` (the old `four_runs` trial's fixed internal 4x-increasing-duration loop is now the caller's choice of count, all runs sharing `RUN_DURATION_MS`). Also the pattern to repeat throughout an external congested-airtime stress window (airtime saturation/bulk throughput/channel interference/broadband noise, see below) once one is running. | `RUN_DURATION_MS` (3000), `ARM_TO_START_GAP_MS` (200) |
| `wake_sweep` | One `TRG_ARM` pulse after each of `WAKE_SWEEP_GAPS_MS`'s idle gaps in turn (1s, 5s, 15s, 30s, 60s), printing the gap before each pulse — the wake-to-first-byte-after-idle-gap sweep the Wi-Fi power-save issue's verification method proposes, for comparing `WIFI_PS_NONE`/`MIN_MODEM`/`MAX_MODEM` modem-sleep wake cost. Takes ~111s (the sum of the gaps) to run. | `WAKE_SWEEP_GAPS_MS` ({1000,5000,15000,30000,60000}) |

Caveats: needs the physical pulser board wired to a real gate's trigger
inputs; no Wi-Fi logic in the pulser itself, it's pure GPIO.

### `cerberus-gate-controller/tools/testing/` — direct HTTP/WS event injection

Sends synthetic gate events straight at cerberus over the network, no
gate/pulser hardware needed at all — useful for exercising cerberus's own
handler/state-machine/dedup logic in isolation, or for crafting scenarios a
real gate could never naturally produce (e.g. a deliberately stale
timestamp). Endpoint schema (both HTTP and WS): `gate_id` (string, free
text), `event` (`ARM`/`START`/`GOAL`/`NEW_MOUSE`/`RESTART`), `tsf_us`
(uint64), `gate_us` (uint64). Full schema/response docs in that directory's
own `README.md`.

- **`send-arm-event.sh` / `send-start-event.sh` / `send-goal-event.sh` /
  `send-new-mouse-event.sh`** — one `curl -i -X POST` to `/api/event` each,
  fixed dummy `tsf_us`/`gate_us` (not real clock readings — fine for
  exercising the handler/state machine, not for timing accuracy).
  `./send-arm-event.sh [gate_id]` (default e.g. `ARM_GATE`),
  `CERBERUS_HOST=<ip>` env var overrides the default host.
- **`send-one-mouse.sh`** — one full unattended timed sequence over HTTP:
  `NEW_MOUSE` (1s) → `ARM` (200ms) → `START` (4s) → `GOAL`, then 3 more
  `ARM`(200ms)/`START`(random 1-5s)/`GOAL` cycles (13 events total).
  `tsf_us`/`gate_us` are stamped with real `date +%s%6N` at send time, so
  timestamps track the real delays. `./send-one-mouse.sh [gate_id]`,
  `CERBERUS_HOST` override. Prints one line per event
  (`HH:MM:SS EVENT HTTP <code>`), continues past failures, ends with an
  `N/13 events failed` summary.
- **`send-full-race.sh`** — runs `send-one-mouse.sh` back-to-back for
  soak/stability testing. `./send-full-race.sh [count] [gate_id]`, default
  20 repeats (80 cycles total), ~20-40s/sequence (~7-13 min for the
  default). A failing sequence is logged; the loop continues.
- **`ws_send_event.py`** — same schema, sent over the persistent `/ws`
  WebSocket endpoint (one held-open connection, matching how a real gate
  behaves post-persistent-connections) instead of a fresh HTTP connection
  per event. Requires `pip install websocket-client`.
  - `./ws_send_event.py ARM|START|GOAL|RESTART|NEW_MOUSE [gate_id]` — single
    event (`gate_id` defaults to `WS_TEST_GATE`).
  - `./ws_send_event.py sequence [--repeats N]` — `NEW_MOUSE` + one fixed
    10s ARM/START/GOAL cycle + `N` more random 3-10s cycles (default 3).
  - `--host HOST` / `CERBERUS_HOST` env var (default `cerberus.local`).
  - `--tsf-us N` — overrides the stamped-`now` `tsf_us` (single-event mode
    only). Used to hand-craft an arbitrarily old `GOAL` on demand — this is
    how the stale-event misattribution scenario is reproduced without
    needing to actually delay a real message in flight.
  - Fire-and-forget: no response comes back over the socket by design, so
    `sent`/`ERROR` in the output only reflects whether the local `send()`
    call succeeded, not that cerberus processed it. Cross-check cerberus's
    own debug log for a `[WS] DATA ... body=...` line to confirm receipt.
- **`SERIAL-TEST-PLAN.md`** (same directory) — a manual, checklist-style
  test plan for the host-PC serial link (RATS V2 protocol), not an
  executable tool. Requires a serial terminal at 9600 baud, CR+LF line
  ending, and a physical board.

### `tools/cerberus_log_stats.py` — latency/gap analysis

The primary bench-data analysis tool. Extracts event-receipt lines from a
cerberus debug log and computes one-way trigger-to-receipt latency as
`recv_ms*1000 - tsf_us` — valid without NTP or any clock sync, since
cerberus and hesperus share the Wi-Fi TSF timeline as stations on the same
AP. Matches on `#[recv_ms] ... body={...}` — deliberately loose about
what precedes `body=`, so it works unmodified on both `[HTTP] POST
/api/event` lines and `[WS] DATA` lines.

```
python3 tools/cerberus_log_stats.py LOGFILE [--event GOAL] [--csv out.csv] [--gaps] [--no-table]
python3 tools/cerberus_log_stats.py -              # read from stdin
```

- `--event TYPE` — filter to one event type (`GOAL`, `START`, `ARM`, …).
- `--gaps` — print consecutive `d_gate_ms`/`d_latency_ms` deltas between
  events (the burst/queueing diagnostic — a growing `d_latency_ms` against
  a roughly-constant `d_gate_ms` is the queueing signature).
- `--csv PATH` — write rows (`line, recv_ms, gate_id, event, tsf_us,
  gate_us, latency_us`) to CSV.
- `--no-table` — suppress the per-row table, stats only.

Output: a per-row table (`recv_ms, event, tsf_us, gate_us, latency_ms`),
then per-event-type and overall latency stats — mean/median/stdev/min/max/
p90/p95/p99, all in ms. This exact format is what appears in the
`*-analysis.txt` companion files in `test-data/` (see below).

### `test-data/` — raw captures and their analysis

Bench-run artifacts live at the workspace root, as paired files:
`<scenario>[-ws]-<YYYYMMDD>-<HHMM>.txt` (raw cerberus debug-log capture)
and, for most, a companion `<same-name>-analysis.txt` (the literal stdout
of `cerberus_log_stats.py` run against it). The `-ws` suffix marks a
WebSocket-transport run, distinguishing it from the HTTP-default baseline
of the same scenario — used throughout `NETWORK-TIMING-ISSUE.md` to compare
before/after a transport change.

## Native unit tests

Deterministic, hardware-free — validate firmware logic in isolation, not
real Wi-Fi/TSF timing or network transport. Run from inside
`cerberus-gate-controller/`:

```
pio test -e native
```

Uses `platform = native` / `test_framework = unity` with mock headers in
`test/native_mocks/` (`Arduino.h`, `debug-log.h`) so firmware logic
compiles and runs on the host, no board needed.

- **`test_race_timer`** (`test/test_race_timer/test_race_timer.cpp`) —
  `race/race-timer.h`: time-formatting helpers, leaderboard construction/
  sorting/tie-handling/truncation, and the full race state machine
  transition table (`CALIBRATE`→`WAITING`→`ARMED`→`RUNNING`→`GOAL`,
  RESTART/NEW_MOUSE handling, stale-GOAL rejection, the exact-start-tsf
  zero-time edge case, max-runs-exhausted behaviour).
- **`test_gate_event_dedup`** (`test/test_gate_event_dedup/test_gate_event_dedup.cpp`) —
  `net/gate-event-dedup.h`'s `gate_event_is_duplicate()`: first-sighting
  vs. exact-repeat, new-tsf-on-same-key, different-gate-id/different-event
  collisions, and round-robin table-wraparound past capacity.

## Not yet built: congested-airtime stress testing

Sketched in `hesperus-timing-gate/review.md`'s "Possible stress tests"
section, referenced from `NETWORK-TIMING-ISSUE.md`'s outstanding-work list,
but no script/tool exists for any of these yet — manual setup only:

- **Airtime saturation** — several extra ESP32s (or laptops) running a
  tight HTTP-request loop against a local server, associated to the same AP.
- **Bulk throughput** — `iperf3 -s` on one machine, `iperf3 -c <ip> -t 300`
  from another, same AP.
- **Channel interference** — a second router broadcasting on an
  overlapping channel.
- **Broadband noise** — a 2.4GHz noise source (a microwave oven ~30cm from
  the AP is noted as a surprisingly effective blunt stressor).
