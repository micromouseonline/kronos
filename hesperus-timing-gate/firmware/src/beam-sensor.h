// ----------------------------------------------------------------------------
//  beam-sensor.h — Dual-EMA beam-break detector.
//
//  Two EMAs of the same raw ADC signal: `fast` (tau ~2ms) tracks the
//  instantaneous reading, `slow` (tau ~1s) tracks the ambient+emitter
//  baseline. A beam-break/occlusion pulls `fast` down toward the ambient-
//  only level much faster than `slow` can follow, so the drop below `slow`
//  crossing BEAM_TRIGGER_DROP_COUNTS (falling, judged on raw) /
//  BEAM_REARM_DROP_COUNTS (rising, judged on `fast`) gives a trigger/re-arm
//  pair with built-in hysteresis. This is an
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
//  `slow` is fed from a short running median of raw (`sustained`,
//  BEAM_SUSTAIN_WINDOW_SAMPLES wide) rather than raw itself, so brief
//  interference spikes -- however large -- don't move the baseline.
//
//  The recovery clamp (slow = max(slow, sustained)) runs only while !armed
//  -- i.e. only while genuinely recovering from a latched trigger/occlusion,
//  not during ordinary armed operation -- so it still lets `slow` snap back
//  up quickly once light returns after a long occlusion, without letting a
//  brief bright external light source (e.g. a passing robot's own
//  reflected-light sensor) drag `slow` up and cause a false trigger once
//  that source's own pulse ends. Clamping to the median rather than `fast`
//  is what stops such a spike doing exactly that in the !armed window just
//  after a trigger. It's a no-op during a real occlusion either way, since
//  the median is low there and doesn't pull slow down.
//
//  A confirm_count requires BEAM_CONFIRM_SAMPLES consecutive *raw* samples
//  past the trigger drop before latching an interrupt, so a dip shorter
//  than that can't trigger however deep it is. It counts raw, not `fast`:
//  `fast` takes several samples to recover from a short dip, and that
//  recovery tail alone used to satisfy the count.
//
//  alpha = 1/(F*tau) is derived from BEAM_SAMPLE_RATE_HZ rather than
//  hardcoded, so if the sample rate changes, alpha_fast/alpha_slow are
//  recalculated together automatically, preserving the same time constants.
// ----------------------------------------------------------------------------
#pragma once

#include <Arduino.h>

#include <atomic>

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
// at all -- a missed trigger, not just a false one. Held at 70 (down from an
// original 300) because some sensor assemblies have proven insensitive, with
// stable emitter contributions of only ~100 counts -- raise it again if
// assembly sensitivity improves.
constexpr float BEAM_TRIGGER_DROP_COUNTS = 70.0f;

// Re-arm once the drop has recovered back below this -- well under
// BEAM_TRIGGER_DROP_COUNTS so the gap gives the same kind of hysteresis the
// old 0.25/0.75 ratio band did, without which noise sitting right at the
// trigger threshold could chatter. An intact beam sits at drop ~0, so this
// can be low without delaying re-arm.
constexpr float BEAM_REARM_DROP_COUNTS = 25.0f;
static_assert(BEAM_REARM_DROP_COUNTS < BEAM_TRIGGER_DROP_COUNTS,
              "re-arm drop must sit below trigger drop, or a drop between "
              "the two re-arms and re-triggers every few samples");

// Window (samples) of the running median that feeds `slow` and the recovery
// clamp -- see BeamSensor::update(). Odd, so the median is a real sample.
// 15 samples = 15ms at BEAM_SAMPLE_RATE_HZ=1000: an interference spike
// shorter than ~7ms (half the window) can't move the median at all, while
// light that has genuinely returned moves it within ~8ms. Must stay well
// under the shortest real occlusion to catch; scale with the sample rate.
constexpr uint8_t BEAM_SUSTAIN_WINDOW_SAMPLES = 15;
static_assert(BEAM_SUSTAIN_WINDOW_SAMPLES % 2 == 1,
              "median window must be odd");

// Consecutive raw samples past the trigger drop required before latching an
// interrupt (~4ms of added latency at BEAM_SAMPLE_RATE_HZ=1000). A count,
// not a time -- rescale it if the sample rate changes. Sized against the
// shortest real occlusion: a 12mm object at 2m/s blocks the beam for 6ms,
// i.e. at most 6 samples, fewer once the partially-blocked edge samples and
// any late sample tick are taken off -- so this must stay well under 6.
constexpr uint8_t BEAM_CONFIRM_SAMPLES = 4;

// Raw ADC samples averaged to seed each channel's filters at startup.
constexpr uint32_t BEAM_SEED_SAMPLE_COUNT = 64;

// Streams raw/fast/slow values for both channels to Serial for bench
// tuning. Switched at runtime by the serial `stream [on|off]` command
// (provisioning-commands.h); BEAM_SENSOR_STREAM_DEBUG only sets its state
// at boot -- off by default, matching WS_EVENT_LOG_DETAIL's convention in
// debug-log.h (pass -D BEAM_SENSOR_STREAM_DEBUG=1 via build_flags to have
// it start on).
#ifndef BEAM_SENSOR_STREAM_DEBUG
#define BEAM_SENSOR_STREAM_DEBUG 0
#endif

// Written by the CLI (loop task), read by beamSampleTask -- a plain bool
// flag, so std::atomic is enough; no ordering with other data is needed.
inline std::atomic<bool> g_beam_stream_enabled{BEAM_SENSOR_STREAM_DEBUG != 0};

// Rate-limited to this many samples between prints (50 samples @
// BEAM_SAMPLE_RATE_HZ=1000 -> 20Hz) so the stream stays well under the
// serial link's bandwidth instead of printing every single sample.
constexpr uint32_t BEAM_DEBUG_STREAM_INTERVAL_SAMPLES = 50;

struct ExpFilter {
  float alpha = 1.0f;
  float value = 0.0f;

  float update(float new_value) {
    value += alpha * (new_value - value);
    return value;
  }
};

// Snapshot taken on the sample that latches a trigger and sent to cerberus
// with the event, so an unexplained trigger can be classified afterwards
// from the host's log (see detection-mechanism.md, "Trigger diagnostics").
struct BeamTriggerDiag {
  // The median window at the trigger, oldest first: the last
  // BEAM_CONFIRM_SAMPLES entries are the low samples that confirmed it,
  // the rest are the lead-in.
  uint16_t raw[BEAM_SUSTAIN_WINDOW_SAMPLES];
  uint16_t slow;      // this channel's baseline at the trigger
  int16_t other_drop; // the other channel's slow - raw on the same sample
};

struct BeamSensor {
  const char *name; // for logging, e.g. "ARM" / "START"
  int pin;
  ExpFilter fast{BEAM_ALPHA_FAST};
  ExpFilter slow{BEAM_ALPHA_SLOW};
  uint16_t history[BEAM_SUSTAIN_WINDOW_SAMPLES] = {}; // ring buffer of raw
  uint8_t history_pos = 0;
  float sustained = 0.0f; // running median of `history`
  float slow_at_trigger = 0.0f; // caps the recovery clamp while !armed
  uint8_t confirm_count = 0;
  bool interrupted = false;
  bool armed = true;

  void seed(float initial_value) {
    fast.value = initial_value;
    slow.value = initial_value;
    sustained = initial_value;
    for (uint16_t &h : history) {
      h = static_cast<uint16_t>(initial_value);
    }
  }

  // Pushes raw_adc into the ring buffer and returns the median of the
  // window. Insertion sort of a 15-element copy -- trivial at 1kHz.
  float update_sustained(uint16_t raw_adc) {
    history[history_pos] = raw_adc;
    history_pos = (history_pos + 1) % BEAM_SUSTAIN_WINDOW_SAMPLES;
    uint16_t sorted[BEAM_SUSTAIN_WINDOW_SAMPLES];
    for (uint8_t i = 0; i < BEAM_SUSTAIN_WINDOW_SAMPLES; i++) {
      uint16_t v = history[i];
      uint8_t j = i;
      for (; j > 0 && sorted[j - 1] > v; j--) {
        sorted[j] = sorted[j - 1];
      }
      sorted[j] = v;
    }
    sustained = sorted[BEAM_SUSTAIN_WINDOW_SAMPLES / 2];
    return sustained;
  }

  // Fills `diag` from this channel's current state; call on the sample
  // update() returned true for. `other`/`other_raw` are the other channel
  // and its raw reading from the same sample tick.
  void snapshot(BeamTriggerDiag &diag, const BeamSensor &other,
                uint16_t other_raw) const {
    for (uint8_t i = 0; i < BEAM_SUSTAIN_WINDOW_SAMPLES; i++) {
      // history_pos is the next slot to overwrite, i.e. the oldest sample.
      diag.raw[i] = history[(history_pos + i) % BEAM_SUSTAIN_WINDOW_SAMPLES];
    }
    diag.slow = static_cast<uint16_t>(slow.value);
    diag.other_drop = static_cast<int16_t>(other.slow.value - other_raw);
  }

  // One call per sample. Returns true exactly on the sample that latches a
  // new trigger -- the caller should dispatch a trigger event once, on
  // that edge, and nothing else.
  bool update(uint16_t raw_adc) {
    fast.update(raw_adc);
    // `slow` follows the running median, not raw: brief interference
    // spikes (shorter than half BEAM_SUSTAIN_WINDOW_SAMPLES) then can't
    // lift the baseline at all, so the gaps between them can't read as a
    // drop. Their energy only lands on `fast`, which shrinks `drop`.
    slow.update(update_sustained(raw_adc));

    // Recovery clamp -- only while recovering from a latched trigger
    // (!armed), not during ordinary armed operation. It pulls `slow` back
    // toward the running median (not `fast`), and never above
    // slow_at_trigger -- its job is only to restore the pre-occlusion
    // baseline once light returns, not to track anything new. Clamping to
    // `fast` let a single large interference spike arriving while !armed
    // copy its full height straight into `slow`, after which every gap
    // between spikes read as a huge drop -- a self-sustaining false-trigger
    // storm (see detection-mechanism.md); the cap also covers dense spikes
    // that do manage to lift the median. Must still run before the
    // dark-floor check below, not after/inside it, so it continues to apply
    // throughout a dark/occluded period.
    if (!armed) {
      slow.value = max(slow.value, min(sustained, slow_at_trigger));
    }

    if (slow.value < BEAM_DARK_FLOOR_COUNTS) {
      // No usable signal -- skip the trigger logic. Reset the confirmation
      // counter so a stale near-miss from before the dark period can't
      // complete once light returns.
      confirm_count = 0;
      return false;
    }

    float drop = slow.value - fast.value;
    // Judged on raw, not `fast` -- see BEAM_CONFIRM_SAMPLES. `fast` is
    // still what re-arm watches, below.
    bool trigger_condition = slow.value - raw_adc >= BEAM_TRIGGER_DROP_COUNTS;
    // Re-arm needs the median to have recovered as well as `fast`: an
    // interference spike during a real occlusion can briefly lift `fast`
    // back near `slow`, and re-arming on that alone would let the occlusion
    // trigger a second time. `fast` alone still matters too -- just after
    // a trigger the median hasn't dropped yet.
    bool rearm_condition = drop <= BEAM_REARM_DROP_COUNTS &&
                           slow.value - sustained <= BEAM_REARM_DROP_COUNTS;

    confirm_count = trigger_condition ? confirm_count + 1 : 0;

    if (rearm_condition) {
      armed = true;
      interrupted = false;
    }

    // Require BEAM_CONFIRM_SAMPLES consecutive raw samples past the trigger
    // drop before latching, to reject dips shorter than that.
    if (!interrupted && armed && confirm_count >= BEAM_CONFIRM_SAMPLES) {
      interrupted = true;
      armed = false;
      slow_at_trigger = slow.value;
      return true;
    }
    return false;
  }
};

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
  if (!g_beam_stream_enabled.load(std::memory_order_relaxed)) {
    return;
  }
  if (++sample_count % BEAM_DEBUG_STREAM_INTERVAL_SAMPLES != 0) {
    return;
  }
  serial_write_lock();
  Serial.printf(
      "armRaw:%u armFast:%.1f armSlow:%.1f armSust:%.0f startRaw:%u "
      "startFast:%.1f startSlow:%.1f startSust:%.0f\n",
      arm_raw, arm_sensor.fast.value, arm_sensor.slow.value,
      arm_sensor.sustained, start_raw, start_sensor.fast.value,
      start_sensor.slow.value, start_sensor.sustained);
  serial_write_unlock();
}

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
