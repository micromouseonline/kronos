// ----------------------------------------------------------------------------
//  race-serial-telemetry.h — TX side of the legacy <type,value> host
//  protocol (net/messages.h): mirrors race_state and run/course timing to
//  the host. Driven by comparing race_state/mouse_id against their
//  last-seen values each loop() tick -- the same edge-detection trick works
//  uniformly regardless of whether a transition came from a button, Serial,
//  or HTTP, since all three funnel through race_timer_handle_command()
//  earlier in the same loop() iteration, before this runs. RX side is
//  net/serial-protocol.h.
// ----------------------------------------------------------------------------
#pragma once

#include <Arduino.h>

#include "net/messages.h"
#include "race-timer.h"

// Manual-testing aid only, NOT part of the wire protocol -- toggle off to
// silence the once-a-second MSG_WATCHDOG line while eyeballing a serial
// terminal by hand (see docs/TESTING-SERIAL.md). Leave on (the default)
// whenever real RATS host software is attached: it expects a watchdog at
// least every 2s and reports a timing-system fault if it stops.
inline bool g_watchdog_tx_enabled = true;

// Explicit remap, not a cast -- the legacy MSG_CURRENT_STATE numbering (see
// net/messages.h's doc comment) predates and doesn't match today's
// RaceState enum order. No default: case inside the switch on purpose, so
// -Wswitch warns if a new RaceState is ever added without updating this;
// the trailing return is just to satisfy control-flow analysis.
inline int race_state_to_legacy_code(RaceState state) {
  switch (state) {
    case RaceState::CALIBRATE:
      return 0;
    case RaceState::WAITING:
      return 1;
    case RaceState::ARMED:
      return 2;
    case RaceState::RUNNING:
      return 4;  // legacy STARTING(3) has no equivalent state today
    case RaceState::GOAL:
      return 5;
    case RaceState::NEW_MOUSE:
      return 6;  // transient -- race-timer.h's own comment says it's rarely, if ever, observed
    case RaceState::TIMED_OUT:
      return 4;  // no legacy equivalent -- RUNNING is the closest semantic match
  }
  return 4;
}

inline void race_serial_telemetry_tick() {
  static RaceState last_state = race_state;  // matches boot value: no spurious first-tick fire
  static uint16_t last_mouse_id = mouse_id;
  static uint32_t last_watchdog_ms = 0;
  static unsigned long watchdog_counter = 0;

  RaceState previous_state = last_state;

  // mouse_id only changes inside race_timer_enter_new_mouse() -- an
  // unambiguous "genuinely new mouse" signal, unlike "race_state became
  // WAITING", which is also reached when an exhausted mouse's ARM bounces
  // back to WAITING (race_timer_try_arm()) without any new mouse involved.
  if (mouse_id != last_mouse_id) {
    last_mouse_id = mouse_id;
    serial_send_message(MSG_COURSE_TIME_MS, 0);
  }

  if (race_state != last_state) {
    last_state = race_state;
    serial_send_message(MSG_CURRENT_STATE, race_state_to_legacy_code(race_state));

    if (previous_state == RaceState::ARMED && race_state == RaceState::RUNNING) {
      serial_send_message(MSG_C1_SPLIT_TIME, 0);
    } else if (previous_state == RaceState::RUNNING && race_state == RaceState::GOAL) {
      // Read the just-committed value, not run_sw.time() directly -- avoids
      // any ordering ambiguity between commit and telemetry.
      unsigned long time_ms = (race_run_count > 0) ? race_runs[race_run_count - 1].time_ms : 0;
      serial_send_run_time(time_ms);
    }
  }

  uint32_t now = millis();
  if (g_watchdog_tx_enabled && now - last_watchdog_ms >= 1000) {
    last_watchdog_ms = now;
    serial_send_message(MSG_WATCHDOG, watchdog_counter++);
  }
}
