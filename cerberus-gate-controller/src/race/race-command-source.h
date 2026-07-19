// ----------------------------------------------------------------------------
//  race-command-source.h — Translates raw input from each producer into the
//  race state machine's RaceCommand vocabulary (race-timer.h). One function
//  per source; a source is never faked as another source's payload type just
//  to reuse a sibling function. Local hardware (GPIO/NeoKey/touch) already
//  collapses to a ButtonID in input-events.h, so all three share
//  race_command_from_button(). Serial/HTTP carry data ButtonID can't hold
//  (mouse name, gate_id, remote timestamp) and get their own producer here
//  once those payload types exist (see docs/DESIGN-REQUIREMENT.md's Serial
//  Monitor Task / Asynchronous HTTP Listener).
// ----------------------------------------------------------------------------
#pragma once

#include "input-events.h"
#include "race-timer.h"

inline RaceCommand race_command_from_button(ButtonID id) {
  switch (id) {
    case BTN_ARM:
      return RaceCommand::ARM;
    case BTN_START:
      return RaceCommand::START;
    case BTN_GOAL:
      return RaceCommand::GOAL;
    case BTN_TOUCH:
      return RaceCommand::NEW_MOUSE;
    default:
      return RaceCommand::NONE;
  }
}

// race_command_from_serial(const SerialLine&) -- TODO once the Serial
// Monitor Task (DESIGN-REQUIREMENT.md) has a payload type to parse.

// race_command_from_http(const HttpGateEvent&) -- TODO once the
// Asynchronous HTTP Listener (DESIGN-REQUIREMENT.md) has a payload type to
// parse.
