// ----------------------------------------------------------------------------
//  debug-log.h — Ad-hoc debug output, shared with the legacy host protocol's
//  UART (net/messages.h's `<type,value>\r\n` lines, see
//  net/serial-protocol.h). Every call here writes a single '#'-prefixed
//  comment line: the host supervisor only parses text between '<' and '>',
//  and treats a line starting with '#' as a comment to display and skip --
//  so debug output is safe to send at any time, including while the legacy
//  protocol is live, as long as each call is one complete line.
//
//  Each debug_print/println/printf call must render a whole line (or, for
//  debug_print, a value that already ends in '\n' -- see input-events.h's
//  debug_print() member for that case): calling debug_print() repeatedly to
//  assemble one line piece by piece would emit a '#' before every fragment
//  instead of once at the start.
//
//  serial_write_mutex guards every write to Serial across all producers of
//  it (this file, net/messages.h's serial_send_message(), and
//  serial-protocol.h's RX echo, which itself goes through debug_printf) --
//  they run from different FreeRTOS tasks on both cores (e.g. Core 0's
//  Wi-Fi connect task vs. Core 1's main loop() telemetry tick) with no other
//  synchronization, and an unguarded interleaved write can corrupt or
//  silently swallow whichever side loses the race. Created eagerly (not
//  lazily like neokey_bus_mutex) since Serial output can start from the
//  very first line of setup(), before any other init has run.
//
//  Deliberately a leaf header (only depends on Arduino.h and FreeRTOS) so
//  every call site can include it without risk of a circular include back
//  through net/serial-protocol.h -> race/race-command-source.h ->
//  input-events.h -> (this file, if it depended on serial-protocol.h).
// ----------------------------------------------------------------------------
#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <stdarg.h>
#include <stdio.h>

// Gates high-frequency, per-event debug traces (button/touch/GPIO/NeoKey
// events in input-events.h, inbound serial RX echo in
// net/serial-protocol.h) behind the Settings screen's sw_debug_verbose
// switch. Separate from the always-on one-shot lifecycle messages (Wi-Fi
// connect, HTTP server start, etc.) elsewhere in this codebase, which stay
// unconditional regardless of this flag -- those are rare and useful even
// with verbose logging off.
inline bool g_debug_verbose_enabled = false;

inline SemaphoreHandle_t serial_write_mutex = xSemaphoreCreateMutex();

inline void serial_write_lock() {
  xSemaphoreTake(serial_write_mutex, portMAX_DELAY);
}
inline void serial_write_unlock() {
  xSemaphoreGive(serial_write_mutex);
}

template <typename T>
inline void debug_print(T value) {
  serial_write_lock();
  Serial.print('#');
  Serial.print(value);
  serial_write_unlock();
}

template <typename T>
inline void debug_println(T value) {
  serial_write_lock();
  Serial.print('#');
  Serial.println(value);
  serial_write_unlock();
}

inline void debug_println() {
  serial_write_lock();
  Serial.println('#');
  serial_write_unlock();
}

inline void debug_printf(const char *fmt, ...) {
  char buf[128];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  serial_write_lock();
  Serial.print('#');
  Serial.print(buf);
  serial_write_unlock();
}
