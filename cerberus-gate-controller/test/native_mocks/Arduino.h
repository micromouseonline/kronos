/**
 * @file Arduino.h (native test stub)
 * Minimal stand-in for the real Arduino core so race-timer.h/stopwatch.h can
 * compile and run on the host. Only millis() is used by the code under test.
 */
#pragma once

#include <cstddef>
#include <cstdint>

inline uint32_t g_mock_millis = 0;

inline uint32_t millis() {
  return g_mock_millis;
}
