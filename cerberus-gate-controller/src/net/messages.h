#pragma once

#include <Arduino.h>

/// @file messages.h
/// @brief Legacy CERBERUS <-> host-PC wire protocol: `<type,value>\r\n`.
///
/// This is the numeric protocol from the original Arduino/Visual-Basic
/// micromouse timer (see the archived, unused `src/messages.h` at the src/
/// root for the full original comment block and the disused calibration/
/// trigger message types). It is kept here, in its intended home, purely as
/// a wire-format layer: no dependency on RaceCommand/RaceState. Only
/// MSG_NEW_MOUSE is parsed inbound this round -- see
/// net/serial-protocol.h for the RX side and race/race-serial-telemetry.h
/// for the TX side.

enum MessageType : int {
  MSG_WATCHDOG = 0,
  MSG_CURRENT_STATE = 4,
  MSG_C1_SPLIT_TIME = 12,
  MSG_C1_RUN_TIME = 13,
  MSG_COURSE_TIME_MS = 30,
  MSG_NEW_MOUSE = 98,
  MSG_SET_MODE = 99,  // parsed but not acted on this round
};

// Trailing annotation character sent after MSG_CURRENT_STATE's closing
// bracket (legacy quirk, preserved for host compatibility -- see the
// original protocol comment: "Any characters following the closing bracket
// are ignored by the supervisor"). Nothing in this codebase sets it to
// anything other than '#' yet; kept as a plain global (not `extern`, unlike
// the original) so this file is self-contained.
inline char last_char = '#';

inline void serial_send_message(int type, unsigned long value) {
  Serial.print('<');
  Serial.print(type);
  Serial.print(',');
  Serial.print(value);
  Serial.print('>');
  if (type == MSG_CURRENT_STATE) {
    Serial.print(' ');
    Serial.print(last_char);
    last_char = '#';
  }
  Serial.println();
}

inline void serial_send_run_time(unsigned long time_ms) {
  // Sent twice, 20ms apart -- matches the legacy protocol's definitive
  // score-time report (see original src/messages.h comment).
  serial_send_message(MSG_C1_RUN_TIME, time_ms);
  delay(20);
  serial_send_message(MSG_C1_RUN_TIME, time_ms);
}
