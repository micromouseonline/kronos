// ----------------------------------------------------------------------------
//  race-timer-display.h — Presentation layer for race-timer.h. Reads that
//  file's state/data via its getters and plain data functions and pushes it
//  to SCREEN_ID_MAIN's LVGL labels; race-timer.h itself has no lv_* calls or
//  ui/screens.h dependency, so this is the only place that translates race
//  data into what's on screen.
//
//  race_timer_render() is called every loop() iteration regardless of
//  events (see main.cpp), so all labels here are simply redrawn from current
//  state each tick rather than pushed incrementally from the state machine.
// ----------------------------------------------------------------------------
#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include <stdio.h>

#include "net/gate-liveness.h"
#include "race-timer.h"
#include "ui/screens.h"

/**
 * For the run times and the leaderboard, we maintain simple text buffers
 * and add lines to them as needed.
 * The text is cleared and recreated whenever a new time is recorded because
 * that is the simplest way to keep them up to date.
 *
 * The default panel size can only hold 5 lines of 16 characters but we
 * set aside more than than to allow for some flexibility
 */
struct LabelListBuffer {
  char text[160];
  size_t len = 0;
};

inline LabelListBuffer run_times_buf;
inline LabelListBuffer leaderboard_buf;

inline void label_list_clear(LabelListBuffer &buf, lv_obj_t *label) {
  buf.text[0] = '\0';
  buf.len = 0;
  lv_label_set_text(label, buf.text);
}

inline void label_list_append(LabelListBuffer &buf, lv_obj_t *label, const char *line) {
  int written = snprintf(buf.text + buf.len, sizeof(buf.text) - buf.len, "%s%s", buf.len ? "\n" : "", line);
  if (written > 0 && (size_t)written < sizeof(buf.text) - buf.len) {
    buf.len += (size_t)written;
  }
  lv_label_set_text(label, buf.text);
}

//============================================================================
inline void race_timer_render_mouse_name() {
  lv_label_set_text(objects.lbl_mouse_name, current_mouse_name);
}

//============================================================================
// Shows the current mouse's own runs -- race_runs[mouse_first_run_index,
// race_run_count), capped at this mouse's allowed run count
// (race_timer_allowed_runs()). ARM is no longer blocked once that count is
// used up (race_timer_try_arm(), race-timer.h), so a mouse can rack up more
// runs than the limit; those still land in race_runs[] (and so still reach
// the leaderboard and host telemetry), they just don't get listed here --
// only the official first N do.
inline void race_timer_render_run_times() {
  label_list_clear(run_times_buf, objects.lbl_run_time_list);
  long allowed = race_timer_allowed_runs();
  for (size_t i = mouse_first_run_index; i < race_run_count; i++) {
    if ((long)race_runs[i].run_number > allowed) {
      break;
    }
    char time_str[16];
    race_timer_format_time(race_runs[i].time_ms, time_str, sizeof(time_str));
    char line[32];
    snprintf(line, sizeof(line), "%u: %s", (unsigned)race_runs[i].run_number, time_str);
    label_list_append(run_times_buf, objects.lbl_run_time_list, line);
  }
}

//============================================================================
// Shows the top TOP_N mice by best time, computed by race-timer.h's
// race_timer_compute_leaderboard().
inline void race_timer_render_leaderboard() {
  static constexpr size_t TOP_N = 5;

  LeaderboardEntry entries[MAX_RESULTS];
  size_t count = race_timer_compute_leaderboard(entries, MAX_RESULTS);

  label_list_clear(leaderboard_buf, objects.lbl_leaderboard_list);
  size_t shown = count < TOP_N ? count : TOP_N;
  for (size_t i = 0; i < shown; i++) {
    char time_str[16];
    race_timer_format_time(entries[i].best_time_ms, time_str, sizeof(time_str));
    char line[40];
    snprintf(line, sizeof(line), "%u: %s", (unsigned)(i + 1), time_str);
    label_list_append(leaderboard_buf, objects.lbl_leaderboard_list, line);
  }
}

//============================================================================
// No default: case on purpose (same convention as race-serial-telemetry.h's
// race_state_to_legacy_code()), so -Wswitch warns if a new RaceState is
// ever added without updating this. TIMED_OUT is declared in race-timer.h
// but nothing currently sets race_state to it -- handled here only for
// switch-exhaustiveness.
inline const char *race_state_abbrev(RaceState state) {
  switch (state) {
    case RaceState::CALIBRATE:
      return "CAL";
    case RaceState::NEW_MOUSE:
      return "NEW";
    case RaceState::WAITING:
      return "WAIT";
    case RaceState::ARMED:
      return "ARMED";
    case RaceState::RUNNING:
      return "RUN";
    case RaceState::GOAL:
      return "GOAL";
    case RaceState::TIMED_OUT:
      return "TOUT";
  }
  return "";
}

// Status strip along the bottom of SCREEN_ID_MAIN, where the four touch
// buttons used to be. Three zones: race state, WiFi link, and a reserved
// (currently unused) gate-status placeholder -- see lbl_status_gates below.
inline void race_timer_render_status_bar() {
  lv_label_set_text(objects.lbl_status_state, race_state_abbrev(race_timer_get_state()));

  if (WiFi.status() == WL_CONNECTED) {
    lv_label_set_text_fmt(objects.lbl_status_wifi, LV_SYMBOL_WIFI " %ddBm", (int)WiFi.RSSI());
    lv_obj_set_style_text_color(objects.lbl_status_wifi, lv_color_hex(0xadff2f), LV_PART_MAIN | LV_STATE_DEFAULT);
  } else {
    lv_label_set_text(objects.lbl_status_wifi, LV_SYMBOL_CLOSE " connecting");
    lv_obj_set_style_text_color(objects.lbl_status_wifi, lv_color_hex(0xff3b30), LV_PART_MAIN | LV_STATE_DEFAULT);
  }

  // Per-role gate connection state (see net/gate-liveness.h) -- green "S"/
  // "G" when that gate's WS link is up, red when not. Recolor markup
  // (enabled in race_timer_display_init()) renders just "S G"; the "#RRGGBB
  // text#" runs themselves aren't visible glyphs.
  lv_label_set_text_fmt(objects.lbl_status_gates, "#%s S# #%s G#",
                         g_gate_link[(int)GateRole::START].connected ? "adff2f" : "ff3b30",
                         g_gate_link[(int)GateRole::GOAL].connected ? "adff2f" : "ff3b30");
}

//============================================================================
inline void race_timer_display_init() {
  race_timer_init();
  label_list_clear(run_times_buf, objects.lbl_run_time_list);
  // The EEZ Studio layout doesn't center this label within pnl_run_number
  // on its own (screens.c only sets its text alignment, not its object
  // position) -- set at runtime rather than hand-editing the generated
  // screens.c, which would be overwritten on the next EEZ Studio export.
  lv_obj_align(objects.lbl_run_number, LV_ALIGN_CENTER, 0, 0);
  // EEZ Studio's editor doesn't expose the label recolor flag -- enabled
  // here at runtime, same reasoning as the alignment fix just above, so
  // race_timer_render_status_bar()'s "#RRGGBB text#" markup for
  // lbl_status_gates actually renders as colored text.
  lv_label_set_recolor(objects.lbl_status_gates, true);
}

// Called every loop() iteration regardless of events, so the Run Timer
// redraws live while RUNNING.
inline void race_timer_render() {
  race_timer_render_mouse_name();
  race_timer_render_status_bar();
  // "current/max" -- e.g. "1/8" -- max is the host's AllowedRuns override
  // when set, else the default MAX_RUNS_PER_MOUSE (race_timer_allowed_runs()
  // in race-timer.h). Redrawn every tick like every other label here, so it
  // updates the instant either number changes.
  lv_label_set_text_fmt(objects.lbl_run_number, "%u/%u", (unsigned)mouse_run_count, (unsigned)race_timer_allowed_runs());

  char buf[16];
  switch (race_timer_get_state()) {
    case RaceState::CALIBRATE:
      lv_label_set_text(objects.lbl_current_run_time, ".........");
      // docs/RACE-STATE-MACHINE.md: "Entry Time shows 00:00" while
      // Calibrating -- a fixed idle value, not whatever EntryTimeS
      // countdown persisted from the last mouse. Overrides the
      // unconditional countdown block below, which explicitly skips
      // CALIBRATE for this reason.
      lv_label_set_text(objects.lbl_time_remaining, "00:00");
      lv_obj_set_style_text_color(objects.lbl_time_remaining, lv_color_hex(0xadff2f), LV_PART_MAIN | LV_STATE_DEFAULT);
      break;
    case RaceState::WAITING:
      lv_label_set_text(objects.lbl_current_run_time, "00:00:000");
      break;
    case RaceState::NEW_MOUSE:
      break;
    case RaceState::ARMED:
    case RaceState::RUNNING:
    case RaceState::GOAL:
      if (g_entry_time_s_limit < 0) {
        // No host-supplied EntryTimeS -- original raw-elapsed display.
        // (When a limit IS set, the countdown below runs unconditionally
        // regardless of race_state, so it's intentionally not duplicated
        // here.)
        race_timer_format_time_seconds(entry_sw.time(), buf, sizeof(buf));
        lv_label_set_text(objects.lbl_time_remaining, buf);
      }
      // GOAL shows the exact committed run time (race_timer_last_run_time_ms(),
      // the same number recorded to the run list/leaderboard), not run_sw.time()
      // -- run_sw is receipt-time based and can differ from the tsf-exact
      // committed value by a little return-leg network jitter. ARMED/RUNNING
      // have no committed value yet, so they still show the live run_sw reading.
      if (race_timer_get_state() == RaceState::GOAL) {
        race_timer_format_time(race_timer_last_run_time_ms(), buf, sizeof(buf));
      } else {
        race_timer_format_time(run_sw.time(), buf, sizeof(buf));
      }
      lv_label_set_text(objects.lbl_current_run_time, buf);
      break;
  }

  // EntryTimeS countdown: shown whenever the host has set a limit, in
  // every state EXCEPT CALIBRATE (which forces a fixed "00:00" above --
  // docs/RACE-STATE-MACHINE.md). It reads as the full starting duration,
  // unchanging, until entry_sw actually starts on the mouse's first ARM
  // (race_timer_try_arm()) -- not at NewMouse itself -- then counts down
  // live, and turns red once it hits zero (track-and-display only:
  // nothing forces a state change at zero; countdown stops/freezes at zero).
  if (race_timer_get_state() != RaceState::CALIBRATE) {
    if (g_entry_time_s_limit >= 0) {
      uint32_t remaining_ms = race_timer_entry_time_remaining_ms();
      race_timer_format_time_seconds(remaining_ms, buf, sizeof(buf));
      lv_label_set_text(objects.lbl_time_remaining, buf);
      bool expired = (remaining_ms == 0);
      lv_obj_set_style_text_color(objects.lbl_time_remaining, expired ? lv_color_hex(0xff0000) : lv_color_hex(0xadff2f), LV_PART_MAIN | LV_STATE_DEFAULT);
    } else {
      // No limit set -- make sure the label isn't left red from a
      // previous mouse's expired countdown (screens.c's original colour).
      lv_obj_set_style_text_color(objects.lbl_time_remaining, lv_color_hex(0xadff2f), LV_PART_MAIN | LV_STATE_DEFAULT);
    }
  }

  // docs/RACE-STATE-MACHINE.md: "Mouse Name, Run Times blank" while
  // Calibrating -- current_mouse_name is already cleared by
  // ENTER_CALIBRATION (race-timer.h), but the run-times list is display
  // state only, so it's blanked here rather than mutating
  // mouse_first_run_index at the model layer (which would lose track of
  // this mouse's earlier runs for a later RESUME_TIMER).
  if (race_timer_get_state() == RaceState::CALIBRATE) {
    label_list_clear(run_times_buf, objects.lbl_run_time_list);
  } else {
    race_timer_render_run_times();
  }
  race_timer_render_leaderboard();
}
