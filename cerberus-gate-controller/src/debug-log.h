// ----------------------------------------------------------------------------
//  debug-log.h — Ad-hoc debug output, gated off once the Serial Monitor
//  Task (net/serial-protocol.h) owns the UART for real host protocol
//  traffic (see docs/DESIGN-REQUIREMENT.md's Debug Output Policy: once that
//  task is live, no other ad-hoc text may write to Serial, or it corrupts
//  the host's <type,value> line parser).
//
//  Deliberately a leaf header (only depends on Arduino.h) so every call
//  site can include it without risk of a circular include back through
//  net/serial-protocol.h -> race/race-command-source.h ->
//  input-events.h -> (this file, if it depended on serial-protocol.h).
// ----------------------------------------------------------------------------
#pragma once

#include <Arduino.h>
#include <stdarg.h>
#include <stdio.h>

// Set true by net/serial-protocol.h's serial_protocol_init() once the RX
// task starts owning the UART.
inline bool g_uart_owned_by_protocol = false;

template <typename T>
inline void debug_print(T value) {
  if (!g_uart_owned_by_protocol) {
    Serial.print(value);
  }
}

template <typename T>
inline void debug_println(T value) {
  if (!g_uart_owned_by_protocol) {
    Serial.println(value);
  }
}

inline void debug_println() {
  if (!g_uart_owned_by_protocol) {
    Serial.println();
  }
}

inline void debug_printf(const char *fmt, ...) {
  if (!g_uart_owned_by_protocol) {
    char buf[128];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    Serial.print(buf);
  }
}
