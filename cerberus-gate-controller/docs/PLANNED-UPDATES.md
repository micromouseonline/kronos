# CERBERUS: Planned Updates & Future Work

This document tracks architecture and feature work deferred from the current implementation.

---

## Supervisor State Machine (READY/RACING/MAINTENANCE)

Original design envisioned a three-level supervisory state machine distinct from the per-mouse race state machine. Current implementation is simpler: the race state machine alone governs timing flow, and screen navigation is decoupled via EEZ Studio's `loadScreen()` (see `INPUT-SYSTEM.md`'s "App state" section).

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

## `race_runs[]` Concurrency Guard (Optional)

Not yet implemented. Currently, Core 1 (state machine) appends to `race_runs[]` while Core 0 (HTTP server) may read it simultaneously to compute the leaderboard.

**Worst case:** A stale leaderboard read, not data corruption (the array is fixed-size, not reallocated).

**Fix:** Wrap array access in a `portMUX` spinlock or FreeRTOS mutex, protecting both the append and leaderboard-read paths. This is **not blocking** and should only be done if stale/garbled reads are actually observed on real hardware.

---

## Touch Calibration NVS Escape Hatch

**Resolved, 2026-08-06.** If NVS holds a `"calibrated"=true` entry with bad
calibration data (e.g. leftover from earlier testing), `calibrate()`
(`src/display/touch-calibration.h`) used to load it and never re-launch
the wizard — since menu/settings navigation is touch-driven, bad
calibration locked the user out of the menu (race commands via NeoKey were
unaffected).

**Fix:** long-press START (key 1) while on the main menu screen now calls
`re_calibrate(lcd)` directly (`main.cpp`'s `input_event_handler`) — a
physical NeoKey gesture, not an on-screen button, since the whole point is
to still work when touch itself is miscalibrated. START's long-press was
otherwise unused (`BUTTON_COMMAND_MAP` maps it to `RaceCommand::NONE`), and
gating it to the menu screen keeps it from ever competing with its
in-race meaning. Chosen over a full flash erase (still works as a
fallback) and over reusing the on-screen "unused" main-menu button slot,
which would have made the escape hatch depend on the very thing it's meant
to recover from.
