// ----------------------------------------------------------------------------
//  serial-protocol.h — RX side of the legacy <type,value> host protocol
//  (net/messages.h): a Core-1 task that owns the UART, accumulates lines,
//  parses them, and posts the resulting RaceCommand onto the Main Event
//  Queue (race/system-event-queue.h). TX side is
//  race/race-serial-telemetry.h.
// ----------------------------------------------------------------------------
#pragma once

#include <Arduino.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <stdio.h>
#include <string.h>

#include "debug-log.h"
#include "race/race-command-source.h"
#include "race/system-event-queue.h"

inline bool serial_protocol_parse_line(const char *line, SerialLine *out) {
  if (line == nullptr || out == nullptr) {
    return false;
  }
  // Skip leading spaces/tabs -- sscanf's leading '<' is a literal in the
  // format, which (unlike %d) does NOT skip whitespace on its own, so a
  // line with any leading indentation (e.g. pasted from an indented
  // markdown code block, like this doc's own test sequences) would
  // otherwise fail to match at all and get silently dropped.
  while (*line == ' ' || *line == '\t') {
    line++;
  }
  int type;
  // Captures everything between the comma and the closing '>' as text --
  // some RATS V2 messages carry a name/keyword (NewMouse, ContestName,
  // EventName, SetMode), not just a number. Numeric messages still parse
  // fine out of the same text field via serial_line_value_as_long().
  // Legacy protocol's own doc: "Any characters following the closing
  // bracket are ignored by the supervisor" -- %[^>] naturally stops at
  // '>' the same way the old %ld stopped at the first non-digit.
  if (sscanf(line, "<%d,%31[^>]>", &type, out->value) != 2) {
    return false;
  }
  // sscanf's return count only reflects %d/%s-style conversions -- a
  // literal '>' in the format that never matched (e.g. an unterminated
  // "<98,0" with no closing bracket at all) still leaves the count at 2,
  // since %[^>] happily stops at end-of-string too. Found via manual
  // testing (tools/testing/SERIAL-TEST-PLAN.md 7.1) accepting "<98,0" as valid.
  // Require an actual '>' to exist somewhere in the line.
  if (strchr(line, '>') == nullptr) {
    return false;
  }
  out->type = type;
  return true;
}

// Handles one already-extracted, NUL-terminated line -- echoes it, parses
// it, and dispatches the result. Split out of serial_protocol_rx_task so
// that function can focus purely on framing complete lines out of the RX
// buffer.
inline void serial_protocol_process_line(const char *line_str) {
  if (g_debug_verbose_enabled) {
    // Confirms to whoever's typing exactly what bytes were seen,
    // independent of terminal echo settings.
    debug_log_enqueue("[serial] rx: \"%s\"", line_str);
  }
  SerialLine line;
  if (serial_protocol_parse_line(line_str, &line)) {
    uint64_t timestamp_us = static_cast<uint64_t>(esp_timer_get_time());
    RaceCommand cmd = race_command_from_serial(line);
    if (cmd != RaceCommand::NONE) {
      // line.value carries the mouse name for MSG_NEW_MOUSE (RATS V2) --
      // forwarded into SystemEvent.payload with payload_is_mouse_name set,
      // so main.cpp's system_event_handler() can tell it apart from
      // HTTP's gate_id (same field, different meaning) and thread it into
      // race_timer_enter_new_mouse().
      system_event_post(cmd, timestamp_us, line.value, /*payload_is_mouse_name=*/true);
    } else {
      // Every other RATS V2 inbound message (ContestName, EventName,
      // AllowedRuns, EntryTimeS, ExtraRun, SetMode, RequestType) --
      // handled directly (see serial_protocol_handle_info_message()'s own
      // comment on which of these still mutate race state via the queue).
      serial_protocol_handle_info_message(line, timestamp_us);
    }
  }
}

inline void serial_protocol_rx_task(void *) {
  // 256 bytes, not one line's worth -- several short RATS lines sent
  // back-to-back with no gap (a scripted multi-line send, or a fast
  // paste into a terminal) can all land in the UART's hardware buffer
  // before this task gets a scheduler slice; draining into one larger
  // buffer and extracting every complete line found in it, rather than
  // resetting a single small buffer after each line, means a burst can't
  // lose a line to timing. Found via manual testing
  // (tools/testing/SERIAL-TEST-PLAN.md Section 8: an unpaused multi-line send
  // produced no response at all).
  static char rx_buf[256];
  static size_t rx_len = 0;
  // One extracted line -- the legacy protocol's own doc already
  // guarantees messages are under 64 bytes, so this stays the per-line
  // cap regardless of how much larger rx_buf is.
  static char line_buf[64];

  for (;;) {
    while (Serial.available() > 0 && rx_len < sizeof(rx_buf) - 1) {
      rx_buf[rx_len++] = (char)Serial.read();
    }

    // Extract and process every complete line currently sitting in
    // rx_buf, in order. Accepts CR, LF, or CRLF as end-of-line --
    // terminal/host line endings vary and there's no way to negotiate
    // one.
    size_t start = 0;
    for (size_t i = 0; i < rx_len; i++) {
      if (rx_buf[i] == '\n' || rx_buf[i] == '\r') {
        size_t line_len = i - start;
        // line_len == 0 swallows the second half of a CRLF pair (arrives
        // as an already-empty line) without emitting a spurious
        // blank-line parse.
        if (line_len > 0) {
          if (line_len >= sizeof(line_buf)) {
            line_len = sizeof(line_buf) - 1;  // truncate an over-long line
          }
          memcpy(line_buf, rx_buf + start, line_len);
          line_buf[line_len] = '\0';
          serial_protocol_process_line(line_buf);
        }
        start = i + 1;
      }
    }

    if (start > 0) {
      // Keep any trailing partial line (no terminator seen yet) for the
      // next pass, at the front of the buffer.
      size_t remaining = rx_len - start;
      memmove(rx_buf, rx_buf + start, remaining);
      rx_len = remaining;
    } else if (rx_len >= sizeof(rx_buf) - 1) {
      // Buffer completely full with no CR/LF anywhere in it -- a single
      // line longer than 256 bytes, or a stray unterminated fragment left
      // over (e.g. a "<98," sent with no closing bracket and nothing
      // after it). Discard rather than wedge the RX path permanently
      // full and unable to accept anything else.
      rx_len = 0;
    }

    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

inline void serial_protocol_init() {
  xTaskCreatePinnedToCore(serial_protocol_rx_task, "serial_rx", 4096, nullptr, 1, nullptr, 1);
}
