# cerberus-gate-controller

ESP32 gate-timer controller with a touchscreen UI. Currently implements a
Supervisor menu and input system (touch / GPIO / NeoKey 1x4) shared across
five board targets; the race-timing logic itself (see
`docs/DESIGN-REQUIREMENT.md`) is not yet implemented.

## What it illustrates

- **Producer-agnostic input dispatch** -- touch, physical GPIO buttons (M5
  Core), and an optional NeoKey 1x4 I2C keypad all post the same `ButtonID`
  events into one FreeRTOS queue from a Core-1 polling task. The main task
  drains the queue and dispatches through a single `on_button_event()`, so
  application logic never knows which physical device generated a press.
  See `docs/USER-INPUT-SYSTEM.md` for the full design.
- **Runtime hardware presence detection** -- the NeoKey module is optional
  on every board; a background task probes for it non-blockingly and the
  rest of the app degrades to silent no-ops if it's absent or unplugged.
- **Multi-target board config** -- `platformio.ini` extends shared base
  environments from `boards.ini`; board-specific pins and display
  driver selection live in `lib/boards/`.
- **Runtime touch calibration** -- resistive-touch boards (XPT2046) persist
  a calibration to NVS and fall back to an interactive wizard if none is
  stored (`touch-calibration.h`).

## Targets

| PlatformIO env                          | Board                              |
|------------------------------------------|-------------------------------------|
| cerberus-esp32-s3-cyd-touch-freenove     | Freenove FNK0104B ESP32-S3 CYD      |
| cerberus-m5-core                         | M5Stack Core                        |
| cerberus-cyd2usb-diymalls-ili9341        | CYD2USB (DIYMalls, ILI9341 panel)   |
| cerberus-cyd2usb-diymalls-st7789         | CYD2USB (DIYMalls, ST7789 panel)    |
| cerberus-jc2432w328c                     | JC2432W328C                         |

## Build

```
pio run -e cerberus-esp32-s3-cyd-touch-freenove
pio run -e cerberus-esp32-s3-cyd-touch-freenove -t upload
```

See the [workspace build guide](../BUILDING.md) for details on targeting
different boards, and `docs/USER-INPUT-SYSTEM.md` for how the input/dispatch
layer works.
