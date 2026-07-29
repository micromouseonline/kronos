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
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
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

// Cerberus and every hesperus gate are stations on the same AP, so the
// Wi-Fi TSF counter (already used for gate-event timing) gives a shared,
// sub-ms-precision clock across boards -- dividing to ms gives a common
// timeline that logs from different boards can be interleaved against
// after the fact, without needing NTP or any other clock sync. Only
// meaningful once Wi-Fi/beacon sync has occurred; lines logged before then
// (early boot) will show a small/near-zero value.
inline uint64_t debug_timestamp_ms() {
  return esp_wifi_get_tsf_time(WIFI_IF_STA) / 1000;
}

template <typename T>
inline void debug_print(T value) {
  serial_write_lock();
  Serial.printf("#[T=%llums] ", debug_timestamp_ms());
  Serial.print(value);
  serial_write_unlock();
}

template <typename T>
inline void debug_println(T value) {
  serial_write_lock();
  Serial.printf("#[T=%llums] ", debug_timestamp_ms());
  Serial.println(value);
  serial_write_unlock();
}

inline void debug_println() {
  serial_write_lock();
  Serial.printf("#[T=%llums]\n", debug_timestamp_ms());
  serial_write_unlock();
}

inline void debug_printf(const char *fmt, ...) {
  char buf[128];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  serial_write_lock();
  Serial.printf("#[T=%llums] ", debug_timestamp_ms());
  Serial.print(buf);
  serial_write_unlock();
}

// ----------------------------------------------------------------------------
//  Queued logging -- for call sites that must never block on Serial I/O
//  (e.g. net/http-server.h's http_log_request(), which runs inside an
//  AsyncWebServer request callback: any delay here adds directly to the
//  client's round-trip time, and a contended serial_write_mutex or a full
//  UART TX FIFO can stall long enough to blow through the client's own HTTP
//  timeout and trigger a needless retry -- observed in practice logging
//  every /api/event request while a gate's retry budget was already tight).
//  debug_log_enqueue() only ever copies a fixed-size struct into a queue
//  (non-blocking, drops the message if full) -- the actual Serial write
//  happens later, on debug_log_drain_task's own task, off the caller's
//  critical path entirely.
// ----------------------------------------------------------------------------
struct LogMessage {
  char text[160];
};

inline QueueHandle_t debug_log_queue = xQueueCreate(16, sizeof(LogMessage));

/// @brief Formats into a fixed buffer and enqueues for debug_log_drain_task
/// to print later -- never blocks on Serial. Silently drops the message if
/// the queue is full (16 deep) rather than blocking the caller or evicting
/// an older entry: losing an occasional diagnostic line under load is
/// preferable to adding response latency on the path that's under load.
/// The timestamp is captured here, at enqueue time, not when
/// debug_log_drain_task eventually prints it -- printing is deferred by
/// design, so a print-time timestamp would misrepresent when the logged
/// event actually happened.
inline void debug_log_enqueue(const char *fmt, ...) {
  uint64_t ts_ms = debug_timestamp_ms();
  char body[136];
  va_list args;
  va_start(args, fmt);
  vsnprintf(body, sizeof(body), fmt, args);
  va_end(args);

  LogMessage msg;
  snprintf(msg.text, sizeof(msg.text), "[T=%llums] %s", ts_ms, body);
  xQueueSend(debug_log_queue, &msg, 0);
}

inline void debug_log_drain_task(void *) {
  LogMessage msg;
  for (;;) {
    if (xQueueReceive(debug_log_queue, &msg, portMAX_DELAY) == pdTRUE) {
      serial_write_lock();
      Serial.print('#');
      Serial.println(msg.text);
      serial_write_unlock();
    }
  }
}

/// @brief Starts the background task that drains debug_log_queue to Serial.
/// Call once from setup(), after Serial.begin() and before anything that
/// might call debug_log_enqueue().
inline void debug_log_init() {
  xTaskCreatePinnedToCore(debug_log_drain_task, "log_drain", 4096, nullptr, 1, nullptr, 1);
}
