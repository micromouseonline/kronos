/**
 * @file debug-log.h (native test stub)
 * Minimal stand-in for src/debug-log.h so race-timer.h can compile on the
 * host -- the real header pulls in esp_wifi.h/FreeRTOS, neither mocked here.
 */
#pragma once

inline void debug_log_enqueue(const char *fmt, ...) {
  (void)fmt;
}
