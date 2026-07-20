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

#include "debug-log.h"
#include "race/race-command-source.h"
#include "race/system-event-queue.h"

// Manual-testing aid only, NOT part of the wire protocol -- independent of
// debug-log.h's g_uart_owned_by_protocol (which gates OFF once this task
// starts owning the UART; this flag gates the opposite thing, an
// interactive echo that only makes sense while this task IS running). Must
// be set false before connecting real host software: this text lands on
// the same UART the host parses as <type,value> lines and would corrupt
// its parser exactly like any other ad-hoc debug output.
inline bool g_serial_protocol_rx_echo = true;

inline bool serial_protocol_parse_line(const char *line, SerialLine *out) {
  if (line == nullptr || out == nullptr) {
    return false;
  }
  int type;
  long value;
  // Legacy protocol's own doc: "Any characters following the closing
  // bracket are ignored by the supervisor" -- sscanf naturally does the
  // same, it just needs the leading "<type,value>" to match.
  if (sscanf(line, "<%d,%ld>", &type, &value) != 2) {
    return false;
  }
  out->type = type;
  out->value = value;
  return true;
}

inline void serial_protocol_rx_task(void *) {
  static char buf[64];
  static size_t len = 0;
  for (;;) {
    while (Serial.available() > 0) {
      char c = (char)Serial.read();
      // Accept CR, LF, or CRLF as end-of-line -- terminal/host line endings
      // vary and there's no way to negotiate one. The `len > 0` guard
      // swallows the second half of a CRLF pair (it arrives as an
      // already-empty line) without emitting a spurious blank-line parse.
      if (c == '\n' || c == '\r') {
        if (len > 0) {
          buf[len] = '\0';
          if (g_serial_protocol_rx_echo) {
            // Confirms to whoever's typing exactly what bytes were seen,
            // independent of terminal echo settings.
            Serial.printf("[serial-protocol] rx: \"%s\"\n", buf);
          }
          SerialLine line;
          if (serial_protocol_parse_line(buf, &line)) {
            RaceCommand cmd = race_command_from_serial(line);
            if (cmd != RaceCommand::NONE) {
              system_event_post(cmd, static_cast<uint64_t>(esp_timer_get_time()));
            }
          }
          len = 0;
        }
      } else if (len < sizeof(buf) - 1) {
        buf[len++] = c;
      }
    }
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

inline void serial_protocol_init() {
  g_uart_owned_by_protocol = true;
  xTaskCreatePinnedToCore(serial_protocol_rx_task, "serial_rx", 4096, nullptr, 1, nullptr, 1);
}
