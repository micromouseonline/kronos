# ares-pulse-generator

## Off-limits directories

- `firmware/.pio/`: generated build artefacts and vendored libraries - do not read or modify

## Hardware design files

- `hardware/pcb/`: KiCad project. `hardware/mechanical/`: enclosure/mounting
  CAD (native source + STEP/STL exports). Both are user-maintained via
  KiCad/CAD tools directly - do not hand-edit as text.

## Source layout

PlatformIO firmware lives under `firmware/` (`platformio.ini`, `boards.ini`,
`src/`, `include/`, `lib/`, `test/`, `dist/`) - run `pio run` from there.

```
firmware/src/
  main.cpp            - entry point: wires up Cli + physical button, drives
                         TRG_ARM/TRG_START/TRG_GOAL output pins
  cli.h               - minimal serial line-reader/dispatcher (Cli class)
  pulser-commands.h   - trial definitions (TRIALS[]) and list/run/arm/status
                         serial commands
```

## Key design points

- Drives three active-low GPIO outputs (`TRG_ARM`/`TRG_START`/`TRG_GOAL`) to
  simulate photodiode/beam-break triggers into a real hesperus-timing-gate
  board under test
- Trigger pins are forced high in `setup()` before `Serial.begin()`, closing
  the false-trigger window where a floating post-reset pin could read as an
  asserted active-low input on the far end
- Trials are runtime-selectable over serial (`list`/`run`/`arm`/`status`) or
  fired by a physical button (`BTN_IN`) against whatever trial is currently
  armed
- Trial timing constants (`BURST_INTERVAL_MS`, `DOUBLE_TRIGGER_GAP_MS`, etc.)
  are deliberately tuned against hesperus's `DEBOUNCE_US` (50ms) to land
  above or below its debounce threshold on purpose - see `pulser-commands.h`
  comments
