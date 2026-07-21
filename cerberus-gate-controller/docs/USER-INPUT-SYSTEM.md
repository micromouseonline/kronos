# Cerberus gate controller — user input system

The input layer lets the user interact directly with the system via touch
screen and/or physical buttons. If present, these must provide at least
three inputs: ARM, START and GOAL.

All local input sources (touch, GPIO buttons, NeoKey) place event
notifications into a shared queue, drained by one common handler (see
Architecture below).

Remote operation by serial messages or HTTP POST requests is implemented
-- see `docs/DESIGN-REQUIREMENT.md`'s Serial Monitor Task / Asynchronous
HTTP Listener and `docs/IMPLEMENTATION-PLAN.md`'s Stages B-D (serial) and
F-H (HTTP). It does **not** share the local-input queue below: Serial
(`net/serial-protocol.h`) and HTTP (`net/http-server.h`) both post into a
separate Main Event Queue (`SystemEvent`, `race/system-event-queue.h`),
since they carry payload types (mouse name, `gate_id`, remote timestamp)
the local queue's `ButtonID`/`InputSource` pair can't hold. Both queues
converge on the same `race_timer_handle_command()` entry point -- the
state machine never needs to know which queue, or which producer, an
event came from.

## Architecture

A Local Input Polling Task on Core 1 (`input_poll_task`, `main.cpp`) polls
GPIO buttons and NeoKey every `INPUT_POLL_PERIOD_MS` (15ms, `config.h`) and
posts events into a shared FreeRTOS queue (`input-events.h`). Touch is
**not** on this task -- it's polled internally by LVGL's own input device
(`lvgl_touch_init()`, `display/lvgl-bridge.h`/`.cpp`) at its own read
period, and posts into the same queue from its LVGL event callbacks
(`eez-actions.cpp`).

`loop()` (`main.cpp`) drains the queue every iteration via
`input_queue_drain(input_event_handler)`. The handler (`main.cpp`) maps
`ButtonID`/`InputEventType` to a `RaceCommand` via
`race_command_from_button()` (`race/race-command-source.h`) and calls
`race_timer_handle_command()` (`race/race-timer.h`) -- see "App state"
below. It also drives NeoKey LED feedback from the current `RaceState`,
and handles a held TOUCH button as a UI-only "return to menu" navigation,
independent of the race state machine.

Producers are interchangeable: touch, physical GPIO buttons, and NeoKey
all post the same `ButtonID`/`InputSource` pairs, so this handler never
needs to know which physical device generated a press.

### Button-activity mapping

What a press or hold actually *means* (which `RaceCommand` it produces) is
centralized in one table, `BUTTON_COMMAND_MAP` (`race/
race-command-source.h`) -- one `{on_press, on_hold}` row per `ButtonID`.
Every producer posts through this table automatically once it's reduced a
physical event to a `ButtonID`/`InputEventType` pair, so changing what any
button's press or hold does is a one-line edit there, not a hunt through
each producer.

Touch's *widget-to-`ButtonID`* assignment -- which on-screen button is
ARM/START/GOAL/TOUCH -- is a separate concern, owned by EEZ Studio's
screen design + `eez-actions.cpp`'s callbacks (see "Input producers"
below).

## Input producers

**Touch** -- 4 of 5 boards. Capacitive (FT6336U, CST820) or resistive
(XPT2046), abstracted by LovyanGFX.
- On-screen buttons are EEZ Studio-generated LVGL screens (`ui/
  screens.c`); each posts via a hand-written callback in `eez-actions.cpp`
  (`action_on_timer_arm/start/goal/touch`). This widget-to-`ButtonID`
  wiring is EEZ Studio's job, not `BUTTON_COMMAND_MAP`'s -- `screens.c` is
  regenerated wholesale on every export, so button *identity* lives
  there/`eez-actions.cpp`, while what a resulting press *does* still comes
  from the shared table.
- All four on-screen buttons also have a long-press binding (LVGL's
  native `LV_EVENT_LONG_PRESSED`, threshold set via `indev->driver->
  long_press_time` in `main.cpp`'s `setup()`), each posting the same
  `ButtonID` + `HELD` into the input queue as every other producer
  (`action_on_timer_arm_long` / `_start_long` / `_goal_long` /
  `_touch_long`, `eez-actions.cpp`).
- TOUCH-held additionally triggers "return to menu" via a special case in
  `main.cpp`'s `input_event_handler` -- same as a held NeoKey TOUCH
  button, not a separate code path.

**Physical GPIO buttons** (`gpio-buttons.h`) -- M5 Core only.
- Buttons A/B/C, active-low, debounced (`DebouncedButton`, `button/
  button.h`).
- All three support press + hold (`GPIO_BUTTON_LONG_PRESS_MS`, `boards/
  m5-core.h`): PRESSED fires on the press edge, HELD fires once mid-hold
  if still down past the threshold -- the same dual-post pattern as
  NeoKey and touch.
- Both map through `BUTTON_COMMAND_MAP` like every other producer.
- This board has no touchscreen, so `BTN_TOUCH` is never produced here.

**NeoKey 1x4** (`neokey/neokey-driver.h` / `neokey-buttons.h` /
`neokey/neokey-pixels.h`) -- optional external I2C attachment (Adafruit
seesaw chip, address 0x30), enabled by default on all 5 boards regardless
of whether a physical module is present.
- Init is fully non-blocking: a background FreeRTOS task does a quick
  presence probe, then the full handshake only if something acks. The rest
  of the app never waits on it.
- Runtime presence detection (`Neokey::isAvailable()`) -- an absent module
  degrades to silent no-ops (no errors, no hangs, no blocking of other
  input producers), both at boot and if unplugged while running.
- Debounce, long-press, double-press, and combo detection are all built
  into the `Neokey` class, but only press and hold (`InputEventType::
  PRESSED` / `HELD` -- there is no release event type) are currently
  wired into the event queue. Double-press and combo detection exist but
  have no call sites yet.
- Held buttons map through `BUTTON_COMMAND_MAP` like every other producer
  (ARM-held -> `RaceCommand::RESTART`); a held NeoKey TOUCH button
  additionally returns to the menu screen via a special case in
  `main.cpp`'s `input_event_handler` (not through the table -- same
  UI-only action as the touch panel's own long-press).
- 4 onboard NeoPixel LEDs, independently controllable (`neokey_set_colour()`
  / `neokey_set_colours()` / `neokey_set_all()`, `neokey/neokey-pixels.h`).
  `neokey_set_colours()` (plural, takes a `KeyColours` struct) is what
  drives the per-`RaceState` LED feedback in `main.cpp`.

Serial/HTTP were originally sketched as a future `InputSource::WIFI_MESSAGE`
value on this same queue; the actual implementation (Stages B-D, F-H) went
a different route instead -- see the intro above and "Remote producers"
note there. `input-events.h`'s `InputSource` enum has no such placeholder
any more.

## App state

There is no `AppState`/Supervisor menu system. Race progress is tracked by
two enums in `race/race-timer.h`:
- `RaceState`: `CALIBRATE` / `NEW_MOUSE` / `WAITING` / `ARMED` / `RUNNING`
  / `GOAL` / `TIMED_OUT`.
- `RaceCommand`: `NONE` / `NEW_MOUSE` / `ARM` / `START` / `GOAL` /
  `RESTART`.

`race_timer_handle_command()` advances `RaceState` in response to each
`RaceCommand` produced by `race_command_from_button()`. Screen navigation
(menu vs. main timer screen) is a separate concern, handled by EEZ
Studio's `loadScreen()` (`ui/screens.c`), decoupled from race state. A
250ms touch lockout (`trigger_touch_lockout()`, `display/lvgl-bridge.cpp`)
debounces touch input immediately after a screen change.

## Touch calibration (`display/touch-calibration.h`)

4-corner interactive wizard, NVS-persisted, reused across every touch
technology -- needed by resistive touch (raw ADC scaling) and by
capacitive touch (rotation offset between the panel and the touch chip's
native orientation) alike. Gated per board by `TOUCH_NEEDS_CALIBRATION`.

Currently only runs automatically at boot (`setup()`, before
`input_poll_task` is created -- boot-time ordering avoids racing the
polling task against `lcd.getTouch()`, rather than any runtime suspend).
The menu has a "Recalibrate" entry (`action_on_menu_calibrate`,
`eez-actions.cpp`) but it is currently a stub that only logs to serial --
it does not yet call into the calibration wizard. On-demand recalibration
is a known gap.

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
and NeoKey pins live in `boards/*.h`, one file per board.
