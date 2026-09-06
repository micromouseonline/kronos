# ESP32 Timing System - Workspace Root

Multi-project workspace for a two-gate infrared timing system.

## Sub-projects

| Folder | Platform | Purpose |
|--------|----------|---------|
| `hesperus-timing-gate/` | ESP32-S3, PlatformIO | Gate firmware (WiFi TSF clock sync, WebSocket event reporting) |
| `cerberus-gate-controller/` | ESP32/ESP32-S3, PlatformIO | Central gate controller: touchscreen UI, NeoKey physical input, race state machine, serial (RATS V2) + WebSocket/HTTP event ingestion, leaderboard. See its own `docs/` (start at `SYSTEM-DESCRIPTION.md` and `OPERATOR-GUIDE.md`) |
| `ares-pulse-generator/` | ESP32-S3, PlatformIO | 1 Hz reference pulse generator for calibration |
| `hesperus-emitter/` | ESP32-S3-Zero, PlatformIO | Button-driven, deep-sleeping bench emitter; shares its enclosure design with `hesperus-timing-gate` (see `hardware-shared/` below) but carries a much simpler board |

`legacy/` (root) is a pre-ESP32 Arduino Uno gate-detector prototype and its
KiCad project — kept for reference (see
`legacy/gate-detector/legacy-evaluation.md`), not part of the active system.

Shared build/flashing/cataloguing utility scripts (not a sub-project) live
in `tools/` at the workspace root, including `esp32_inventory.csv` (hardware
asset inventory) and `tools/esp-catalogue.py` (its cataloguing tool, notes
in `tools/esp-catalogue-reliability.md`).

Each sub-project keeps its PlatformIO firmware (`src/`, `include/`, `lib/`,
`test/`, `platformio.ini`, `boards.ini`, `dist/`) in its own `firmware/`
subfolder, so that `README.md`/`CLAUDE.md`/`docs/`/`hardware/` at the
sub-project root aren't mixed in with firmware source.

Each sub-project may contain its own `hardware/` folder for that target's
PCB design (`hardware/pcb/`, a KiCad project) and, where the mechanical
design isn't shared with another sub-project, mechanical CAD
(`hardware/mechanical/`, enclosures/mounts as native CAD source plus
STEP/STL exports). These are user-maintained design files edited via
KiCad/CAD tools directly, not something to hand-edit as text.

`hesperus-emitter` and `hesperus-timing-gate` are the two halves of the
same physical gate (same enclosure/mount, different PCB inside), so their
shared mechanical design lives once at the workspace root in
`hardware-shared/mechanical/` (not a sub-project, same pattern as
`tools/` above) instead of being duplicated under each project's own
`hardware/`. Each of those two projects keeps only `hardware/pcb/`
locally for its own distinct board.

Outstanding work is tracked in `TODO.md` (root) — an index pointing at each
sub-project's own planning docs plus the cross-project (interoperability,
testing) items that don't belong to any single one.

## Off-limits directories

- `.pio/` in any sub-project's `firmware/` folder: generated build artefacts and vendored libraries - do not read or modify
- `references/`: hardware datasheets and reference documents only - do not modify
- `docs/_resources/`: binary assets (images, ZIPs) - do not modify
- `references`: in the root, this just contains datasheets and similar files - do not read.

## Build commands

- Embedded: `pio run` (build), `pio run -t upload` (flash) - run inside the relevant sub-project's `firmware/` folder

## Architecture notes

- Gates synchronise timing via Wi-Fi TSF (802.11 beacon timestamp)
- Each gate reports TRIGGER_A and TRIGGER_B events to cerberus over a persistent WebSocket connection (`/ws`); `POST /api/event` still serves as a secondary HTTP path
- Clock disciplining uses an EMA-weighted drift scaling factor (`clock_alpha`)
