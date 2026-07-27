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
// exists. `value` holds the raw text between the comma and '>' -- RATS V2
// (docs/preferredMessageSequencesV2.pdf) carries names/keywords on several
// message types, not just numbers, so the field can't be a plain long
// anymore; see serial_line_value_as_long() below for the numeric ones.
struct SerialLine {
  int type;
  char value[32];
};

inline long serial_line_value_as_long(const SerialLine &line) {
  return strtol(line.value, nullptr, 10);
}

// Host session metadata captured from ContestName/EventName/AllowedRuns/
// EntryTimeS -- informational only this round (not read anywhere yet, not
// enforced against mouse_run_count/entry_sw). Kept here rather than in
// race-timer.h since these describe the host's session, not race state.
inline char g_contest_name[32] = "";
inline char g_event_name[32] = "";
inline long g_allowed_runs = -1;      // -1 = not set by host
inline long g_entry_time_s_limit = -1;

inline RaceCommand race_command_from_serial(const SerialLine &line) {
  if (line.type == MSG_NEW_MOUSE) {
    // RESTART, not NEW_MOUSE -- race_timer_handle_command's WAITING/
    // RUNNING/GOAL branches only act on RESTART, same reasoning as
    // BUTTON_COMMAND_MAP's ARM-hold above. This way the host's new-mouse
    // command always takes effect regardless of current race state.
    return RaceCommand::RESTART;
  }
  return RaceCommand::NONE;
}

// Handles every RATS V2 inbound message that ISN'T a race-state transition
// (see race_command_from_serial() above for MSG_NEW_MOUSE, which is).
// These are host/session metadata and device-identification requests --
// routing them through SystemEvent/RaceCommand would force them into a
// vocabulary that's specifically the race state machine's (see
// race-timer.h's header comment), so they're captured/replied to directly
// here instead, in the same RX task, alongside the queue post.
inline void serial_protocol_handle_info_message(const SerialLine &line) {
  switch (line.type) {
    case MSG_CONTEST_NAME:
      strncpy(g_contest_name, line.value, sizeof(g_contest_name) - 1);
      g_contest_name[sizeof(g_contest_name) - 1] = '\0';
      break;
    case MSG_EVENT_NAME:
      strncpy(g_event_name, line.value, sizeof(g_event_name) - 1);
      g_event_name[sizeof(g_event_name) - 1] = '\0';
      break;
    case MSG_ALLOWED_RUNS:
      g_allowed_runs = serial_line_value_as_long(line);
      break;
    case MSG_ENTRY_TIME_S:
      g_entry_time_s_limit = serial_line_value_as_long(line);
      break;
    case MSG_EXTRA_RUN:
      // Not wired into mouse_run_count this round -- no AllowedRuns
      // enforcement exists yet for this to meaningfully extend (see
      // IMPLEMENTATION-PLAN.md/plan notes). Logged so it's not silently
      // dropped.
      debug_printf("[serial-protocol] ExtraRun received (not enforced this round)\n");
      break;
    case MSG_SET_MODE:
      // TIMER/CALIBRATION: CERBERUS has no local gate hardware to
      // calibrate (gates are separate WiFi-connected devices, see
      // DESIGN-REQUIREMENT.md), so there's no established behaviour for
      // CALIBRATION here yet -- logged only, pending clarification.
      debug_printf("[serial-protocol] SetMode(%s) received (no action taken this round)\n", line.value);
      break;
    case MSG_REQUEST_TYPE:
      // CERBERUS's telemetry is C1-only (race-serial-telemetry.h has no
      // C2 messages anywhere), so it always identifies as a 1-channel timer.
      serial_send_message_str(MSG_TIMER_TYPE, "1CH");
      break;
    default:
      break;
  }
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
