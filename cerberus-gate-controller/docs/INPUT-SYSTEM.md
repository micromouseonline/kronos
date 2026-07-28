# Cerberus gate controller — input system

Race commands (ARM, START, GOAL, new mouse) are driven by physical
buttons — today, exclusively the NeoKey 1x4 I2C keypad, since none of the
four supported boards enable the (still-present) GPIO-button code path.
Touch does **not** drive race commands: the on-screen ARM/START/GOAL
buttons were removed in favour of physical NeoKey input, and touch's only
remaining role is generic screen navigation (menu, settings, WiFi setup)
and tapping the WiFi-status panel.

All local input sources (currently NeoKey, plus the dormant GPIO-button
path) place event notifications into a shared queue, drained by one common
handler (see Architecture below).

Remote operation by serial messages or HTTP POST requests is implemented
-- see `docs/SYSTEM-DESCRIPTION.md`'s "5. Serial Protocol & Legacy Host
Interop" and "3. HTTP Server" sections. It does **not** share the
local-input queue below: Serial (`net/serial-protocol.h`) and HTTP
(`net/http-server.h`) both post into a separate Main Event Queue
(`SystemEvent`, `race/system-event-queue.h`), since they carry payload
types (mouse name, `gate_id`, remote timestamp) the local queue's
`ButtonID`/`InputSource` pair can't hold. Both queues converge on the same
`race_timer_handle_command()` entry point -- the state machine never needs
to know which queue, or which producer, an event came from.

## Architecture

A Local Input Polling Task on Core 1 (`input_poll_task`, `main.cpp`) polls
GPIO buttons and NeoKey every `INPUT_POLL_PERIOD_MS` (15ms, `config.h`) and
posts events into a shared FreeRTOS queue (`input-events.h`). Touch is
**not** on this task, is not a race-command producer, and never posts into
this queue (`InputSource::TOUCH` is still declared in the `InputSource`
enum but nothing produces it) -- it's polled internally by LVGL's own
input device (`lvgl_touch_init()`, `display/lvgl-bridge.h`/`.cpp`) purely
for screen navigation.

`loop()` (`main.cpp`) drains the queue every iteration via
`input_queue_drain(input_event_handler)`. The handler (`main.cpp`) maps
`ButtonID`/`InputEventType` to a `RaceCommand` via
`race_command_from_button()` (`race/race-command-source.h`) and calls
`race_timer_handle_command()` (`race/race-timer.h`) -- see "App state"
below. It also drives NeoKey LED feedback from the current `RaceState`
(`neokey_reflect_race_state()`, `main.cpp:75-125`), and handles a held
NeoKey TOUCH key as a UI-only "return to menu" navigation, independent of
the race state machine.

Producers are interchangeable: any local producer posts the same
`ButtonID`/`InputSource` pairs, so this handler never needs to know which
physical device generated a press. That held true when touch was also a
producer and remains true now that NeoKey is the only active one.

### Button-activity mapping

What a press or hold actually *means* (which `RaceCommand` it produces) is
centralized in one table, `BUTTON_COMMAND_MAP` (`race/
race-command-source.h`) -- one `{on_press, on_hold}` row per `ButtonID`.
Every producer posts through this table automatically once it's reduced a
physical event to a `ButtonID`/`InputEventType` pair, so changing what any
button's press or hold does is a one-line edit there, not a hunt through
each producer.

```c++
constexpr ButtonCommandMap BUTTON_COMMAND_MAP[NUM_BUTTONS] = {
    /* BTN_ARM   */ {RaceCommand::ARM, RaceCommand::RESTART},
    /* BTN_START */ {RaceCommand::START, RaceCommand::NONE},
    /* BTN_GOAL  */ {RaceCommand::GOAL, RaceCommand::NONE},
    /* BTN_TOUCH */ {RaceCommand::NEW_MOUSE, RaceCommand::NONE},
};
```

## Input producers

**NeoKey 1x4** (`neokey-driver.h` / `neokey-buttons.h` / `neokey-pixels.h`,
all at `src/`, not under a `neokey/` subfolder) -- optional external I2C
attachment (Adafruit seesaw chip, address 0x30), enabled by default on all
4 boards regardless of whether a physical module is present. **This is the
only active local producer of ARM/START/GOAL/TOUCH race commands today.**
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
  `main.cpp`'s `input_event_handler` (not through the table -- a UI-only
  action, same idea as the old touch-panel long-press before it was
  removed).
- 4 onboard NeoPixel LEDs, independently controllable (`neokey_set_colour()`
  / `neokey_set_colours()` / `neokey_set_all()`, `neokey-pixels.h`).
  `neokey_set_colours()` (plural, takes a `KeyColours` struct) is what
  drives the per-`RaceState` LED feedback in `main.cpp`. Key 3 (BTN_TOUCH)
  is normally reserved for the WiFi-status indicator
  (`net/wifi-manager.h`'s `WIFI_STATUS_KEY`) rather than race-state
  feedback, since none of the target boards has a working onboard status
  LED. See `docs/OPERATOR-GUIDE.md` for what each LED colour means during
  a race.

**Physical GPIO buttons** (`gpio-buttons.h`) -- present in code, currently
unused: no board enables `HAS_GPIO_BUTTONS`.
- Buttons A/B/C, active-low, debounced (`DebouncedButton`, `button/
  button.h`).
- All three support press + hold (`GPIO_BUTTON_LONG_PRESS_MS`): PRESSED
  fires on the press edge, HELD fires once mid-hold if still down past
  the threshold -- the same dual-post pattern as NeoKey.
- Maps through `BUTTON_COMMAND_MAP` like every other producer.
- On a board with no touchscreen, `BTN_TOUCH` would never be produced here.

**Touch** -- all 4 boards. Capacitive (FT6336U, CST820) or resistive
(XPT2046), abstracted by LovyanGFX. **Not a race-command producer.** Its
role today is limited to:
- On-screen navigation between the menu, main timer, settings, and
  WiFi-setup screens, via EEZ Studio-generated LVGL widgets (`ui/
  screens.c`) and their callbacks in `eez-actions.cpp`
  (`action_on_menu_*`, `action_on_settings_*`, `action_on_wifi_setup_*`).
- A tap on the WiFi-status panel on the main screen
  (`action_on_menu_setup`, wired to `pnl_status_wifi` in
  `create_screen_main()`).

An earlier design routed ARM/START/GOAL/TOUCH through on-screen buttons
(`action_on_timer_arm/start/goal/touch` in `eez-actions.cpp`, wired from
`ui/screens.c`'s main timer screen). That UI was removed when the status
bar replaced it; the four `action_on_timer_*` function bodies are gone,
though `ui/actions.h` still carries their now-unused `extern`
declarations. If you find yourself looking for on-screen race-command
buttons, they no longer exist -- use the physical NeoKey keypad.

A 250ms touch lockout (`trigger_touch_lockout()`,
`display/lvgl-bridge.cpp`) debounces touch input immediately after a
screen change.

## Remote producers (Serial & HTTP)

Serial and HTTP were originally sketched as a future `InputSource::
WIFI_MESSAGE` value on the local input queue; the actual implementation
went a different route instead (`input-events.h`'s `InputSource` enum has
no such placeholder). Both post into the separate `SystemEvent` queue and
converge on `race_timer_handle_command()` alongside local button input:

- **Serial** (`net/serial-protocol.h`, `race/race-command-source.h`) --
  `race_command_from_serial()` and `serial_protocol_handle_info_message()`
  parse the legacy bracket-CSV RATS V2 protocol (`<type,value>`) and post
  `RaceCommand`s such as `NEW_MOUSE` (from `MSG_NEW_MOUSE`),
  `ENTER_CALIBRATION`/`RESUME_TIMER` (from `MSG_SET_MODE`), and
  `EXTRA_RUN` (from `MSG_EXTRA_RUN`).
- **HTTP** (`net/http-server.h`, `race/race-command-source.h`) --
  `POST /api/event` events are mapped through `HTTP_EVENT_COMMAND_MAP` via
  `race_command_from_http()` into the same `RaceCommand` set.

## App state

There is no `AppState`/Supervisor menu system. Race progress is tracked by
two enums in `race/race-timer.h`:
- `RaceState`: `CALIBRATE` / `NEW_MOUSE` / `WAITING` / `ARMED` / `RUNNING`
  / `GOAL` / `TIMED_OUT`.
- `RaceCommand`: `NONE` / `NEW_MOUSE` / `ARM` / `START` / `GOAL` /
  `RESTART` / `ENTER_CALIBRATION` / `RESUME_TIMER` / `EXTRA_RUN`.

`race_timer_handle_command()` advances `RaceState` in response to each
`RaceCommand`, from whichever producer it came from. Screen navigation
(menu vs. main timer screen) is a separate concern, handled by EEZ
Studio's `loadScreen()` (`ui/screens.c`), decoupled from race state. A
250ms touch lockout (`trigger_touch_lockout()`, `display/lvgl-bridge.cpp`)
debounces touch input immediately after a screen change.

## Touch calibration (`display/touch-calibration.h`)

4-corner interactive wizard, NVS-persisted, reused across every touch
technology -- needed by resistive touch (raw ADC scaling) and by
capacitive touch (rotation offset between the panel and the touch chip's
native orientation) alike. Gated per board by `TOUCH_NEEDS_CALIBRATION`.
Since touch now drives only menu/settings navigation (not race commands),
a bad calibration locks the user out of the menu, not out of racing.

Currently only runs automatically at boot (`setup()`, before
`input_poll_task` is created -- boot-time ordering avoids racing the
polling task against `lcd.getTouch()`, rather than any runtime suspend).
The menu has a "Recalibrate" entry (`action_on_menu_calibrate`,
`eez-actions.cpp`) but it is currently a stub that only logs to serial --
it does not yet call into the calibration wizard. On-demand recalibration
is a known gap (see `docs/PLANNED-UPDATES.md`).

## Per-board capability matrix

| Board | Touch | Calibration | Physical buttons | NeoKey SDA/SCL | NeoKey confirmed |
|---|---|---|---|---|---|
| Freenove S3 CYD | FT6336U (capacitive) | yes | none | 6/5 | present + absent |
| JC2432W328C | CST820 (capacitive) | yes | none | 21/22 | present + absent |
| CYD2USB (ILI9341) | XPT2046 (resistive) | yes | none | 27/22 | present + absent |
| CYD2USB (ST7789) | XPT2046 (resistive) | yes | none | 27/22 | present + absent |

Capability flags (`HAS_TOUCH_INPUT`, `HAS_GPIO_BUTTONS`,
`HAS_NEOKEY_BUTTONS`, `TOUCH_NEEDS_CALIBRATION`, `TOUCH_SHARES_DISPLAY_SPI_BUS`)
and NeoKey pins live in `boards/*.h`, one file per board.
