// ----------------------------------------------------------------------------
//  board-role.h — Which race event a gate's two physical channels report as.
//  Determined authoritatively at every boot by a hardware jumper
//  (JUMPER_DRIVE_PIN driven low, JUMPER_SENSE_PIN read with its internal
//  pull-up): wired together -> START, left disconnected -> GOAL -- a fixed
//  physical per-board decision, not a software setting. See
//  board_role_from_jumper() below. JUMPER_DRIVE_PIN/JUMPER_SENSE_PIN
//  themselves come from the per-board build_flags in
//  platformio.ini/boards.ini (same convention as STATUS_LED/GATE_PIN_A/
//  GATE_PIN_B) -- e.g. GPIO9/8 on most boards, GPIO7/14 on the QT Py ESP32
//  Pico, which needs GPIO8 free for its NeoPixel power-enable pin instead.
//
//  `role <start|goal>` (provisioning-commands.h) and NVS still exist as a
//  same-session manual override for testing without re-wiring the jumper --
//  main.cpp's setup() re-reads the jumper and overwrites NVS on every boot,
//  so a `role` override only lasts until the next power cycle.
//
//  Cerberus's real ingestion endpoint
//  (cerberus-gate-controller/src/net/http-server.h's POST /api/event) has no
//  concept of gate roles at all -- it just executes whatever `event` string
//  arrives (ARM/START/GOAL/RESTART/NEW_MOUSE, see
//  cerberus-gate-controller/src/race/race-command-source.h's
//  HTTP_EVENT_COMMAND_MAP). Role mapping is therefore entirely this board's
//  job: translate "channel A/B fired" into the right event name before
//  sending it.
//
//  Wiring: channel A is GATE_PIN_A, channel B is GATE_PIN_B (both
//  active-low, per-board build_flags -- see main.cpp).
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

// JUMPER_DRIVE_PIN drives low; JUMPER_SENSE_PIN senses it through its
// internal pull-up. A wire between the two reads low (shorted -> START);
// left disconnected, the pull-up holds JUMPER_SENSE_PIN high (GOAL). On
// most boards JUMPER_DRIVE_PIN (GPIO9) is a strapping pin, but that's
// resolved by the ROM bootloader before setup() ever runs, so driving it
// here afterwards doesn't interfere with boot -- not necessarily true of
// every board's JUMPER_DRIVE_PIN value, worth checking against the target
// chip's own strapping-pin list before repurposing a new one here.

/// @brief Reads the start/goal hardware jumper. Always returns a definite
/// answer (never UNSET) -- disconnected is itself a valid, meaningful
/// reading (GOAL), not an absence of one.
inline BoardRole board_role_from_jumper() {
  pinMode(JUMPER_DRIVE_PIN, OUTPUT);
  digitalWrite(JUMPER_DRIVE_PIN, LOW);
  pinMode(JUMPER_SENSE_PIN, INPUT_PULLUP);
  delayMicroseconds(10);  // let the pull-up settle before sampling
  return (digitalRead(JUMPER_SENSE_PIN) == LOW) ? BoardRole::START : BoardRole::GOAL;
}

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
