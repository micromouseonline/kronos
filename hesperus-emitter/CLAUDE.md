# hesperus-emitter

PlatformIO firmware for the ESP32-S3-Zero emitter half of a Hesperus gate:
same enclosure as `hesperus-timing-gate`, but a much simpler board with no
WiFi/sensor logic of its own.

## Off-limits directories

- `firmware/.pio/`: generated build artefacts and vendored libraries - do not read or modify

## Hardware design files

- `hardware/pcb/`: KiCad project for this board's own emitter-side PCB.
  User-maintained via KiCad directly - do not hand-edit as text.
- The enclosure/mounting CAD is shared with `hesperus-timing-gate` (same
  physical gate housing, different PCB inside) and lives at the workspace
  root in `hardware-shared/mechanical/` (native source + STEP/STL exports),
  not under this project's own `hardware/`.

## Source layout

PlatformIO firmware lives under `firmware/` (`platformio.ini`, `boards.ini`,
`src/`, `include/`, `lib/`, `test/`, `dist/`) - run `pio run` from there.

```
firmware/src/
  main.cpp      - entry point: button-driven output state, deep sleep
```

## Key design points

- Drives two GPIO outputs (`PIN_A`/`PIN_B`, GPIO2/3) through one of four
  combinations, cycled by a physical button (`BTN_PIN`) and persisted
  across deep sleep in NVS (`hesp-emitter` namespace - kept ≤15 chars,
  the cap PlatformIO's NVS silently enforces)
- Spends almost all its time in deep sleep: `rtc_gpio_hold_en()` latches
  `PIN_A`/`PIN_B` at their last level through sleep (the pads would
  otherwise float once the digital domain powers down), and wakes only on
  `BTN_PIN` going low (`esp_sleep_enable_ext0_wakeup`)
- Held awake for `FLASH_WINDOW_MS` after a cold boot/reset (not a button
  wake) so the USB CDC link stays up long enough for esptool's normal
  reset-to-bootloader handshake - without it, `setup()` reaches
  `goToSleep()` before esptool ever gets a port to open, and the board
  becomes unflashable without forcing a POR with BOOT held
- Status NeoPixel (`STATUS_LED`/`NEOPIXEL_COLOR_ORDER`) comes from
  per-board `build_flags`, same convention as `hesperus-timing-gate`

## Libraries (managed by PlatformIO)

- Adafruit NeoPixel
- Preferences (ESP32 NVS wrapper)
