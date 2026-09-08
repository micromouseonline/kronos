// ----------------------------------------------------------------------------
//  beam-sensor.h — Dual-EMA beam-break detector.
//
//  Two EMAs of the same raw ADC signal: `fast` (tau ~2ms) tracks the
//  instantaneous reading, `slow` (tau ~1s) tracks the ambient+emitter
//  baseline. A beam-break/occlusion pulls `fast` down toward the ambient-
//  only level much faster than `slow` can follow, so `slow - fast` crossing
//  BEAM_TRIGGER_DROP_COUNTS (falling) / BEAM_REARM_DROP_COUNTS (rising)
//  gives a trigger/re-arm pair with built-in hysteresis. This is an
//  absolute count drop, not a ratio -- it specifically measures the
//  emitter's own contribution being lost, which stays meaningful regardless
//  of the ambient level (a fast/slow *ratio* doesn't: strong ambient can
//  both mask a real occlusion and make a depressed baseline easy to false-
//  trigger on). See detection-mechanism.md for the full writeup.
//
//  Both EMAs are seeded from a burst of real ADC samples at startup
//  (beam_sensor_seed_from_adc()) so readings are valid immediately rather
//  than ramping up from a zeroed start.
//
//  The recovery clamp (slow = max(slow, fast)) runs only while !armed --
//  i.e. only while genuinely recovering from a latched trigger/occlusion,
//  not during ordinary armed operation -- so it still lets `slow` snap back
//  up quickly once light returns after a long occlusion, without letting a
//  brief bright external light source (e.g. a passing robot's own
//  reflected-light sensor) drag `slow` up and cause a false trigger once
//  that source's own pulse ends. It's a no-op during a real occlusion
//  either way, since `fast` is low there and max(slow, fast) doesn't pull
//  slow down.
//
//  A confirm_count requires BEAM_CONFIRM_SAMPLES consecutive samples past
//  the trigger ratio before latching an interrupt, to reject single-sample
//  noise glitches.
//
//  alpha = 1/(F*tau) is derived from BEAM_SAMPLE_RATE_HZ rather than
//  hardcoded, so if the sample rate changes, alpha_fast/alpha_slow are
//  recalculated together automatically, preserving the same time constants.
// ----------------------------------------------------------------------------
#pragma once

#include <Arduino.h>

#include "debug-log.h" // debug_printf

constexpr uint32_t BEAM_SAMPLE_RATE_HZ = 1000; // see main.cpp's timer setup
constexpr float BEAM_TAU_FAST_S = 0.002f;      // ~2ms, tracks the raw signal
constexpr float BEAM_TAU_SLOW_S = 1.0f;        // ~1s, tracks the ambient baseline
constexpr float BEAM_ALPHA_FAST = 1.0f / (BEAM_SAMPLE_RATE_HZ * BEAM_TAU_FAST_S);
constexpr float BEAM_ALPHA_SLOW = 1.0f / (BEAM_SAMPLE_RATE_HZ * BEAM_TAU_SLOW_S);

// ~1% of 12-bit full scale (4095) -- below this the sensor isn't seeing
// usable light at all, so the trigger logic is skipped entirely.
constexpr float BEAM_DARK_FLOOR_COUNTS = 40.0f;

// Minimum drop of `fast` below `slow` (in raw ADC counts) required to latch
// a trigger -- an absolute measure of the emitter's own light being lost,
// not a fast/slow ratio. Bench data shows a properly aimed pair contributes
// at least ~400-500 counts to `slow` once both emitters are on (see
// test-data.md, case C); this sits with margin below that minimum, well
// above what ambient/external-illumination noise alone plausibly moves in
// ~2ms (BEAM_TAU_FAST_S). An absolute threshold also avoids a failure mode
// the old 0.25x ratio had: under strong ambient (test-data.md case D, where
// incandescent ambient reached ~69% of the START channel's own emitter
// contribution), the ratio could fail to cross 0.25 on a *real* occlusion
// at all -- a missed trigger, not just a false one.
constexpr float BEAM_TRIGGER_DROP_COUNTS = 300.0f;

// Re-arm once the drop has recovered back below this -- well under
// BEAM_TRIGGER_DROP_COUNTS so the gap gives the same kind of hysteresis the
// old 0.25/0.75 ratio band did, without which noise sitting right at the
// trigger threshold could chatter.
constexpr float BEAM_REARM_DROP_COUNTS = 100.0f;

// Consecutive samples past the trigger drop required before latching an
// interrupt (~3ms of added latency at BEAM_SAMPLE_RATE_HZ=1000) -- tune
// against real sensor noise on the bench.
constexpr uint8_t BEAM_CONFIRM_SAMPLES = 3;

// Raw ADC samples averaged to seed each channel's filters at startup.
constexpr uint32_t BEAM_SEED_SAMPLE_COUNT = 64;

// Streams raw/fast/slow values for both channels to Serial for bench
// tuning -- off by default, matching WS_EVENT_LOG_DETAIL's convention in
// debug-log.h (pass -D BEAM_SENSOR_STREAM_DEBUG=1 via build_flags for a
// bench-tuning session and rebuild).
#ifndef BEAM_SENSOR_STREAM_DEBUG
#define BEAM_SENSOR_STREAM_DEBUG 0
#endif

#if BEAM_SENSOR_STREAM_DEBUG
// Rate-limited to this many samples between prints (50 samples @
// BEAM_SAMPLE_RATE_HZ=1000 -> 20Hz) so the stream stays well under the
// serial link's bandwidth instead of printing every single sample.
constexpr uint32_t BEAM_DEBUG_STREAM_INTERVAL_SAMPLES = 50;
#endif

struct ExpFilter {
  float alpha = 1.0f;
  float value = 0.0f;

  float update(float new_value) {
    value += alpha * (new_value - value);
    return value;
  }
};

struct BeamSensor {
  const char *name; // for logging, e.g. "ARM" / "START"
  int pin;
  ExpFilter fast{BEAM_ALPHA_FAST};
  ExpFilter slow{BEAM_ALPHA_SLOW};
  uint8_t confirm_count = 0;
  bool interrupted = false;
  bool armed = true;

  void seed(float initial_value) {
    fast.value = initial_value;
    slow.value = initial_value;
  }

  // One call per sample. Returns true exactly on the sample that latches a
  // new trigger -- the caller should dispatch a trigger event once, on
  // that edge, and nothing else.
  bool update(uint16_t raw_adc) {
    fast.update(raw_adc);
    slow.update(raw_adc);

    // Recovery clamp -- only while recovering from a latched trigger
    // (!armed), not during ordinary armed operation. Gating on !armed
    // keeps this inert during normal operation, so a brief bright external
    // light source (e.g. a passing robot's own reflected-light sensor)
    // can't drag `slow` up and cause a false trigger once that source's
    // pulse ends -- see beam-sensor.h's header comment. Must still run
    // before the dark-floor check below, not after/inside it, so it
    // continues to apply throughout a dark/occluded period.
    if (!armed) {
      slow.value = max(slow.value, fast.value);
    }

    if (slow.value < BEAM_DARK_FLOOR_COUNTS) {
      // No usable signal -- skip the trigger logic. Reset the confirmation
      // counter so a stale near-miss from before the dark period can't
      // complete once light returns.
      confirm_count = 0;
      return false;
    }

    float drop = slow.value - fast.value;
    bool trigger_condition = drop >= BEAM_TRIGGER_DROP_COUNTS;
    bool rearm_condition = drop <= BEAM_REARM_DROP_COUNTS;

    confirm_count = trigger_condition ? confirm_count + 1 : 0;

    if (rearm_condition) {
      armed = true;
      interrupted = false;
    }

    // Require BEAM_CONFIRM_SAMPLES consecutive samples past the trigger
    // drop before latching, to reject single-sample noise glitches.
    if (!interrupted && armed && confirm_count >= BEAM_CONFIRM_SAMPLES) {
      interrupted = true;
      armed = false;
      return true;
    }
    return false;
  }
};

#if BEAM_SENSOR_STREAM_DEBUG
// Prints one line of "label:value" pairs (Arduino IDE Serial Plotter
// format) per BEAM_DEBUG_STREAM_INTERVAL_SAMPLES samples, e.g.
// "armRaw:1820 armFast:1815.2 armSlow:1818.9 startRaw:1790 ...". Bypasses
// debug_printf's "[T=...]" timestamp prefix (serial_write_lock/unlock is
// reused directly instead) since that text breaks the plotter's per-line
// numeric parsing.
inline void beam_sensor_stream_debug(uint16_t arm_raw,
                                      const BeamSensor &arm_sensor,
                                      uint16_t start_raw,
                                      const BeamSensor &start_sensor) {
  static uint32_t sample_count = 0;
  if (++sample_count % BEAM_DEBUG_STREAM_INTERVAL_SAMPLES != 0) {
    return;
  }
  serial_write_lock();
  Serial.printf(
      "armRaw:%u armFast:%.1f armSlow:%.1f startRaw:%u startFast:%.1f "
      "startSlow:%.1f\n",
      arm_raw, arm_sensor.fast.value, arm_sensor.slow.value, start_raw,
      start_sensor.fast.value, start_sensor.slow.value);
  serial_write_unlock();
}
#endif

// Startup seeding: takes BEAM_SEED_SAMPLE_COUNT raw analogRead() samples on
// sensor.pin, averages them, and seeds both EMAs from that mean. Returns
// false if the mean is below BEAM_DARK_FLOOR_COUNTS
// -- a real, technician-actionable fault (sensor unlit/miswired/obstructed)
// the caller should surface, since a seed this dark means the trigger
// detection logic will not function at all until it changes. Must be called
// after analogReadResolution()/analogSetPinAttenuation() have configured
// sensor.pin.
inline bool beam_sensor_seed_from_adc(BeamSensor &sensor) {
  uint32_t sum = 0;
  for (uint32_t i = 0; i < BEAM_SEED_SAMPLE_COUNT; i++) {
    sum += analogRead(sensor.pin);
    delayMicroseconds(200);
  }
  float mean = static_cast<float>(sum) / BEAM_SEED_SAMPLE_COUNT;
  sensor.seed(mean);
  bool ok = mean >= BEAM_DARK_FLOOR_COUNTS;
  debug_printf("[BEAM] %s seeded mean=%.0f (%s)\n", sensor.name, mean,
               ok ? "ok" : "BELOW DARK FLOOR");
  return ok;
}
