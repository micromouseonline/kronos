# ESP32 Timing System - Workspace Root

Multi-project workspace for a two-gate infrared timing system.

## Sub-projects

| Folder | Platform | Purpose |
|--------|----------|---------|
| `hesperus-timing-gate/` | ESP32-S3, PlatformIO | Gate firmware (WiFi TSF clock sync, WebSocket event reporting) |
| `cerberus-gate-controller/` | ESP32/ESP32-S3, PlatformIO | Central gate controller: touchscreen UI, NeoKey physical input, race state machine, serial (RATS V2) + WebSocket/HTTP event ingestion, leaderboard. See its own `docs/` (start at `SYSTEM-DESCRIPTION.md` and `OPERATOR-GUIDE.md`) |
| `ares-pulse-generator/` | ESP32-S3, PlatformIO | 1 Hz reference pulse generator for calibration |
| `hesperus-emitter/` | ESP32-S3-Zero, PlatformIO | Minimal bench stub: drives pins 2 and 3 low as outputs, no other logic |

Shared build/flashing/cataloguing utility scripts (not a sub-project) live
in `tools/` at the workspace root.

Outstanding work is tracked in `TODO.md` (root) — an index pointing at each
sub-project's own planning docs plus the cross-project (interoperability,
testing) items that don't belong to any single one.

## Off-limits directories

- `.pio/` in any sub-project: generated build artefacts and vendored libraries - do not read or modify
- `references/`: hardware datasheets and reference documents only - do not modify
- `docs/_resources/`: binary assets (images, ZIPs) - do not modify
- `references`: in the root, this just contains datasheets and similar files - do not read.

## Build commands

- Embedded: `pio run` (build), `pio run -t upload` (flash) - run inside the relevant sub-project folder

## Architecture notes

- Gates synchronise timing via Wi-Fi TSF (802.11 beacon timestamp)
- Each gate reports TRIGGER_A and TRIGGER_B events to cerberus over a persistent WebSocket connection (`/ws`); `POST /api/event` still serves as a secondary HTTP path
- Clock disciplining uses an EMA-weighted drift scaling factor (`clock_alpha`)
