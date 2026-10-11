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
(`firmware/src/display/touch-calibration.h`) used to load it and never re-launch
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

---

## Open interface questions for RATS

RATS was written independently and predates this project, and its V2 spec
(`preferredMessageSequencesV2.pdf`) defines messages, not which side owns
each responsibility. Until the questions below are answered, CERBERUS
treats run and entry-time limits as advisory -- see "Run and entry-time
limits are advisory" in `RACE-STATE-MACHINE.md`.

**The dual-role problem.** Attached to RATS, CERBERUS is a timing
instrument and should hold no policy. Standalone (local buttons, its own
leaderboard page) it is the only place policy could live. The protocol
gives it no way to tell which role it is in.

**Questions for the RATS author:**

1. **Who owns the run count?** RATS increments its own count when the
   gate reports State 4 and checks it against the runs allowed (spec 3.3).
   The gate may also count ("not required to", spec 4), and `ExtraRun`
   asks the gate to decrement its count. No message carries the count in
   either direction, so the two can drift. Which is authoritative, and
   should the gate count at all?
2. **Who owns the entry time?** RATS "implements the entry time
   constraints" (spec 5, `CourseTimeMs`), but the gate starts that clock
   with `<30,0>` and gate values take precedence over RATS's own (spec
   3.2). Who declares that time has expired, and should the gate react?
3. **Practice mode.** RATS can let robots cycle freely with no run or
   time exhaustion, but only RATS knows it is doing so: `SetMode`
   (`<99,...>`) accepts only `TIMER` and `CALIBRATION`. Does RATS send
   anything different in practice mode (no `AllowedRuns`, a large value)?
   Could `SetMode` gain a `PRACTICE` value?
4. **Outcomes never come back.** No message tells the gate that a run was
   disallowed, a touch penalty applied, or an entry ended. Should one
   exist?
5. **Spec looseness.** Section 2 says the value is an integer, while
   Annex A carries names and keywords (`NewMouse`, `ContestName`,
   `EventName`, `SetMode`). Annex A's descriptions for 95 and 96 are
   misaligned (96 `ContestName` carries the event-name text).

**Known consequence today.** The CERBERUS leaderboard takes each mouse's
best time over every run CERBERUS timed
(`race_timer_compute_leaderboard()`, `firmware/src/race/race-timer.h`), so
it can disagree with RATS's scores: runs RATS disallowed, touch penalties
and practice runs are all invisible to it.

**Possible direction, not decided.** Distinguish "RATS attached" from
"standalone", and apply local run/time rules only when standalone.
