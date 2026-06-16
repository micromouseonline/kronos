# ESP32 Timing System - Workspace Root

Multi-project workspace for a two-gate infrared timing system.

## Sub-projects

| Folder | Platform | Purpose |
|--------|----------|---------|
| `ESP32-timing-gates-hesperus/` | ESP32-S3, PlatformIO | Gate firmware (WiFi TSF clock sync, HTTP event reporting) |
| `event-pulse-generator-arduino-nano/` | Arduino Nano, PlatformIO | 1 Hz reference pulse generator for calibration |
| `gate-controller-python-test-cerberus/` | Python 3 | HTTP server: receives gate events, matches pairs, logs forensics |

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
