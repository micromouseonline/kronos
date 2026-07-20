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
#include <stdio.h>

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
  lv_label_set_text(objects.lbl_mouse_name, mouse_names[mouse_id % NUM_MICE]);
}

//============================================================================
// Shows the current mouse's own runs -- race_runs[mouse_first_run_index,
// race_run_count), which is at most MAX_RUNS_PER_MOUSE entries because runs
// for a given mouse are always contiguous.
inline void race_timer_render_run_times() {
  label_list_clear(run_times_buf, objects.lbl_run_time_list);
  for (size_t i = mouse_first_run_index; i < race_run_count; i++) {
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
inline void race_timer_display_init() {
  race_timer_init();
  label_list_clear(run_times_buf, objects.lbl_run_time_list);
}

// Called every loop() iteration regardless of events, so the Run Timer
// redraws live while RUNNING.
inline void race_timer_render() {
  race_timer_render_mouse_name();
  lv_label_set_text_fmt(objects.lbl_run_number, "%u", (unsigned)mouse_run_count);

  char buf[16];
  switch (race_timer_get_state()) {
    case RaceState::CALIBRATE:
      lv_label_set_text(objects.lbl_current_run_time, ".........");
      break;
    case RaceState::WAITING:
      lv_label_set_text(objects.lbl_current_run_time, "00:00:000");
      break;
    case RaceState::NEW_MOUSE:
      break;
    case RaceState::ARMED:
    case RaceState::RUNNING:
    case RaceState::GOAL:
      race_timer_format_time_seconds(entry_sw.time(), buf, sizeof(buf));
      lv_label_set_text(objects.lbl_time_remaining, buf);
      race_timer_format_time(run_sw.time(), buf, sizeof(buf));
      lv_label_set_text(objects.lbl_current_run_time, buf);
      break;
  }

  race_timer_render_run_times();
  race_timer_render_leaderboard();
}
