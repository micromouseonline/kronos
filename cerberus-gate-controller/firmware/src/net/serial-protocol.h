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
  // sscanf only reads the type and locates the start of the value field --
  // %n is left untouched (-1) unless the literal ',' was actually reached.
  // The value itself is NOT scanned with %[^>]: a scanset must match at
  // least one character, so an empty field ("<98,>") would fail the whole
  // parse and the line would be silently dropped.
  int consumed = -1;
  if (sscanf(line, "<%d,%n", &type, &consumed) != 1 || consumed < 0) {
    return false;
  }
  // Everything between the comma and the closing '>' is the value, as text
  // -- some RATS V2 messages carry a name/keyword (NewMouse, ContestName,
  // EventName, SetMode), not just a number. Numeric messages still parse
  // fine out of the same text field via serial_line_value_as_long().
  // Legacy protocol's own doc: "Any characters following the closing
  // bracket are ignored by the supervisor".
  const char *value_start = line + consumed;
  // Require an actual '>' -- an unterminated "<98,0" is not a message.
  // Found via manual testing (tools/testing/SERIAL-TEST-PLAN.md 7.1)
  // accepting "<98,0" as valid.
  const char *value_end = strchr(value_start, '>');
  if (value_end == nullptr) {
    return false;
  }
  // Trim surrounding spaces/tabs, so "<98, >" is the same empty value as
  // "<98,>" and "<99, TIMER>" still matches its keyword. Interior spaces
  // (a name like "Name With Spaces") are kept. An empty value is valid --
  // each message handler decides what that means for its own type.
  while (value_start < value_end && (*value_start == ' ' || *value_start == '\t')) {
    value_start++;
  }
  while (value_end > value_start && (value_end[-1] == ' ' || value_end[-1] == '\t')) {
    value_end--;
  }
  size_t value_len = static_cast<size_t>(value_end - value_start);
  if (value_len >= sizeof(out->value)) {
    value_len = sizeof(out->value) - 1;  // truncate an over-long value
  }
  memcpy(out->value, value_start, value_len);
  out->value[value_len] = '\0';
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
      // race_timer_enter_new_mouse(). A host NewMouse with no name
      // ("<98,>" or "<98, >") gets a fixed placeholder rather than an
      // empty payload, which would otherwise fall through to a canned
      // mouse_names[] pick.
      const char *mouse_name = (line.value[0] != '\0') ? line.value : SERIAL_UNNAMED_MOUSE_NAME;
      system_event_post(cmd, timestamp_us, mouse_name, /*payload_is_mouse_name=*/true);
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
