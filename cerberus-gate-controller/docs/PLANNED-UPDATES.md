# CERBERUS: Planned Updates & Future Work

This document tracks architecture and feature work deferred from the current implementation.

---

## Supervisor State Machine (READY/RACING/MAINTENANCE)

Original design envisioned a three-level supervisory state machine distinct from the per-mouse race state machine. Current implementation is simpler: the race state machine alone governs timing flow, and screen navigation is decoupled via EEZ Studio's `loadScreen()` (see `USER-INPUT-SYSTEM.md:125`).

A future supervisor layer could centralize mode-switching logic, but this is **not blocking current operation** and may not be necessary if the simpler architecture continues to work.

---

## Logging Infrastructure (SD Card + CSV Format)

Not yet implemented. Original design in `SYSTEM-DESCRIPTION.md` (earlier drafts) called for:

* **`LogMessage` struct:** Fixed-size, serializable to a single canonical CSV line.
* **CSV output:** Appended to SD card files (named `/logs/RACE_NUM_[Counter].CSV`), simultaneously mirrored to host over serial.
* **NVS boot counter:** Non-volatile counter incremented on each boot, used to name log files sequentially without colliding with previous boots.
* **Log file pointers:** `/logs/LATEST.TXT` rewritten on each boot to point to the current session's CSV file.

**Rationale:** A canonical CSV format ensures SD-logged data and host-mirrored data are byte-identical, and a saved `.CSV` session file can be replayed over the serial link later to reproduce an identical event/timing stream (no separate playback protocol needed).

**Blocker:** Requires integrating an SD card driver (SPI) with the existing display/touch drivers (also SPI) — bus sharing and concurrency guards. Partition-table updates to reserve SPIFFS/SD space are already done (`boards.ini`).

---

## HTTP Log Streaming & Maintenance Mode

Not yet implemented. Once SD-card logging exists:

* A new `MAINTENANCE` supervisor state would halt all incoming race/lap timing triggers and lock the system for file operations.
* An HTTP `GET /download?file=RACE_NUM_123.CSV` endpoint would stream a requested CSV log from the SD card to a host browser in chunks, with short `vTaskDelay` yields to prevent starving background tasks.

---

## TSF-Based Drift Compensation

Not yet implemented. Original design called for:

* Gates include both `tsf_us` (WiFi AP's global TSF clock) and `gate_us` (gate's own free-running microsecond timer) in every `POST /api/event` message.
* CERBERUS would track the relationship between its own `esp_timer_get_time()` and the incoming TSF values to model clock drift dynamically.
* During WiFi beacon loss or AP time shifts, a gate's `gate_us` cross-reference would allow CERBERUS to compensate for the drift and maintain microsecond-level accuracy for up to ~5 minutes of independent operation.

**Current state:** `gate_us` is parsed but not used (`src/net/http-server.h:219`). TSF timestamps (`tsf_us`) are taken at face value. CERBERUS has no local TSF reader.

**Rationale:** With an AP that stays online and stable, drift is negligible; this is forward-looking insurance for edge cases (transient AP reset, dynamic mesh topology, RF interference).

---

## `race_runs[]` Concurrency Guard (Optional)

Not yet implemented. Currently, Core 1 (state machine) appends to `race_runs[]` while Core 0 (HTTP server) may read it simultaneously to compute the leaderboard.

**Worst case:** A stale leaderboard read, not data corruption (the array is fixed-size, not reallocated).

**Fix:** Wrap array access in a `portMUX` spinlock or FreeRTOS mutex, protecting both the append and leaderboard-read paths. This is **not blocking** and should only be done if stale/garbled reads are actually observed on real hardware.

---

## Unresolved Protocol Behaviors (from RATS V2)

Three areas from `src/net/serial-protocol.h`, `src/race-command-source.h`, and the RATS V2 spec have no defined behavior yet:

### `MSG_SET_MODE=99` / CALIBRATION Value

The legacy host protocol includes a `SetMode` command; one mode value is CALIBRATION. CERBERUS firmware runs on a display board with no local gate sensor hardware (gates are separate WiFi-connected devices), so a "calibration mode" for phototransistor/pot readings doesn't apply.

**Open question:** Should `MSG_SET_MODE=CALIBRATION` trigger any visible state change on the display (e.g. show a "Calibration Mode" banner)? Or is it ignored entirely?

### `MSG_EXTRA_RUN=92` Enforcement

The host can signal an extra run via `MSG_EXTRA_RUN`. Currently this is logged/echoed but does not actually increment `g_allowed_runs` or bypass any run-count limits.

**Open question:** Should extra runs be immediately granted, or should they increment a separate "extra runs granted" counter that the firmware can check?

### `MSG_ALLOWED_RUNS=94` Enforcement

The host sets the max run count per mouse via `MSG_ALLOWED_RUNS`. Currently stored in `g_allowed_runs` but never checked; `MAX_RUNS_PER_MOUSE` is still a fixed local constant (`src/race/race-timer.h`).

**Open question:** Should `g_allowed_runs` override the fixed constant, or is it informational metadata only?

---

## Host-Supplied Mouse Names

The RATS V2 protocol allows the host to send a mouse name in the `MSG_NEW_MOUSE=98` message (value field now contains text, not always `0`). This name reaches `SystemEvent.payload` (`src/race-command-source.h:280–289`) but is not currently threaded into the race state machine's run records.

**Current behavior:** `RaceRun` entries and leaderboard display still pick from the canned `mouse_names[]` array (`src/race/race-timer.h:29–68`), ignoring the host-supplied name.

**Open question:** Should the host-supplied name be stored in `RaceRun` and used on-screen and in the leaderboard instead of the canned list? If so, what UI fallback applies if a run completes without a host-supplied name?

---

## Touch Calibration NVS Escape Hatch

**Issue:** If NVS holds a `"calibrated"=true` entry with bad calibration data (e.g. leftover from earlier testing), `calibrate()` (`src/display/touch-calibration.h`) loads it and never re-launches the wizard. Since supervisor navigation is touch-driven, bad calibration locks the user out.

**Current workaround:** Full flash erase (`pio run -e <env> -t erase`).

**Possible fix:** A way to force re-calibration without working touch (e.g. hold a screen corner or other fixed physical action during boot, checked before `calibrate()` loads stored data in `app_setup()` / `main.cpp`).
