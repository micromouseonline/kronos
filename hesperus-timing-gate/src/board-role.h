// ----------------------------------------------------------------------------
//  board-role.h — Which race event a gate's two physical channels report as,
//  set via the `role <start|goal>` serial command (see provisioning-
//  commands.h) and persisted to NVS. Cerberus's real ingestion endpoint
//  (cerberus-gate-controller/src/net/http-server.h's POST /api/event) has no
//  concept of gate roles at all -- it just executes whatever `event` string
//  arrives (ARM/START/GOAL/RESTART/NEW_MOUSE, see
//  cerberus-gate-controller/src/race/race-command-source.h's
//  HTTP_EVENT_COMMAND_MAP). Role mapping is therefore entirely this board's
//  job: translate "channel A/B fired" into the right event name before
//  sending it.
//
//  Wiring: channel A is GPIO 7, channel B is GPIO 6, both active-low.
//  START boards: channel A -> ARM, channel B -> START.
//  GOAL boards: both channels -> GOAL (single GOAL event, no per-lane
//  distinction -- matches cerberus's RaceCommand::GOAL having no lane
//  concept today).
// ----------------------------------------------------------------------------
#pragma once

#include <Arduino.h>
#include <Preferences.h>

const char* const BOARD_ROLE_NAMESPACE = "board-cfg";

enum class BoardRole { UNSET,
                        START,
                        GOAL };

/// @brief Loads the saved role from NVS. Returns BoardRole::UNSET if the
/// `role` command has never been run on this board.
inline BoardRole board_role_load() {
  Preferences prefs;
  prefs.begin(BOARD_ROLE_NAMESPACE, true);
  uint8_t stored = prefs.getUChar("role", static_cast<uint8_t>(BoardRole::UNSET));
  prefs.end();
  return static_cast<BoardRole>(stored);
}

inline void board_role_save(BoardRole role) {
  Preferences prefs;
  prefs.begin(BOARD_ROLE_NAMESPACE, false);
  prefs.putUChar("role", static_cast<uint8_t>(role));
  prefs.end();
}

inline const char* board_role_name(BoardRole role) {
  switch (role) {
    case BoardRole::START:
      return "start";
    case BoardRole::GOAL:
      return "goal";
    default:
      return "unset";
  }
}

/// @brief Maps this board's role and which physical channel fired to the
/// event string cerberus's /api/event expects. Returns nullptr if role is
/// UNSET -- callers must refuse to send rather than guess.
inline const char* board_event_name(BoardRole role, bool is_channel_a) {
  switch (role) {
    case BoardRole::START:
      return is_channel_a ? "ARM" : "START";
    case BoardRole::GOAL:
      return "GOAL";
    default:
      return nullptr;
  }
}
