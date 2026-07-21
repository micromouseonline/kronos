# CERBERUS Implementation Plan

Single working document for the current round of work: serial interface (legacy
host protocol), WiFi connect, HTTP server, HTTP-fed event queue, leaderboard
page. Supersedes ad-hoc planning notes elsewhere — this file is the one to
read for current status and next steps.

Execute stages sequentially, one at a time. After each stage: build (`pio run`
for every board env touched) and report the result. Do not speculatively fix
build errors — report and wait. Four-step handoff per stage, in order: (1)
manual hardware verification confirms the stage is correct, (2) this doc's
Status table is updated to mark it done, (3) the user makes the git commit
(never done by the assistant), (4) the user says to proceed before the next
stage starts.

---

## Status

| Stage | Work | Build | Manual verify | Status |
|---|---|---|---|---|
| A | `net/messages.h` protocol constants + `SystemEvent` queue | PASS (all 5 envs) | PASS | **done** |
| B | Serial RX parsing, `MSG_NewMouse` → `RaceCommand::RESTART` | PASS (all 5 envs) | PASS | **done** |
| C | Debug Output Policy migration | PASS (all 5 envs) | PASS | **done** |
| D | Serial TX telemetry | PASS (all 5 envs) | PASS | **done** |
| E | WiFi connect (non-blocking) | PASS (all 5 envs) | PASS | **done** |
| F | `boards.ini` HTTP feature block | — | — | not started |
| G | HTTP server + `POST /api/event` | — | — | not started |
| H | Leaderboard page (`GET /`) | — | — | not started |
| I | `race_runs[]` concurrency guard (optional) | — | — | deferred until after G/H |
| J | Docs sync (this file + header comments) | — | — | not started |

---

## Context

State machine, LVGL display, and physical UI (GPIO/NeoKey/touch) are already
built and working (see `USER-INPUT-SYSTEM.md`). This round adds: a serial
link to legacy host PC software, WiFi station connect, and an HTTP server
(gate event ingestion + a spectator leaderboard page).

`DESIGN-REQUIREMENT.md` describes a fuller target architecture (full
`SystemEvent`/Main Event Queue, `READY`/`RACING`/`MAINTENANCE` supervisor,
NVS/SD logging). This round builds only what's needed for serial + WiFi +
HTTP; the supervisor and logging layers are deferred (see mapping table
below).

## Key findings from review

- `src/messages.h` (project root, unused/dead) holds the **legacy
  `<type,value>\r\n` numeric protocol** from the old VB-host project — this
  is the one CERBERUS must interoperate with (legacy PC-side equipment), not
  the ASCII-line example (`NEW_MOUSE:MightyMouse\n`) originally sketched in
  `DESIGN-REQUIREMENT.md`.
- **Bug found and fixed in scope**: `RaceCommand::NEW_MOUSE` is a no-op while
  `race_state` is `WAITING`/`RUNNING`/`GOAL` (`race-timer.h`'s
  `race_timer_handle_command`) — only `ARM`/`RESTART` act there.
  `RaceCommand::RESTART` already works from every state (same reasoning as
  the existing "hold ARM for new mouse regardless of state" fix).
- `src/net/wifi-manager.h` already has working `wifi_connect()`/
  `is_wifi_active()`, and `src/net/secrets.h` already has real credentials —
  neither wired into `main.cpp` yet.
- `input-events.h`'s queue is explicitly local-hardware-only; Serial/HTTP get
  their own producer + queue per its own header comment.
- No HTTP/JSON library in `boards.ini` yet.

## Decisions

1. Serial protocol = legacy `<type,value>\r\n` bracket-CSV, adapted into
   `src/net/messages.h` (was an empty stub). Only inbound message needed:
   `MSG_NewMouse=98`.
2. Received `MSG_NewMouse` maps to `RaceCommand::RESTART` (not `NEW_MOUSE`)
   so it always works regardless of current race state.
3. Mouse-name extraction from the wire message is **out of scope this
   round** — the legacy format carries no name (value always 0). A TODO is
   left in code (`system-event-queue.h`, `race-command-source.h`) for later.
4. Architecture scope: incremental. `SystemEvent` struct + a new Main Event
   Queue (same pattern as `input-events.h`), fed by Serial RX and HTTP POST,
   both draining into `race_timer_handle_command()`. The
   READY/RACING/MAINTENANCE supervisor, logging queue, NVS/SD, and
   `/download` are **not** built this round.
5. Local buttons keep their existing direct-call path
   (`input_event_handler` → `race_timer_handle_command()`), unchanged — not
   rerouted onto the new queue. Both paths converge on the same
   state-machine entry point.
6. WiFi connect runs as a **non-blocking** Core-0 background retry task —
   local racing works even with no router present; WiFi joins whenever the
   router becomes available, no reboot needed.
7. `RaceState::TIMED_OUT`'s legacy `MSG_CURRENT_STATE` telemetry code reuses
   `4` (RUNNING) — no legacy equivalent exists, this is the closest semantic
   match.
8. The legacy `MSG_CURRENT_STATE` trailing `last_char` annotation quirk is
   **kept** (byte-for-byte compatibility with legacy host software), even
   though nothing in this codebase currently sets it to anything but `#`.

---

## Stage detail

**A. `src/net/messages.h` + `src/race/system-event-queue.h`** — protocol
constants (`MSG_WATCHDOG=0`, `MSG_CURRENT_STATE=4`, `MSG_C1_SPLIT_TIME=12`,
`MSG_C1_RUN_TIME=13`, `MSG_COURSE_TIME_MS=30`, `MSG_NEW_MOUSE=98`,
`MSG_SET_MODE=99` unused) + `serial_send_message()`/`serial_send_run_time()`.
`SystemEvent{RaceCommand type; uint64_t timestamp_us; char payload[32];}`,
32-deep queue, `system_event_queue_init/post/drain()` — mirrors
`input-events.h`'s existing queue pattern. Wired into `main.cpp`
(`system_event_handler` calls `race_timer_handle_command(evt.type)`).
*Verify:* temporary mock-event push in `setup()` (`RESTART, ARM, START,
GOAL` ×3) drains in FIFO order and drives real state transitions visible on
screen at boot (ends in `GOAL`). Remove the mock push once Stage B gives the
queue a real producer.

**B. `src/net/serial-protocol.h`** (new) — `SerialLine{int type; long
value;}`, line parser, Core-1 RX task (64-byte buffer, split on `\n`). Add
`race_command_from_serial(const SerialLine&)` to `race-command-source.h`
(replaces its TODO stub): `MSG_NEW_MOUSE` with `value == 0` (per the
protocol doc's "value argument will always be passed as 0") →
`RaceCommand::RESTART`, else `NONE`. *Verify:* type `<98,0>\r\n` in a serial
terminal from any race state; confirm new-mouse transition happens
regardless of state. **Findings from hardware testing, fixed in this
stage:** (1) line terminator accepts CR, LF, or CRLF, not just `\n` — some
terminals send CR-only; (2) added a `[serial-protocol] rx: "..."` receipt
echo so it's visible what bytes actually arrived; (3) the NeoKey LED update
only ran from `input_event_handler` (local buttons), so a serial-driven
state change left the LEDs stale — factored into a shared
`neokey_reflect_race_state()` called from both `input_event_handler` and
`system_event_handler`; (4) `src/messages.h` (legacy, dead, unused) renamed
to `src/messages-legacy-reference.h` to stop colliding with the active
`src/net/messages.h`.

**C. Debug Output Policy migration** — new `src/debug-log.h` with
`debug_print/println/printf` gated on a `g_uart_owned_by_protocol` flag (set
by `serial_protocol_init()`). Swap raw `Serial.print*` call sites:
`main.cpp`, `net/wifi-manager.h`, `neokey/neokey-driver.h`,
`input-events.h`'s `debug_print()`, `eez-actions.cpp`'s calibrate stub,
`display/touch-calibration.h`. Leave `net/messages.h`'s own writes raw
(that's the protocol itself). `wifi-scan.h`/`net/esp32-info.h` need no
change — dead code, zero call sites. *Verify:* `grep -rn
"Serial\.\(print\|println\|printf\)" src` matches only `net/messages.h`
afterward.

**D. `src/race/race-serial-telemetry.h`** (new) — `race_state_to_legacy_code
(RaceState)` (explicit remap table, not a cast), `race_serial_telemetry_tick
()` called once per `loop()`: `MSG_CURRENT_STATE` on state change,
`MSG_COURSE_TIME_MS`/`MSG_C1_SPLIT_TIME`/run-time messages on the relevant
transitions, `MSG_WATCHDOG` every 1000ms. *Verify:* ARM→START→GOAL via
physical buttons with a serial terminal open; clean `<type,value>` lines, no
interleaved debug text.

**E. WiFi connect** — `wifi_connect_start_async()` in `wifi-manager.h`:
background Core-0 task, called from `setup()` (after the serial-ready wait)
instead of a blocking call. Polls `WiFi.status()` on a 250ms tick and reacts
to edges (was-connected vs connected) rather than a monitor-then-check-once
structure, so a connect is logged whether it happens instantly (cached NVS
association) or mid-retry. Initial `WiFi.begin()`, then `WiFi.reconnect()`
on drop/retry (lighter than re-issuing `begin()`, since credentials are
already applied). *Verify:* boots and races locally with router off;
connects within a normal DHCP window once router is on, no reboot needed;
survives a live router radio toggle and reconnects without a device reboot.
**Findings from hardware testing, fixed in this stage:** (1) none of the 5
target boards has a working onboard status LED (`STATUS_LED=-1` on three,
`HAS_LED` missing on `jc2432w328c`, and the nominal `HAS_NEOPIXEL` board's
pin doesn't light in practice) — Wi-Fi status moved off `StatusLED` onto
NeoKey key 3 (`WIFI_STATUS_KEY`, the `BTN_TOUCH` position), which
`neokey_reflect_race_state()` (Stage B) no longer touches, so it's owned
exclusively by Wi-Fi status; (2) Stage C's Debug Output Policy (gate ad-hoc
output off entirely once the serial protocol owns the UART) made
`wifi_connect_task`'s own connect/reconnect messages invisible — revised so
`debug-log.h` always prints, prefixed with `#` per-line (the host
supervisor treats `#` lines as comments to skip, confirmed safe at any
time); added `serial_write_mutex` (`debug-log.h`) guarding every UART write
across producers (ad-hoc debug output and `messages.h`'s
`serial_send_message()`), since unsynchronized Core-0/Core-1 writes could
interleave and silently corrupt or drop a line; (3) an earlier
monitor-then-check-once structure silently missed real connects (already
connected at task start from NVS cache, or connecting slightly after a 10s
window gave up) — replaced with the edge-detection loop described above,
confirmed against a captured `CORE_DEBUG_LEVEL=5` log (temporary, reverted)
showing Arduino-ESP32's own auto-reconnect already retrying every ~2.5s
independently of this task, and a real handshake+DHCP completing in well
under a second once the AP was reachable.

**F. `boards.ini` / `platformio.ini`** — new `[feature_http]` block
(`AsyncTCP`, `ESPAsyncWebServer` — verify exact maintained-fork package
names via `pio pkg search` at implementation time, `ArduinoJson @ ^7`),
wired into all 5 board envs via the existing `extends`/`${parent.key}`
convention. *Verify:* `python3 tools/check_ini_composition.py`, then `pio
run` all 5 envs.

**G. `src/net/http-server.h`** (new) — `AsyncWebServer` on port 80. `GET /`
stub first (`"CERBERUS OK"`), then `POST /api/event` (JSON body
`gate_id`/`event`/`tsf_us`/`gate_us` per `DESIGN-REQUIREMENT.md`) parsed via
`AsyncCallbackJsonWebHandler`, mapped through a new
`HTTP_EVENT_COMMAND_MAP` in `race-command-source.h` (mirrors
`BUTTON_COMMAND_MAP`), pushed via `system_event_post()`. Note:
`gate-controller-python-test-cerberus/server.py` is an unrelated GET-based
prototype — not this JSON POST contract. *Verify:* `curl` GET/POST both
work; 5 POSTs 20ms apart don't drop/crash.

**H. Leaderboard page** — replace the `GET /` stub with server-rendered HTML
from `race_timer_compute_leaderboard()` + `race_timer_format_time()`. No JS
framework; optional meta-refresh. *Verify:* run laps via buttons, confirm
browser leaderboard matches on-screen panel.

**I. (optional, not blocking)** — `portMUX`/mutex guard around `race_runs[]`
append + leaderboard read, since Core 0 (HTTP) now reads what Core 1 (state
machine) writes. Worst case today is a benign stale read, not corruption.
Only do this if it becomes a real problem.

**J. Docs sync** — update `race-timer.h`/`input-events.h` header comments
(RESTART now has producers; Main Event Queue now exists),
`USER-INPUT-SYSTEM.md`, and this file's Status table.

---

## Mapping to the original phase/step plan

| Original step | Status | Note |
|---|---|---|
| 1.1 Data Structures & Main App Task | REVISED | `SystemEvent`/queue built (Stage A); no separate high-priority task — `loop()` still plays that role |
| 1.2 Supervisor (READY/RACING/MAINTENANCE) | DEFERRED | Out of scope this round |
| 2.1 Local Input Polling | DONE | Unchanged |
| 2.2 HTTP Input Handler | IN PROGRESS | `POST /api/event` (Stage G); leaderboard `GET /` added as new scope (Stage H) |
| 2.3 Serial Monitor Task | IN PROGRESS | Legacy bracket-CSV protocol (Stages B-D), not the ASCII-line example originally sketched; only `MSG_NewMouse` parsed inbound |
| 3.1 NVS/Storage | DEFERRED | |
| 3.2 Logging Queue | DEFERRED | |
| 4.1 Exclusive Display Updates | DONE | Retroactively — `race-timer-display.h` already does this |
| 4.2 Maintenance/Log Streaming | DEFERRED | |

New steps not in the original plan: Debug Output Policy migration (Stage C,
own step — was buried in 2.3's text), leaderboard page (Stage H).
