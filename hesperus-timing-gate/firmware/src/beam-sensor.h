// ----------------------------------------------------------------------------
//  beam-sensor.h — Dual-EMA reflective/occlusion beam-break detector.
//
//  Ported from legacy/gate-detector/gate-detector/gate-detector.ino's
//  ExpFilter/GateSensor classes (a fast/slow exponential-moving-average
//  ratio detector, not the FIR/IIR/closed-loop-PWM scheme described in this
//  project's own synchronous-reflective-detection.md, which is a different,
//  unrelated, unimplemented design). Two EMAs of the same raw ADC signal:
//  `fast` (tau ~2ms) tracks the instantaneous reading, `slow` (tau ~1s)
//  tracks the ambient baseline. A beam-break/occlusion pulls `fast` down
//  toward zero much faster than `slow` can follow, so the ratio
//  fast/slow crossing 0.25 (falling) / 0.75 (rising) gives a
//  self-normalizing trigger/re-arm pair with built-in hysteresis --
//  see legacy-evaluation.md for the derivation and known weaknesses of
//  this technique.
//
//  Three fixes are applied on top of the literal legacy algorithm, per
//  legacy-evaluation.md's own suggested-improvements list:
//   - Both EMAs are seeded from a burst of real ADC samples at startup
//     (beam_sensor_seed_from_adc()) instead of legacy's implicit start at
//     value=0, which caused a startup delay before readings were valid.
//   - The recovery clamp (slow = max(slow, fast)) runs unconditionally on
//     every update, not gated behind the dark-floor check -- legacy only
//     ran it once already above the floor, so it never applied during a
//     dark/occluded period and the detector could get stuck non-responsive
//     after a long occlusion.
//   - A confirm_count requires BEAM_CONFIRM_SAMPLES consecutive samples
//     past the trigger ratio before latching an interrupt, instead of
//     legacy's single-sample latch, to reject single-sample noise glitches.
//
//  alpha = 1/(F*tau) is derived from BEAM_SAMPLE_RATE_HZ rather than
//  hardcoded, per legacy's own portability note: if the actual sample rate
//  changes, alpha_fast/alpha_slow must be recalculated together to
//  preserve the same time constants -- here that recalculation happens
//  automatically.
// ----------------------------------------------------------------------------
#pragma once

#include <Arduino.h>

#include "debug-log.h" // debug_printf

constexpr uint32_t BEAM_SAMPLE_RATE_HZ = 1000; // see main.cpp's timer setup
constexpr float BEAM_TAU_FAST_S = 0.002f;      // ~2ms, tracks the raw signal
constexpr float BEAM_TAU_SLOW_S = 1.0f;        // ~1s, tracks the ambient baseline
constexpr float BEAM_ALPHA_FAST = 1.0f / (BEAM_SAMPLE_RATE_HZ * BEAM_TAU_FAST_S);
constexpr float BEAM_ALPHA_SLOW = 1.0f / (BEAM_SAMPLE_RATE_HZ * BEAM_TAU_SLOW_S);

// ~1% of 12-bit full scale (4095), rescaled from legacy's 10-count floor on
// a 10-bit (1023) ADC -- below this the sensor isn't seeing usable light,
// so the ratio logic is skipped rather than dividing by near-zero noise.
constexpr float BEAM_DARK_FLOOR_COUNTS = 40.0f;

// Consecutive samples past the 0.25x trigger ratio required before latching
// an interrupt (~3ms of added latency at BEAM_SAMPLE_RATE_HZ=1000) -- tune
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

    // Unconditional recovery clamp (fix #2, see header comment) -- must run
    // before the dark-floor check below, not after/inside it.
    slow.value = max(slow.value, fast.value);

    if (slow.value < BEAM_DARK_FLOOR_COUNTS) {
      // No usable signal -- skip the ratio logic exactly as legacy did.
      // Reset the confirmation counter so a stale near-miss from before the
      // dark period can't complete once light returns.
      confirm_count = 0;
      return false;
    }

    bool trigger_condition = fast.value < 0.25f * slow.value;
    bool rearm_condition = fast.value > 0.75f * slow.value;

    confirm_count = trigger_condition ? confirm_count + 1 : 0;

    if (rearm_condition) {
      armed = true;
      interrupted = false;
    }

    // Confirmation counter (fix #3, see header comment) -- require
    // BEAM_CONFIRM_SAMPLES consecutive samples past the trigger ratio
    // before latching, instead of legacy's single-sample trigger.
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

// Startup seeding (fix #1, see header comment): takes BEAM_SEED_SAMPLE_COUNT
// raw analogRead() samples on sensor.pin, averages them, and seeds both EMAs
// from that mean. Returns false if the mean is below BEAM_DARK_FLOOR_COUNTS
// -- a real, technician-actionable fault (sensor unlit/miswired/obstructed)
// the caller should surface, since a seed this dark means the ratio
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
