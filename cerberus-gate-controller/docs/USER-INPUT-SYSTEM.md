# Cerberus gate controller — input system

What the input layer provides, as built.

## Architecture

A Local Input Polling Task on Core 1 polls every input producer every
`INPUT_POLL_PERIOD_MS` and posts events into a shared FreeRTOS queue
(`input-events.h`). The main task drains that queue each `app_loop()` tick
and dispatches by `ButtonID` through `BUTTON_MENU[id].onPress()` ->
`on_button_event()` (`app-modes.h`). Producers are interchangeable: touch,
physical GPIO buttons, and NeoKey all post the same `ButtonID`/
`InputSource` pairs, so application logic never needs to know which
physical device generated a press.

## Input producers

**Touch** (`touch-buttons.h`) -- 4 of 5 boards. Capacitive (FT6336U,
CST820) or resistive (XPT2046), abstracted by LovyanGFX. Draws the
on-screen PREV / NEXT / ACTION / -- button bar (`BUTTON_MENU`,
`gui-button.h`) once at boot.

**Physical GPIO buttons** (`gpio-buttons.h`) -- M5 Core only (buttons
A/B/C). Active-low, debounced, with long-press support on the double-duty
button C.

**NeoKey 1x4** (`neokey-driver.h` / `neokey-buttons.h` / `neokey-pixels.h`)
-- optional external I2C attachment (Adafruit seesaw chip, address 0x30),
enabled by default on all 5 boards regardless of whether a physical module
is present:
- Init is fully non-blocking: a background FreeRTOS task does a quick
  presence probe, then the full handshake only if something acks. The rest
  of the app never waits on it.
- Runtime presence detection (`Neokey::isAvailable()`) -- an absent module
  degrades to silent no-ops (no errors, no hangs, no blocking of other
  input producers), both at boot and if unplugged while running.
- Debounce, long-press, double-press, and combo detection are all built
  into the `Neokey` class, though only press/release is currently wired
  into the event queue.
- 4 onboard NeoPixel LEDs, independently controllable
  (`neokey_set_colour()` / `neokey_set_all()`).

## Button identity and styling

- `ButtonID` (`BTN_ARM` / `BTN_START` / `BTN_GOAL` / `BTN_TOUCH`) is the
  single ID space all three producers post into.
- `BUTTON_MENU` (`gui-button.h`) is the canonical PREV/NEXT/ACTION/--
  labelling, shared by the touch bar and the dispatcher.
- `set_touch_button_style(ButtonID, ButtonColour)` (`touch-buttons.h`)
  redraws one on-screen button in place. GPIO buttons have no LEDs on any
  board, so there's no GPIO equivalent. NeoKey's 4 pixels are general
  indicators, not per-button feedback -- addressed directly by
  `neokey_set_colour()`/`neokey_set_all()` (`neokey-pixels.h`), not through
  `ButtonID`. (An earlier unified `set_button_style()` spanning all three
  producers was removed -- GPIO's leg was a permanent no-op and NeoKey's
  didn't fit the per-button framing.)

## App state (`app-modes.h`)

`AppState`: `SUPERVISOR` (mode-select menu, the boot default) /
`RECALIBRATE_TOUCH` / `PLACEHOLDER`. `on_button_event(ButtonID)` is the
single dispatch point every producer's press ultimately reaches, switched
on the current state. Unimplemented Supervisor entries share the generic
`PLACEHOLDER` state (`enter_placeholder()`) until they become real
sub-applications.

## Touch calibration (`touch-calibration.h`)

4-corner interactive wizard, NVS-persisted, reused across every touch
technology -- needed by resistive touch (raw ADC scaling) and by
capacitive touch (rotation offset between the panel and the touch chip's
native orientation) alike. Gated per board by `TOUCH_NEEDS_CALIBRATION`.
Available on demand via the Supervisor's "Recalibrate Touch" entry, which
suspends the input polling task for the duration.

## Per-board capability matrix

| Board | Touch | Calibration | Physical buttons | NeoKey SDA/SCL | NeoKey confirmed |
|---|---|---|---|---|---|
| M5 Core | none | n/a | A/B/C (GPIO) | 21/22 | absent-module only |
| Freenove S3 CYD | FT6336U (capacitive) | yes | none | 6/5 | present + absent |
| JC2432W328C | CST820 (capacitive) | yes | none | 21/22 | present + absent |
| CYD2USB (ILI9341) | XPT2046 (resistive) | yes | none | 27/22 | present + absent |
| CYD2USB (ST7789) | XPT2046 (resistive) | yes | none | 27/22 | present + absent |

Capability flags (`HAS_TOUCH_INPUT`, `HAS_GPIO_BUTTONS`,
`HAS_NEOKEY_BUTTONS`, `TOUCH_NEEDS_CALIBRATION`, `TOUCH_SHARES_DISPLAY_SPI_BUS`)
and NeoKey pins live in `lib/boards/*.h`, one file per board.
