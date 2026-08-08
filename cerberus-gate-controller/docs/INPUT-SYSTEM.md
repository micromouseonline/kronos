
# Cerberus Gate Controller — Physical Input System Architecture

## Overview & Executive Summary

Race commands (**ARM**, **START**, **GOAL**, **NEW MOUSE**) are driven exclusively by physical hardware. Today, the **Adafruit NeoKey 1x4 I2C keypad** is the sole active local source, consisting of four buttons marked

 - 'A' - ARM
 - 'S' - START
 - 'G' - GOAL
 - 'T' - TOUCH

Each button, when pressed drives a race command, setting the race state, controlling the timers and notifying the RATS host.
Touch input does **not** drive race commands, it simply notifies the RATS host of a touch. 

Long presses on the buttons perform special actions. At the time of writing, only two such actions exist:

 - 'A' - long press starts a new mouse
 - 'T' - long press switches from the race display to the main menu

Physical GPIO inputs can also be used to drive the ARM, START and GOAL commands but this is not currently implemented

```
                 ┌──────────────────────┐
                 │   NeoKey 1x4 (I2C)   │
                 └──────────┬───────────┘
                            │ (Polls every 15ms on Core 1)
                            ▼
┌─────────────────┐  ┌──────────────┐
│  GPIO Buttons   ├──►  Local Queue │
│ (Dormant path)  │  │(ButtonID/Type│
└─────────────────┘  └──────┬───────┘
                            │
                            │ (Drained via loop())
                            ▼
┌────────────────────┐  ┌──────────────┐   BUTTON_COMMAND_MAP  ┌────────────────────────────┐
│ Serial Messages    ├──► Main System  ├───────────────────────► race_timer_handle_command()│
├────────────────────┤  │    Queue     │                       │   (Race State Machine)     │
│ WebSocket /ws      ├──►              │                       └────────────────────────────┘
│ (primary, gates)   │  │(SystemEvent) │
├────────────────────┤  │              │
│ HTTP POST /api/    ├──►              │
│ event (secondary)  │  └──────────────┘
└────────────────────┘
```

---

## 1. Local vs. Remote Event Architecture

Local and remote inputs both drive the central race state machine (`race_timer_handle_command()`), but use separate event queues based on payload requirements. The buttons carry no additional information but network and serial inputs can carry various metadata items:

* **Local Input Queue (`input-events.h`):** Receives events from physical hardware via a simple `ButtonID` / `InputSource` pair.
* **Main System Event Queue (`race/system-event-queue.h`):** Receives `SystemEvent` payloads from **Serial** (`net/serial-protocol.h`) and the network — primarily a persistent **WebSocket** (`/ws`) connection held open by each gate, with **HTTP POST `/api/event`** kept as a secondary one-shot fallback for the same event schema (both handled by `net/http-server.h`). This separate queue is required to transport rich metadata (mouse name, `gate_id`, remote timestamps).

The core state machine remains completely agnostic to which queue or physical device produced an event.

---

## 2. Core Processing Flow (Local Input)

1. **Polling (`Core 1`):** A dedicated task (`input_poll_task`, `main.cpp`) polls physical inputs every 15ms (`INPUT_POLL_PERIOD_MS`).
2. **Queueing:** Physical presses are pushed to the shared FreeRTOS queue.
3. **Execution (`loop()`):** The main loop drains the queue via `input_queue_drain(input_event_handler)`.
4. **Command Mapping:** The handler maps `ButtonID` and `InputEventType` to a `RaceCommand` using `BUTTON_COMMAND_MAP` and updates `RaceState`.
5. **Visual Feedback:** Updates the NeoKey status LEDs using `neokey_reflect_race_state()`.

---

## 3. Command Mapping Reference

Input intent is centralized in `BUTTON_COMMAND_MAP` (`race/race-command-source.h`). Changing a button's press or hold behavior requires editing a single row in this table:

```c++
constexpr ButtonCommandMap BUTTON_COMMAND_MAP[NUM_BUTTONS] = {
/* BTN_ARM   */ {RaceCommand::ARM,       RaceCommand::RESTART},
/* BTN_START */ {RaceCommand::START,     RaceCommand::NONE},
/* BTN_GOAL  */ {RaceCommand::GOAL,      RaceCommand::NONE},
/* BTN_TOUCH */ {RaceCommand::NONE,      RaceCommand::NONE},
};
```

---

## 4. Input Producers

### NeoKey 1x4 Keypad (Primary Local Input)

* **Status:** Active.
* **Driver:** `src/neokey-driver.h`, `neokey-buttons.h`, `neokey-pixels.h`
* **Bus Address:** I2C address `0x30` (Adafruit seesaw).
* **Fault Tolerance:** Non-blocking initialization via a background FreeRTOS task. If missing or disconnected mid-run, `Neokey::isAvailable()` allows the system to degrade silently to no-ops without blocking other inputs.
* **Supported Events:** Handles `PRESSED` and `HELD` events.
* **LED Feedback:** Features 4 onboard NeoPixels. .
* **Special Navigation Behaviour:** Holding the NeoKey `TOUCH` key bypasses the command table to trigger an immediate UI return-to-menu action.

### Physical GPIO Buttons (Fallback)

* **Status:** Dormant (No active boards set `HAS_GPIO_BUTTONS`).
* **Driver:** `gpio-buttons.h` / `button/button.h`
* **Logic:** Debounced active-low inputs (Buttons A/B/C). Fires `PRESSED` on the initial edge and `HELD` once if held past `GPIO_BUTTON_LONG_PRESS_MS`. Maps directly through `BUTTON_COMMAND_MAP`.

These can be used as alternate manual inputs or could be wired to external gate hardware. In the latter case, they woudld really need to drive an interrupt toget accurate timing data.

### Touchscreen Panel

* **Status:** Active for UI navigation only (Not a race command producer).
* **Hardware Support:** FT6336U / CST820 (Capacitive) and XPT2046 (Resistive), abstracted via LovyanGFX.
* **Handling:** Polled directly on Core 0 by LVGL (`lvgl_touch_init()`). It never posts to the local input queue.
* **Debounce Protection:** Enforces a 250ms touch lockout (`trigger_touch_lockout()`) immediately following screen transitions.

### Remote Producers (Serial, WebSocket & HTTP)

* **Serial Protocol (`net/serial-protocol.h`):** Parses legacy bracket-CSV RATS V2 commands (`<type,value>`). A `NewMouse` line maps to `RaceCommand::RESTART`, not `NEW_MOUSE` — deliberately, so it works from any race state (`race_command_from_serial()`, `race/race-command-source.h`). Everything else (`SetMode` → `ENTER_CALIBRATION`/`RESUME_TIMER`, `ExtraRun` → `EXTRA_RUN`, etc.) is handled by a separate function in the same file, `serial_protocol_handle_info_message()`.
* **WebSocket (`net/http-server.h`, route `/ws`):** The primary transport for remote gate boards — each gate holds one persistent connection open, parsed by `ws_event_handler()`.
* **HTTP Server (`net/http-server.h`):** `POST /api/event` is a secondary, one-shot fallback carrying the same JSON event schema as `/ws`.

  Both the WebSocket and HTTP POST paths share one dispatch function, `handle_gate_event_json()` → `race_command_from_http()` (`HTTP_EVENT_COMMAND_MAP`, `race/race-command-source.h`) — there's no separate WS-only command map.

---

## 5. State Machine & Navigation

Race progression is tracked in `race/race-timer.h` across two primary enums:

* **`RaceState`:** `CALIBRATE` | `NEW_MOUSE` | `WAITING` | `ARMED` | `RUNNING` | `GOAL` | `TIMED_OUT`
* **`RaceCommand`:** `NONE` | `ARM` | `START` | `GOAL` | `RESTART` | `ENTER_CALIBRATION` | `RESUME_TIMER` | `EXTRA_RUN`

Screen transitions (e.g., loading screens via EEZ Studio’s `loadScreen()`) run independently of race state execution.

---

## 6. Touch Calibration (`display/touch-calibration.h`)

* **Scope:** 4-corner interactive calibration wizard, stored in NVS. Required by resistive screens (ADC scaling) and capacitive screens (rotation offset corrections). Controlled per board via `TOUCH_NEEDS_CALIBRATION`.
* **Current Behaviour:** Runs automatically at boot (`setup()`) before launching `input_poll_task`. A test is made for an existing calibration and, if there is none, halts the startup to allow manual calibration.
* **Manual Calibration**: if needed, the main menu screen provides a manual calibration option.

---

## 7. Board Capability Matrix

| Board Model | Touch Controller | Calibration | GPIO Buttons | NeoKey (SDA/SCL) | NeoKey Tested |
| --- | --- | --- | --- | --- | --- |
| **Freenove S3 CYD** | FT6336U (Capacitive) | Yes | None | 6 / 5 | Present & Absent |
| **JC2432W328C** | CST820 (Capacitive) | Yes | None | 21 / 22 | Present & Absent |
| **CYD2USB (ILI9341)** | XPT2046 (Resistive) | Yes | None | 27 / 22 | Present & Absent |
| **CYD2USB (ST7789)** | XPT2046 (Resistive) | Yes | None | 27 / 22 | Present & Absent |

*Note: Hardware capability flags and pin definitions are located in `boards/*.h`.*