# ESP32 Timing System - Workspace Root

Multi-project workspace for a two-gate infrared timing system.

## Sub-projects

| Folder | Platform | Purpose |
|--------|----------|---------|
| `hesperus-timing-gate/` | ESP32-S3, PlatformIO | Gate firmware (WiFi TSF clock sync, HTTP event reporting) |
| `cerberus-gate-controller/` | ESP32/ESP32-S3, PlatformIO | Central gate controller: touchscreen UI, NeoKey physical input, race state machine, serial (RATS V2) + HTTP event ingestion, leaderboard. See its own `docs/` (start at `SYSTEM-DESCRIPTION.md` and `OPERATOR-GUIDE.md`) |
| `ares-pulse-generator/` | ESP32-S3, PlatformIO | 1 Hz reference pulse generator for calibration |
| `gate-controller-python-test-cerberus/` | Python 3 | HTTP server: receives gate events, matches pairs, logs forensics |

Shared build/flashing/cataloguing utility scripts (not a sub-project) live
in `tools/` at the workspace root.

## Off-limits directories

- `.pio/` in any sub-project: generated build artefacts and vendored libraries - do not read or modify
- `references/`: hardware datasheets and reference documents only - do not modify
- `_resources/`: binary assets (images, ZIPs) - do not modify
- `references`: in the root, this just contains datasheets and similar files - do not read.

## Build commands

- Embedded: `pio run` (build), `pio run -t upload` (flash) - run inside the relevant sub-project folder
- Python server: `python server.py` inside `gate-controller-python-test-cerberus/`

## Architecture notes

- Gates synchronise timing via Wi-Fi TSF (802.11 beacon timestamp)
- Each gate reports TRIGGER_A, TRIGGER_B, and HEARTBEAT events over HTTP
- The Python server matches paired triggers within a configurable threshold (default 200 ms)
- Clock disciplining uses an EMA-weighted drift scaling factor (`clock_alpha`)
