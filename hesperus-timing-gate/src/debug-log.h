// ----------------------------------------------------------------------------
//  debug-log.h — Shared debug output, mirrors
//  cerberus-gate-controller/src/debug-log.h's shape. Every line is prefixed
//  with the current Wi-Fi TSF counter (ms) -- hesperus and cerberus are both
//  stations on the same AP, so this is a shared, sub-ms-precision clock
//  across boards (already used for gate-event timing), letting logs from
//  different boards be interleaved into a true event order after the fact.
//  Only meaningful once Wi-Fi/beacon sync has occurred; lines logged before
//  then (early boot) show a small/near-zero value.
//
//  serial_write_mutex guards every write to Serial -- multiple producers
//  call these (uploadWorkerTask, heartbeatTimerCallback's FreeRTOS timer
//  context, loop() itself), and an unguarded interleaved write can corrupt
//  or split a line. No queued/async variant like cerberus's
//  debug_log_enqueue() -- nothing on hesperus is response-latency-sensitive
//  to a blocking Serial write the way cerberus's HTTP handler was.
// ----------------------------------------------------------------------------
#pragma once

#include <Arduino.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <stdarg.h>
#include <stdio.h>

inline SemaphoreHandle_t serial_write_mutex = xSemaphoreCreateMutex();

inline void serial_write_lock() {
  xSemaphoreTake(serial_write_mutex, portMAX_DELAY);
}
inline void serial_write_unlock() {
  xSemaphoreGive(serial_write_mutex);
}

inline uint64_t debug_timestamp_ms() {
  return esp_wifi_get_tsf_time(WIFI_IF_STA) / 1000;
}

template <typename T>
inline void debug_println(T value) {
  serial_write_lock();
  Serial.printf("[T=%llums] ", debug_timestamp_ms());
  Serial.println(value);
  serial_write_unlock();
}

inline void debug_printf(const char *fmt, ...) {
  char buf[160];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  serial_write_lock();
  Serial.printf("[T=%llums] ", debug_timestamp_ms());
  Serial.print(buf);
  serial_write_unlock();
}
