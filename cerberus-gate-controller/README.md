# cerberus-gate-controller

ESP32 gate-timer controller with a touchscreen UI. Race commands (ARM,
START, GOAL, new mouse) are driven by a physical NeoKey 1x4 keypad, or
remotely over serial/HTTP; touch drives on-screen navigation only, not
race commands. Shared input system across four board targets. There is
no Supervisor/menu state machine -- race progress is tracked entirely by
`RaceState`, decoupled from screen navigation.

See `docs/OPERATOR-GUIDE.md` for how to run a race day, and
`docs/SYSTEM-DESCRIPTION.md` for the full architecture.

## What it illustrates

- **Producer-agnostic input dispatch** -- physical NeoKey buttons (and,
  in code, an unused GPIO-button path) all post the same `ButtonID`
  events into one FreeRTOS queue from a Core-1 polling task. The main task
  drains the queue and dispatches through a single handler, so
  application logic never knows which physical device generated a press.
  See `docs/INPUT-SYSTEM.md` for the full design.
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
| cerberus-cyd2usb-diymalls-ili9341        | CYD2USB (DIYMalls, ILI9341 panel)   |
| cerberus-cyd2usb-diymalls-st7789         | CYD2USB (DIYMalls, ST7789 panel)    |
| cerberus-jc2432w328c                     | JC2432W328C                         |

## Build

From the command line, something like:

```
pio run -e cerberus-esp32-s3-cyd-touch-freenove
pio run -e cerberus-esp32-s3-cyd-touch-freenove -t upload
```

See the [workspace build guide](../PLATFORMIO.md) for details on targeting
different boards, and `docs/INPUT-SYSTEM.md` for how the input/dispatch
layer works.
