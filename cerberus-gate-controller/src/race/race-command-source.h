// ----------------------------------------------------------------------------
//  race-command-source.h — Translates raw input from each producer into the
//  race state machine's RaceCommand vocabulary (race-timer.h). One function
//  per source; a source is never faked as another source's payload type just
//  to reuse a sibling function. Local hardware (GPIO/NeoKey/touch) already
//  collapses to a ButtonID in input-events.h, so all three share
//  race_command_from_button() and, through it, BUTTON_COMMAND_MAP below.
//  Serial/HTTP carry data ButtonID can't hold (mouse name, gate_id, remote
//  timestamp) and get their own producer here once those payload types
//  exist (see docs/DESIGN-REQUIREMENT.md's Serial Monitor Task /
//  Asynchronous HTTP Listener).
// ----------------------------------------------------------------------------
#pragma once

#include "input-events.h"
#include "race-timer.h"

struct ButtonCommandMap {
  RaceCommand on_press;
  RaceCommand on_hold;
};

// One row per ButtonID -- the single place to change what a press or hold
// produces. Applies uniformly to every local producer (GPIO, NeoKey, and
// touch's short presses -- see eez-actions.cpp's action_on_timer_* -- all
// post into the same queue and land here).
//
// ARM's hold is a force-new-mouse override regardless of current race state
// (e.g. escaping a mouse_exhausted WAITING state) -- RESTART, not NEW_MOUSE,
// since race_timer_handle_command's WAITING/GOAL branches only act on
// RESTART. TOUCH's hold has no RaceCommand here: on GPIO/NeoKey it's a
// UI-only "return to menu" handled in main.cpp's input_event_handler; on
// the touch panel itself it's a separate LVGL-native long-press
// (action_on_timer_touch_long, eez-actions.cpp) that never reaches this
// table at all.
// TODO examine these against the state machine
constexpr ButtonCommandMap BUTTON_COMMAND_MAP[NUM_BUTTONS] = {
    /* BTN_ARM   */ {RaceCommand::ARM, RaceCommand::RESTART},
    /* BTN_START */ {RaceCommand::START, RaceCommand::NONE},
    /* BTN_GOAL  */ {RaceCommand::GOAL, RaceCommand::NONE},
    /* BTN_TOUCH */ {RaceCommand::NEW_MOUSE, RaceCommand::NONE},
};

inline RaceCommand race_command_from_button(ButtonID id, InputEventType type = InputEventType::PRESSED) {
  if (id < 0 || id >= NUM_BUTTONS) {
    return RaceCommand::NONE;
  }
  const ButtonCommandMap &map = BUTTON_COMMAND_MAP[id];
  return (type == InputEventType::HELD) ? map.on_hold : map.on_press;
}

// race_command_from_serial(const SerialLine&) -- TODO once the Serial
// Monitor Task (DESIGN-REQUIREMENT.md) has a payload type to parse.

// race_command_from_http(const HttpGateEvent&) -- TODO once the
// Asynchronous HTTP Listener (DESIGN-REQUIREMENT.md) has a payload type to
// parse.
