# cerberus-gate-controller

Central gate controller: touchscreen UI, NeoKey physical input, race state
machine, serial (RATS V2) + WebSocket/HTTP event ingestion, leaderboard.

## Platform

- Board: ESP32/ESP32-S3 (dual-core), Cheap Yellow Display (CYD) - SPI TFT +
  XPT2046 touch, five shipping board environments (see `../PLATFORMIO.md`)
- Framework: Arduino + FreeRTOS + LVGL
- Build tool: PlatformIO (`pio run -e <env>`, `pio run -e <env> -t upload`)
- Serial monitor: 9600 baud (host link, RATS V2 protocol)

## Off-limits directories

- `firmware/.pio/`: generated build artefacts and vendored libraries - do
  not read or modify

## Hardware design files

- `hardware/pcb/`: KiCad project. `hardware/mechanical/`: enclosure/mounting
  CAD (native source + STEP/STL exports). Both are user-maintained via
  KiCad/CAD tools directly - do not hand-edit as text.

## Source layout

```
firmware/src/
  main.cpp       - entry point, FreeRTOS task setup
  boards/        - per-board pin/display definitions (board-select.h + one
                   header per shipping CYD variant)
  button/        - physical GPIO button driver
  display/       - LovyanGFX/LVGL display bridge, touch calibration
  html/          - static leaderboard page assets (clock.html, lb.html, css)
  neokey/        - I2C NeoKey keypad driver + pixel feedback
  net/           - WiFi manager, HTTP/WebSocket server, serial protocol,
                   gate event dedup/liveness
  race/          - race state machine, SystemEvent queue, serial telemetry
  status-led/    - onboard NeoPixel status LEDs
  ui/            - eez-studio-generated screens/actions/styles
```

## Key design points

- Core 0: WiFi station + async HTTP/WebSocket server (gate event ingestion
  via persistent `/ws`, `POST /api/event` as secondary path). Core 1: main
  app task (race state machine + display), input polling, serial driver.
- `SystemEvent` (fixed-size, no dynamic allocation) is the single event type
  crossing all producers (buttons, WS/HTTP, serial) into the main queue -
  see `firmware/src/race/system-event-queue.h`.
- Remote gates send `tsf_us` (WiFi TSF timestamp); CERBERUS accepts it at
  face value as the event's absolute time. Local events use
  `esp_timer_get_time()`.
- Serial link speaks a legacy bracket-CSV protocol (RATS V2,
  `preferredMessageSequencesV2.pdf`) to a host PC - see
  `firmware/src/net/serial-protocol.h`.
- Debug output always goes to the same UART as the RATS V2 protocol, `#`-
  prefixed so the host parser treats it as a comment line - see
  `firmware/src/debug-log.h`.

## Docs

Start at `docs/SYSTEM-DESCRIPTION.md`, then `docs/RACE-STATE-MACHINE.md`,
`docs/INPUT-SYSTEM.md`, `docs/OPERATOR-GUIDE.md`, `docs/PLANNED-UPDATES.md`.
