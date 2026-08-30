# CERBERUS: Multi-Gate Race Timer System Description

CERBERUS is a multi-gate timing system for micromouse-style runs. A central controller (this project) runs on a Cheap Yellow Display (CYD) board and combines local physical inputs, a serial link to a host PC, and a WiFi station connection to a shared network. The controller is the definitive record for all run and session timing.

## Hardware Target

* **MCU:** ESP32 or ESP32-S3 (dual-core).
* **Display & Touch:** Cheap Yellow Display (CYD) board with SPI TFT screen and XPT2046 touch controller.
* **Storage:** Onboard SD card slot sharing the SPI bus with display and touch.
* **Local inputs:** Four physical buttons (GPIO or I2C NeoKey expander, depending on board — see `INPUT-SYSTEM.md`), mapped to race commands ARM, START, GOAL (plus RESTART, via ARM's long-press). Touch drives on-screen navigation only, not race commands.
* **Status LEDs:** Four onboard NeoPixel LEDs, independently controllable.

---

## Core Architecture & Execution Model

The system uses an event-driven, decoupled, asynchronous architecture managed by FreeRTOS. Tasks communicate via thread-safe FreeRTOS queues.

```
Physical Buttons (NeoKey)  → InputEvent Queue  → race_timer_handle_command()
Serial Parser               → SystemEvent Queue ↑
HTTP Server                  → SystemEvent Queue ↑

Touch → screen navigation only (loadScreen()); not a race-command producer
```

### Core Assignment

* **Core 0:** Network stack, WiFi station connection, asynchronous HTTP/WebSocket server engine. CERBERUS joins the shared network as a client (AP is separate, not CERBERUS) and accepts a persistent WebSocket connection (`/ws`) from each remote intelligent gate, parses JSON event frames, and injects events into the SystemEvent queue. A `POST /api/event` HTTP endpoint with the same JSON schema still serves as a secondary path.
* **Core 1:** Main application task (race state machine, display ownership), local input polling task (physical buttons), and serial driver task (host link).

### Debug Output Policy

All debug output is written to the host UART with a `#` prefix, making each debug line a comment to the legacy host protocol parser (which only parses text between `<` and `>` and treats `#` lines as comments to skip). This scheme is safe to use at any time, including while the Serial Driver Task is active.

Debug output is implemented via `debug_print()`, `debug_println()`, and `debug_printf()` functions in `firmware/src/debug-log.h` (lines 57–89). These always write to Serial, protected by a `serial_write_mutex` shared with protocol writes to prevent interleaved corruption across Core 0/Core 1 producers.

High-frequency per-event traces (button/touch/GPIO input events in `firmware/src/input-events.h` and inbound serial RX echo in `firmware/src/net/serial-protocol.h`) are gated behind the Settings screen's "Verbose Debug" toggle (`objects.sw_debug_verbose`, toggled in `firmware/src/eez-actions.cpp:145–155`, persisted to NVS via `firmware/src/settings-store.h`). This flag controls the `g_debug_verbose_enabled` variable (`firmware/src/debug-log.h:46`). One-shot lifecycle messages (WiFi connect, HTTP server start, etc.) are unconditional and remain visible regardless of this setting.

---

## Safe Memory & Data Structures

### `SystemEvent` Struct

A fixed-size struct passed by value into the Main Event Queue. No dynamic allocation is permitted.

```c++
struct SystemEvent {
  RaceCommand type;           // ARM, START, GOAL, RESTART, etc.
  uint64_t timestamp_us;      // TSF time if remote, esp_timer_get_time() if local
  char payload[32];           // Mouse name (RESTART, from a NewMouse) or gate_id (HTTP)
  bool payload_is_mouse_name; // Disambiguates payload content
};
```

Defined in `firmware/src/race/system-event-queue.h:19–28`.

### `RaceCommand` Enum

Defined in `firmware/src/race/race-timer.h:76–86`. Values: `NONE`, `ARM`, `START`, `GOAL`, `RESTART`, `ENTER_CALIBRATION`, `RESUME_TIMER`, `EXTRA_RUN`.

---

## Task Breakdown & Functional Specifications

### 1. Input Layer & Event Generation

See `INPUT-SYSTEM.md` for complete details on local button hardware, debouncing, and event routing.

* **Local Input Polling Task (Core 1):** Polls GPIO buttons and the I2C NeoKey expander every 15ms, handles debouncing in software, maps valid inputs to `RaceCommand` (ARM/START/GOAL, plus RESTART via ARM's long-press), and pushes to the race state machine via `input_event_handler()` with local `esp_timer_get_time()` timestamp. Touch is polled separately by LVGL's own input device and drives on-screen navigation only — it is not on this task and does not produce race commands.
* **Asynchronous HTTP/WebSocket Listener (Core 0):** Connects to shared WiFi network as a station and runs an async web server on port 80. Remote intelligent gates hold a persistent WebSocket connection to `/ws` (replacing an earlier per-event TCP connect+POST+close cycle) and send timing events as JSON text frames:
  ```json
  {
    "gate_id": "START_GATE",
    "event": "START",
    "tsf_us": 4321098765,
    "gate_us": 1098765634
  }
  ```
  CERBERUS acks each event back over the same WebSocket connection (`{"ack_tsf_us": ...}`) so the gate's retry logic knows it landed. The same JSON schema is also still accepted over `POST /api/event` as a secondary path. Either way, the handler parses JSON, constructs a `SystemEvent`, and pushes it to the Main Event Queue via `system_event_post()`. Shared parsing in `firmware/src/net/http-server.h`'s `handle_gate_event_json()`; WebSocket handling in `ws_event_handler()`, HTTP handling in `http_handle_event()`.
* **Serial Monitor Task (Core 1):** Bidirectional owner of host UART. RX: parses legacy bracket-CSV protocol (`<type,value>`) per `preferredMessageSequencesV2.pdf`, pushes `SystemEvent` to Main Event Queue via `system_event_post()`. TX: mirrors every generated race event back to the host in real time using the legacy protocol. See `firmware/src/net/serial-protocol.h` for parsing, `firmware/src/net/messages.h` for protocol constants, `firmware/src/race/race-serial-telemetry.h` for TX telemetry tick.

### 2. Main Processing & State Machine (Core 1)

* **Resource Ownership:** The main application task holds exclusive ownership of the display (LovyanGFX) and the race state machine.
* **Operation:** Loops using a bounded-timeout queue receive so it wakes on a short tick (~30–50ms) even with no event pending — required for live timer redraw. On timeout with no event, redraws active timers; on a real message, advances the state machine and updates the display.
* **Race State Machine:** Manages the sequence of states for a single mouse run. See `docs/RACE-STATE-MACHINE.md` for the authoritative state diagram and transition rules.
  * **States** (from `firmware/src/race/race-timer.h:88–96`): `CALIBRATE` (boot), `NEW_MOUSE` (the reset-for-a-new-entry step — name/run-count/timers — but never actually stored in `race_state`: `race_timer_enter_new_mouse()` runs that reset and writes `WAITING` directly, since nothing can observe `race_state` mid-call anyway; kept as an enum value for switch-exhaustiveness), `WAITING` (idle, waiting for start), `ARMED` (mouse in start cell), `RUNNING` (active race), `GOAL` (run finished), `TIMED_OUT` (run exceeded entry time).
  * **Entry Time Countdown:** A per-mouse countdown starting at first ARM, configured via host `MSG_ENTRY_TIME_S` (default 600s). Stored in `g_entry_time_s_limit` (`firmware/src/race/race-timer.h:163`), displayed on screen, clamped to zero. Behaviour on expiry is already decided (freeze timer, do not auto-advance state).
* **Leaderboard:** Computes top finishers from completed runs, displayed on-screen (top 5 only due to space) and served via HTTP `GET /leaderboard` for full standings in a browser.

### 3. HTTP Server

Implemented in `firmware/src/net/http-server.h` using `AsyncWebServer`. Endpoints:

* `GET /` — Liveness check, returns `"CERBERUS OK"`.
* `WS /ws` — Persistent WebSocket connection for gate event ingestion; primary transport for `hesperus-timing-gate` boards. Parses JSON (`gate_id`, `event`, `tsf_us`, `gate_us`), maps `event` string to `RaceCommand` via `race_command_from_http()`, posts `SystemEvent` to queue, acks back over the same connection.
* `POST /api/event` — Same JSON schema and dispatch as `/ws`, as a secondary one-shot HTTP path.
* `GET /leaderboard` — Server-rendered HTML leaderboard, shows all completed runs (not just top 5), with a live-update mechanism via `EventSource('/events')` for single-page refresh on run completion.
* `GET /time` — Returns current Unix timestamp (ms) for browser clock sync every 10s.
* `GET /events` — Server-Sent Events endpoint; fires a message when a run is committed, triggering the leaderboard page to reload.

### 4. WiFi & Network

Implemented in `firmware/src/net/wifi-manager.h`.

* **Non-blocking connect:** Runs as a background Core-0 task, polling WiFi status every 250ms. Reacts to connect/disconnect edges, not one-time checks, so reconnects are logged whether instant (cached NVS) or mid-retry.
* **Fallback:** Local racing works even with no router present; WiFi joins whenever the router becomes available, no reboot needed. There's no connect timeout — the task retries forever and never forces a hand-off to WiFi provisioning; that only happens on an explicit `wifi_request_provisioning()` call from the WiFi Setup screen.
* **Credential storage:** SSID and password cached in NVS (`firmware/src/net/secrets.h`), loaded at boot.
* **Status display:** WiFi connection status shown on the main screen's status bar (WIFI + dBm when connected, red "MANUAL" when not) — not on the NeoKey.

### 5. Serial Protocol & Legacy Host Interop

Implemented in `firmware/src/net/serial-protocol.h`, `firmware/src/net/messages.h`, `firmware/src/race/race-command-source.h`.

* **Inbound:** Legacy bracket-CSV format (`<type,value>\r\n` or `<type,name>\r\n`). Parses all 8 RATS V2 message types from `preferredMessageSequencesV2.pdf` (dated 24 July 2026):
  * `MSG_NEW_MOUSE=98` — Maps to `RaceCommand::RESTART` (works from any state).
  * `MSG_ENTRY_TIME_S=93` — Sets `g_entry_time_s_limit`.
  * `MSG_ALLOWED_RUNS=94` — Sets `g_allowed_runs`, enforced by `race_timer_allowed_runs()` (`race-timer.h:310–319`) as the run-count cap.
  * `MSG_EVENT_NAME=95` — Stores event name (metadata, not used).
  * `MSG_CONTEST_NAME=96` — Stores contest name (metadata, not used).
  * `MSG_REQUEST_TYPE=97` — Replied to with fixed `MSG_TIMER_TYPE=96` response (`"1CH"`).
  * `MSG_EXTRA_RUN=92` — Posts `RaceCommand::EXTRA_RUN`, which decrements `mouse_run_count` to "undo" a false-start run (`race-timer.h:407–412`).
  * `MSG_SET_MODE=99` — `"CALIBRATION"` posts `RaceCommand::ENTER_CALIBRATION`, `"TIMER"` posts `RaceCommand::RESUME_TIMER`; both are handled in `race_timer_handle_command()`. Any other value is logged and ignored.
* **Outbound:** State transitions (`MSG_CURRENT_STATE=4`), timing splits/runs (`MSG_C1_SPLIT_TIME=12`, `MSG_C1_RUN_TIME=13`), watchdog/health (`MSG_WATCHDOG=0` every 1000ms), course-time marker (`MSG_COURSE_TIME_MS=30`). Driven by the telemetry tick in `firmware/src/race/race-serial-telemetry.h`, called once per `loop()` iteration.

### 6. NVS & Persistent Settings

Stored in ESP32 flash via Arduino Preferences API:

* **Display calibration** (`touch-calibration.h`): XPT2046 touch offsets.
* **Debug settings** (`settings-store.h`): Verbose debug enabled/disabled flag.
* **WiFi credentials** (`net/secrets.h`): SSID and password.
* **Boot counter** (future): Not yet implemented; see Planned Updates.

---

## Time Synchronization

Remote intelligent gates and CERBERUS all connect to a shared WiFi AP (a separate travel router), which broadcasts a unified global clock via the 802.11 Timing Synchronization Function (TSF) in its beacon frames. Each gate reads the AP's TSF as `tsf_us` when an event occurs, and that timestamp is sent to CERBERUS in the event's JSON body (over `/ws` or `/api/event`).

CERBERUS accepts `tsf_us` at face value as the event's absolute timestamp (`SystemEvent.timestamp_us = tsf_us`, `handle_gate_event_json()` in `http-server.h`). Local events (button presses, serial commands) are timestamped with the local `esp_timer_get_time()` reading at arrival time. Local timestamps thus lack the global TSF reference but are sufficient for race timing (millisecond resolution).

The `gate_us` field (gate's own free-running microsecond timer) is parsed but currently not used. See Planned Updates for the planned TSF-based drift-compensation scheme.

---

## Coding Requirements

* Generate clean, highly modular, thread-safe C/C++ code utilizing the Arduino-ESP32 core framework.
* Ensure all SPI bus transactions are properly guarded.
* FreeRTOS queue API interactions must check for timeout constraints.
* No dynamic memory allocation is used inside the execution path.
* Debug output uses the `debug_print()/debug_println()/debug_printf()` functions from `firmware/src/debug-log.h`, which always emit `#`-prefixed lines safe to send at any time on the shared UART.
* Use Doxygen-compatible comments throughout.

---

## References

* **Race state machine:** `docs/RACE-STATE-MACHINE.md` (authoritative state diagram and transition rules).
* **Input hardware & debouncing:** `docs/INPUT-SYSTEM.md`.
* **Operating the device on race day:** `docs/OPERATOR-GUIDE.md`.
* **Serial protocol detail:** `tools/testing/SERIAL-TEST-PLAN.md`, `docs/preferredMessageSequencesV2.pdf`.
* **Leaderboard implementation:** `firmware/src/race-timer.h`, `firmware/src/net/http-server.h`.
