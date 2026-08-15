#pragma once

#include <Arduino.h>

#include "debug-log.h"

/// @file messages.h
/// @brief Legacy CERBERUS <-> host-PC wire protocol: `<type,value>\r\n`.
///
/// This is the numeric protocol from the original Arduino/Visual-Basic
/// micromouse timer, extended with the inbound messages from
/// docs/preferredMessageSequencesV2.pdf ("RATS" protocol -- Registration
/// And Timing System). It is kept here, in its intended home, purely as a
/// wire-format layer: no dependency on RaceCommand/RaceState. See
/// net/serial-protocol.h for the RX side and race/race-serial-telemetry.h
/// for the TX side.

enum MessageType : int {
  MSG_WATCHDOG = 0,
  MSG_CURRENT_STATE = 4,
  MSG_C1_SPLIT_TIME = 12,
  MSG_C1_RUN_TIME = 13,
  MSG_COURSE_TIME_MS = 30,

  // Outbound (Arduino to PC): a short TOUCH ("T") press while the main race
  // screen is showing. BUTTON_COMMAND_MAP (race-command-source.h) no longer
  // feeds this into the RaceCommand pipeline at all - this is purely a
  // host-facing notification, the same "bypass the state
  // machine, just tell RATS" pattern serial_send_run_time() and EXTRA_RUN
  // already use. Value always 1, mirroring MSG_EXTRA_RUN's shape just below
  // (not otherwise related to it).
  MSG_TOUCH_SHORT_PRESS = 91,

  // Inbound (PC to Arduino), from preferredMessageSequencesV2.pdf Annex A.
  MSG_EXTRA_RUN = 92,     // decrement current run count by 1 (value always 1)
  MSG_ENTRY_TIME_S = 93,  // seconds allowed for this entry, sent after NewMouse
  MSG_ALLOWED_RUNS = 94,  // number of runs allowed for this entry, sent after NewMouse
  MSG_EVENT_NAME = 95,    // name of the robotics event in progress, e.g. "Minos 2026"
  MSG_CONTEST_NAME = 96,  // name of the current contest, e.g. "Senior Maze Solver"
  MSG_REQUEST_TYPE = 97,  // PC asks the gate to identify itself (value always 0)
  MSG_NEW_MOUSE = 98,     // value is now the mouse's name (text), not always 0
  MSG_SET_MODE = 99,      // value is "TIMER" or "CALIBRATION"

  // Outbound (Arduino to PC), reply to MSG_REQUEST_TYPE. Deliberately the
  // same numeric code as MSG_CONTEST_NAME above -- the source spec's own
  // Annex A reuses 96 for both "ContestName" (inbound) and "TimerType"
  // (outbound); confirmed a naming quirk in the document, not a runtime
  // conflict, since RX and TX are independent directions on the same wire.
  // Not merged into one name so each call site stays self-documenting.
  MSG_TIMER_TYPE = 96,
};

// Maps a wire type code back to a human-readable name, for the trailing
// comment below. Takes int, not MessageType -- switching on the enum type
// wouldn't help anyway, since MSG_CONTEST_NAME and MSG_TIMER_TYPE share the
// same value (96, see the enum's comment above) and can't both be case
// labels here regardless of the switch expression's type. MSG_CONTEST_NAME
// is RX-only and never passed to serial_send_message, so labelling 96 as
// TIMER_TYPE (the only value actually sent through this function) is always
// correct in practice.
inline const char *message_type_name(int type) {
  switch (type) {
    case MSG_WATCHDOG:
      return "WATCHDOG";
    case MSG_CURRENT_STATE:
      return "CURRENT_STATE";
    case MSG_C1_SPLIT_TIME:
      return "C1_SPLIT_TIME";
    case MSG_C1_RUN_TIME:
      return "C1_RUN_TIME";
    case MSG_COURSE_TIME_MS:
      return "COURSE_TIME_MS";
    case MSG_TOUCH_SHORT_PRESS:
      return "TOUCH_SHORT_PRESS";
    case MSG_EXTRA_RUN:
      return "EXTRA_RUN";
    case MSG_ENTRY_TIME_S:
      return "ENTRY_TIME_S";
    case MSG_ALLOWED_RUNS:
      return "ALLOWED_RUNS";
    case MSG_EVENT_NAME:
      return "EVENT_NAME";
    case MSG_TIMER_TYPE:
      return "TIMER_TYPE";
    case MSG_REQUEST_TYPE:
      return "REQUEST_TYPE";
    case MSG_NEW_MOUSE:
      return "NEW_MOUSE";
    case MSG_SET_MODE:
      return "SET_MODE";
    default:
      return "UNKNOWN";
  }
}

// Every outbound line ends in " #" plus the message type's name, unconditionally
// -- originally a legacy quirk limited to MSG_CURRENT_STATE (see the
// original protocol comment: "Any characters following the closing bracket
// are ignored by the supervisor"), now applied to every message so the wire
// is self-describing for whoever's eyeballing it, without the host's parser
// caring. An explicit comment (e.g. race_state_name() for MSG_CURRENT_STATE)
// is appended after the type name rather than replacing it.
inline void serial_send_message(int type, unsigned long value, const char *comment = nullptr) {
  // Locked for the whole line -- shares the UART with debug-log.h's ad-hoc
  // output (Core 0's Wi-Fi task, in particular) and an unguarded interleave
  // between the two would corrupt this line for the host's parser.
  serial_write_lock();
  Serial.print('<');
  Serial.print(type);
  Serial.print(',');
  Serial.print(value);
  Serial.print('>');
  Serial.print(" # ");
  Serial.print(message_type_name(type));
  if (comment != nullptr) {
    Serial.print(':');
    Serial.print(comment);
  }
  Serial.println();
  serial_write_unlock();
}

// String-valued counterpart to serial_send_message() -- needed for replies
// whose value is text rather than a number (currently just MSG_TIMER_TYPE's
// "1CH"/"2CH", see race-command-source.h's reply to MSG_REQUEST_TYPE).
inline void serial_send_message_str(int type, const char *value, const char *comment = nullptr) {
  serial_write_lock();
  Serial.print('<');
  Serial.print(type);
  Serial.print(',');
  Serial.print(value);
  Serial.print('>');
  Serial.print(" # ");
  Serial.print(message_type_name(type));
  if (comment != nullptr) {
    Serial.print(':');
    Serial.print(comment);
  }
  Serial.println();
  serial_write_unlock();
}

inline void serial_send_run_time(unsigned long time_ms) {
  // Sent three times, 20ms apart -- RATS V2's definitive score-time
  // report (docs/preferredMessageSequencesV2.pdf Annex A: "repeated for a
  // total of 3 transmissions to mitigate any line errors... RATS will
  // only record the run time once and discards additional messages for
  // the same run time", matched in docs/RACE-STATE-MACHINE.md's GOAL
  // section). The old legacy protocol only sent this twice; RATS V2
  // supersedes that.
  serial_send_message(MSG_C1_RUN_TIME, time_ms);
  delay(20);
  serial_send_message(MSG_C1_RUN_TIME, time_ms);
  delay(20);
  serial_send_message(MSG_C1_RUN_TIME, time_ms);
}
