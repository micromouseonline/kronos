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
#include "net/messages.h"
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

// Parsed by net/serial-protocol.h's line parser from a `<type,value>` wire
// message (see net/messages.h). Defined here, not in serial-protocol.h, so
// this file doesn't need serial-protocol.h to know its own payload type --
// HttpGateEvent will follow the same reasoning once the HTTP producer
// exists.
struct SerialLine {
  int type;
  long value;
};

inline RaceCommand race_command_from_serial(const SerialLine &line) {
  // TODO(mouse-name): the legacy <98,value> message carries no mouse name
  // (value is always 0 per the protocol's own doc); once the wire format
  // can carry one, thread it through SerialLine -> SystemEvent.payload ->
  // an optional-name parameter on race_timer_enter_new_mouse().
  // The protocol doc for MSG_NewMouse: "value argument will always be
  // passed as 0" -- treated as a strict precondition, not just a comment:
  // a non-zero value means this isn't the message it looks like, reject it.
  if (line.type == MSG_NEW_MOUSE && line.value == 0) {
    // RESTART, not NEW_MOUSE -- race_timer_handle_command's WAITING/
    // RUNNING/GOAL branches only act on RESTART, same reasoning as
    // BUTTON_COMMAND_MAP's ARM-hold above. This way the host's new-mouse
    // command always takes effect regardless of current race state.
    return RaceCommand::RESTART;
  }
  return RaceCommand::NONE;  // MSG_SetMode and everything else: out of scope this round
}

// Parsed by net/http-server.h's POST /api/event JSON handler (DESIGN-
// REQUIREMENT.md's Asynchronous HTTP Listener). gate_id is carried through
// unchanged into SystemEvent::payload (the same field NEW_MOUSE's mouse
// name would use); event is looked up in HTTP_EVENT_COMMAND_MAP below.
struct HttpGateEvent {
  char gate_id[32];
  char event[16];
  uint64_t tsf_us;
  uint64_t gate_us;
};

struct HttpEventCommandMap {
  const char *name;
  RaceCommand command;
};

// One row per accepted `event` string -- mirrors BUTTON_COMMAND_MAP's "one
// place to change what a source produces" role, just keyed by name instead
// of ButtonID since a remote gate has no button to press. RESTART is
// reachable here (unlike from a physical button, see BUTTON_COMMAND_MAP's
// comment) since DESIGN-REQUIREMENT.md notes it "has no dedicated physical
// button -- it is only ever raised via serial or HTTP command."
// NEW_MOUSE -> RESTART, not RaceCommand::NEW_MOUSE -- same reasoning as
// BUTTON_COMMAND_MAP's ARM-hold and race_command_from_serial's <98,0>
// handling above: race_timer_handle_command's WAITING/RUNNING/GOAL branches
// only act on RESTART, so this is what makes an HTTP new-mouse request take
// effect regardless of current race state, matching the other two producers.
constexpr HttpEventCommandMap HTTP_EVENT_COMMAND_MAP[] = {
    {"ARM", RaceCommand::ARM},
    {"START", RaceCommand::START},
    {"GOAL", RaceCommand::GOAL},
    {"RESTART", RaceCommand::RESTART},
    {"NEW_MOUSE", RaceCommand::RESTART},
};

inline RaceCommand race_command_from_http(const HttpGateEvent &evt) {
  for (const auto &row : HTTP_EVENT_COMMAND_MAP) {
    if (strcmp(evt.event, row.name) == 0) {
      return row.command;
    }
  }
  return RaceCommand::NONE;  // unrecognised `event` value
}
