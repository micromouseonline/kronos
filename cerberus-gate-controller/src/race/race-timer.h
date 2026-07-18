// ----------------------------------------------------------------------------
//  race-timer.h — Race timing state machine (docs/maze-timer-state-machine.md,
//  workspace root). Knows nothing about input hardware -- it only consumes
//  RaceEvents (whatever produces them: local buttons today, Serial/HTTP
//  later -- see main.cpp's input_event_handler() for the current
//  ButtonID -> RaceEvent mapping), drives a Stopwatch-based Run Timer, and
//  pushes results into the run-times / leaderboard labels on SCREEN_ID_MAIN.
//
//  States: CALIBRATE, NEW_MOUSE, WAITING, ARMED, RUNNING, GOAL. CALIBRATE
//  here is this race state machine's boot state and is unrelated to
//  display/touch-calibration.h's screen-touch calibration wizard.
//
//  Scope: EV_RESTART (the doc's global "any state" override) has no
//  producer yet -- included for forward compatibility with the future
//  Serial/HTTP phase, currently a dead path. Session Countdown Timer,
//  MAINTENANCE, and persistent mouse names are out of scope; see
//  IMPLEMENTATION-PLAN.md for those phases.
// ----------------------------------------------------------------------------
#pragma once

#include <Arduino.h>
#include <stdio.h>

#include "stopwatch.h"
#include "ui/screens.h"

const char *mouse_names[] = {
    // Original list
    "CUNNING ODYSSEUS",  // 16 chars
    "CUMBERSOME OTIS",   // 16 chars
    "MOUSE BRET OSMIC",  // 16 chars
    "MOUSE BOT CRIMES",  // 16 chars
    "MOUSE TOM SCRIBE",  // 16 chars
    "MOUSE ROMEO RICH",  // 16 chars
    "BRONZE NUMERATOR",  // 16 chars
    "TITAN MECHANOIDX",  // 16 chars
    "CHRONOS ROBOTICS",  // 16 chars
    "VECTOR AUTOMATON",  // 16 chars
    "ORION STEALTH 09",  // 16 chars
    "ROBOT ANDREW 001",  // 16 chars
    "R. DANEEL OLIVAW",  // 16 chars
    "AEGIS DEFENDER V",  // 16 chars
    "VORTEX BOT V2.04",  // 16 chars
    "SPECTER MECH 77B",  // 16 chars
    "SIGMA LOGIC CORE",  // 16 chars
    "AURA STRIKER 440",  // 16 chars
    "OMEGA DRONE MKIV",  // 16 chars
    "CYPHER SYNTH 99X",  // 16 chars
    "NEBULA SEEKER X1",  // 16 chars
    "QUASAR TRACKER V",  // 16 chars
    "PULSAR PROBE 900",  // 16 chars
    "ANDROMEDA ROVER1",  // 16 chars
    "STELLAR OBSERVER",  // 16 chars
    "ORION ENGINE 500",  // 16 chars
    "SUPERNOVA ROVERX",  // 16 chars
    "GALACTIC WALKER8",  // 16 chars
    "ECLIPSE RUNNER V",  // 16 chars
    "NEXUS SIX RUNNER",  // 16 chars
    "SKYNET ASSASSINS",  // 16 chars
    "BORG DRONE SEVEN",  // 16 chars
    "DALEK SUPREME 01",  // 16 chars
    "MATRIX SENTINELS",  // 16 chars
    "WEYLAND SYNTH 01",  // 16 chars
    "HAL 9000 SYSTEMS",  // 16 chars
    "ROBOCOP OCP UNIT",  // 16 chars
    "GLADOS MAINFRAME",  // 16 chars
};
constexpr size_t NUM_MICE = sizeof(mouse_names) / sizeof(mouse_names[0]);

// RaceEvent is this state machine's only vocabulary -- it has no idea what
// produced one (button, Serial, HTTP). Don't confuse it with InputEvent
// (input-events.h), which is a raw button/touch press from a specific
// source; something upstream (main.cpp today) decides what a given
// InputEvent means as a RaceEvent.
enum RaceEvent { EV_NONE, EV_NEW_MOUSE, EV_ARM, EV_START, EV_GOAL, EV_RESTART };

enum class RaceState : uint8_t { CALIBRATE, NEW_MOUSE, WAITING, ARMED, RUNNING, GOAL };

struct RaceRun {
  uint16_t mouse_id;
  uint16_t run_number;  // 1..MAX_RUNS_PER_MOUSE, from mouse_run_count
  uint32_t time_ms;
};

constexpr size_t MAX_RESULTS = 120;
constexpr size_t MAX_RUNS_PER_MOUSE = 5;

inline RaceRun race_runs[MAX_RESULTS];
inline size_t race_run_count = 0;

// Index into race_runs[] where the current mouse's block of runs begins.
// Runs for a given mouse are always contiguous (no interleaving between
// mice), so this plus race_run_count is enough to slice out "this mouse's
// runs" without a separate per-mouse array.
inline size_t mouse_first_run_index = 0;

// Monotonically increasing, never wraps or resets -- each new mouse gets a
// fresh id. The display name wraps: mouse_names[mouse_id % NUM_MICE].
inline uint16_t mouse_id = 0;

inline Stopwatch run_sw;
inline RaceState race_state = RaceState::CALIBRATE;
inline uint16_t mouse_run_count = 0;

inline RaceState race_timer_get_state() {
  return race_state;
}

inline void race_timer_format_time(uint32_t ms, char *buf, size_t len) {
  uint32_t minutes = ms / 60000;
  uint32_t seconds = (ms / 1000) % 60;
  uint32_t millis_part = ms % 1000;
  snprintf(buf, len, "%02u:%02u.%03u", (unsigned)minutes, (unsigned)seconds, (unsigned)millis_part);
}

/**
 * For the run times and the leaderboard, we maintain/ simple text buffers
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

// Shows the current mouse's own runs -- race_runs[mouse_first_run_index,
// race_run_count), which is at most MAX_RUNS_PER_MOUSE entries because runs
// for a given mouse are always contiguous.
inline void race_timer_update_run_times() {
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
/**
 * Shows the fastest run for each mouse, best time first, top TOP_N mice.
 *
 * Because each mouse's runs are one contiguous block in race_runs[], a
 * single forward pass finds every mouse's best time: walk the array and
 * close out the previous block's minimum each time mouse_id changes.
 * The resulting per-mouse candidate list is small (one entry per mouse
 * seen this session), so a plain insertion sort is fine.
 */
inline void race_timer_update_leaderboard() {
  static constexpr size_t TOP_N = 5;

  struct Candidate {
    uint16_t mouse_id;
    uint32_t best_time_ms;
  };
  Candidate candidates[MAX_RESULTS];
  size_t candidate_count = 0;

  for (size_t i = 0; i < race_run_count;) {
    uint16_t id = race_runs[i].mouse_id;
    uint32_t best = race_runs[i].time_ms;
    size_t j = i + 1;
    while (j < race_run_count && race_runs[j].mouse_id == id) {
      if (race_runs[j].time_ms < best) {
        best = race_runs[j].time_ms;
      }
      j++;
    }
    candidates[candidate_count++] = {id, best};
    i = j;
  }

  // insertion sort by best_time_ms ascending
  for (size_t i = 1; i < candidate_count; i++) {
    Candidate key = candidates[i];
    size_t j = i;
    while (j > 0 && candidates[j - 1].best_time_ms > key.best_time_ms) {
      candidates[j] = candidates[j - 1];
      j--;
    }
    candidates[j] = key;
  }

  label_list_clear(leaderboard_buf, objects.lbl_leaderboard_list);
  size_t shown = candidate_count < TOP_N ? candidate_count : TOP_N;
  for (size_t i = 0; i < shown; i++) {
    char time_str[16];
    race_timer_format_time(candidates[i].best_time_ms, time_str, sizeof(time_str));
    char line[40];
    snprintf(line, sizeof(line), "%u: %s %s", (unsigned)(i + 1), mouse_names[candidates[i].mouse_id % NUM_MICE], time_str);
    label_list_append(leaderboard_buf, objects.lbl_leaderboard_list, line);
  }
}

//============================================================================
/***
 * This is the main interface where a new run time is recorded
 */
inline void race_timer_commit_run(uint32_t time_ms) {
  if (race_run_count < MAX_RESULTS) {
    RaceRun &r = race_runs[race_run_count++];
    r.mouse_id = mouse_id;
    r.run_number = mouse_run_count;
    r.time_ms = time_ms;
  }

  race_timer_update_run_times();
  race_timer_update_leaderboard();
}

//============================================================================
// Doc's "first time for this mouse" contest_time/run-counter reset is
// consolidated here (NEW_MOUSE unconditionally resets mouse_run_count before
// WAITING/ARMED is ever reached) rather than re-checked again on every ARMED
// entry -- same observable behaviour, simpler state.
inline void race_timer_enter_new_mouse() {
  mouse_id++;
  mouse_run_count = 0;
  mouse_first_run_index = race_run_count;
  lv_label_set_text(objects.lbl_mouse_name, mouse_names[mouse_id % NUM_MICE]);
  race_state = RaceState::WAITING;
}

//============================================================================
inline bool race_timer_mouse_exhausted() {
  return mouse_run_count >= MAX_RUNS_PER_MOUSE;
}

// Arms for another run unless this mouse has used up its MAX_RUNS_PER_MOUSE
// attempts, in which case it drops back to WAITING -- the operator must send
// EV_NEW_MOUSE to continue.
inline void race_timer_try_arm() {
  race_state = race_timer_mouse_exhausted() ? RaceState::WAITING : RaceState::ARMED;
}

//============================================================================
inline void race_timer_handle_event(RaceEvent event) {
  if (event == EV_NONE) {
    return;
  }

  switch (race_state) {
    case RaceState::CALIBRATE:
      if (event == EV_NEW_MOUSE || event == EV_RESTART) {
        race_timer_enter_new_mouse();
      }
      break;

    case RaceState::WAITING:
      if (event == EV_ARM) {
        race_timer_try_arm();
      } else if (event == EV_RESTART) {
        race_timer_enter_new_mouse();
      }
      break;

    case RaceState::ARMED:
      if (event == EV_START) {
        run_sw.restart();
        mouse_run_count++;
        lv_label_set_text_fmt(objects.lbl_run_number, "%u", (unsigned)mouse_run_count);
        race_state = RaceState::RUNNING;
      } else if (event == EV_NEW_MOUSE || event == EV_RESTART) {
        race_timer_enter_new_mouse();
      }
      break;

    case RaceState::RUNNING:
      if (event == EV_GOAL) {
        run_sw.stop();
        race_timer_commit_run(run_sw.time());
        race_state = RaceState::GOAL;
      } else if (event == EV_ARM) {
        // Manual recovery. Abandon run
        race_timer_try_arm();
      } else if (event == EV_RESTART) {
        race_timer_enter_new_mouse();
      }
      break;

    case RaceState::GOAL:
      if (event == EV_ARM) {
        race_timer_try_arm();
      } else if (event == EV_RESTART) {
        race_timer_enter_new_mouse();
      }
      break;

    case RaceState::NEW_MOUSE:
      // Transient -- race_timer_enter_new_mouse() always leaves this state
      // set to WAITING directly, so it's never observed here.
      break;
  }
}

inline void race_timer_init() {
  label_list_clear(run_times_buf, objects.lbl_run_time_list);
}

// Called every loop() iteration regardless of events, so the Run Timer
// redraws live while RUNNING.
inline void race_timer_render() {
  char buf[16];
  switch (race_state) {
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
      race_timer_format_time(run_sw.time(), buf, sizeof(buf));
      lv_label_set_text(objects.lbl_current_run_time, buf);
      break;
  }
}
