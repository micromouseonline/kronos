// ----------------------------------------------------------------------------
//  race-timer.h — Race timing state machine (docs/maze-timer-state-machine.md,
//  workspace root). This is the model: state machine + run/leaderboard data.
//  It knows nothing about input hardware (only RaceCommands -- see
//  race-command-source.h for the current ButtonID -> RaceCommand mapping)
//  and nothing about display hardware (no lv_* calls, no ui/screens.h -- see
//  race-timer-display.h, which reads this file's data/getters each render
//  tick and owns SCREEN_ID_MAIN's labels).
//
//  States: CALIBRATE, NEW_MOUSE, WAITING, ARMED, RUNNING, GOAL. CALIBRATE
//  here is this race state machine's boot state and is unrelated to
//  display/touch-calibration.h's screen-touch calibration wizard.
//
//  Scope: RaceCommand::RESTART (the doc's global "any state" override) now
//  has three producers, none of them a plain button press -- ARM-held
//  (BUTTON_COMMAND_MAP, race-command-source.h), the legacy serial
//  protocol's MSG_NEW_MOUSE (race_command_from_serial(), same file), and
//  HTTP's `"event": "RESTART"` (race_command_from_http(), same file).
//  Session Countdown Timer, MAINTENANCE, and persistent mouse names are
//  out of scope; see PLANNED-UPDATES.md.
// ----------------------------------------------------------------------------
#pragma once

#include <Arduino.h>
#include <stdio.h>

#include "stopwatch.h"

const char *mouse_names[] = {
    // Original list
    "CUNNING ODYSSEUS",  // 16 chars
    "CUMBERSOME OTIS",   // 16 chars
    "BRET OSMIC MOUSE",  // 16 chars
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
    "MOUSE TOM SCRIBE",  // 16 chars
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
    "MOUSE BOT CRIMES",  // 16 chars
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

// RaceCommand is this state machine's only vocabulary -- it has no idea what
// produced one (button, Serial, HTTP). Don't confuse it with InputEvent
// (input-events.h), which is a raw button/touch press from a specific
// source; something upstream (race-command-source.h today) decides what a
// given InputEvent means as a RaceCommand.
enum class RaceCommand {
  NONE,               //
  NEW_MOUSE,          //
  ARM,                //
  START,              //
  GOAL,               //
  RESTART,            //
  ENTER_CALIBRATION,  // host SetMode(CALIBRATION) -- serial-only, see race-command-source.h
  RESUME_TIMER,       // host SetMode(TIMER) -- serial-only
  EXTRA_RUN           // host ExtraRun -- serial-only
};

enum class RaceState : uint8_t {
  CALIBRATE,  //
  NEW_MOUSE,  //
  WAITING,    // waiting for new mouse
  ARMED,      //
  RUNNING,    //
  GOAL,       //
  TIMED_OUT   // mouse still running but timed out
};

struct RaceRun {
  uint16_t mouse_id;
  uint16_t run_number;  // 1..MAX_RUNS_PER_MOUSE, from mouse_run_count
  uint32_t time_ms;
  char name[32];  // copied from current_mouse_name at commit time
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

inline Stopwatch run_sw;    // measures individual run time
inline Stopwatch entry_sw;  // measures elapsed maze time

inline RaceState race_state = RaceState::CALIBRATE;
inline uint16_t mouse_run_count = 0;

// False from boot until the operator picks the maze timer off the main
// menu (action_on_menu_maze_timer, eez-actions.cpp) -- race_timer_handle_
// command() is a no-op until then, so button/serial/HTTP traffic arriving
// while the menu is showing can't advance race_state. Never cleared once
// set, so navigating back to the menu screen afterwards (BTN_TOUCH HELD,
// main.cpp's input_event_handler) leaves an in-progress race running.
inline bool race_timer_active = false;

const uint32_t RACE_TIME_LIMIT = 5L * 60L * 1000L;
inline uint32_t time_left = RACE_TIME_LIMIT;

// Host-configurable overrides from the RATS V2 serial messages AllowedRuns/
// EntryTimeS (net/serial-protocol.h, race-command-source.h). -1 = not set
// by the host -- race_timer_mouse_exhausted() falls back to
// MAX_RUNS_PER_MOUSE, and the display (race-timer-display.h) falls back to
// its original raw-elapsed behaviour. Plain globals, not routed through the
// Main Event Queue -- benign stale reads only (same tolerance already
// accepted elsewhere in this codebase, e.g. mouse_id in
// race-serial-telemetry.h), unlike ENTER_CALIBRATION/RESUME_TIMER/
// EXTRA_RUN below which actually mutate race_state/mouse_run_count and so
// must go through the queue.
//
// g_allowed_runs resets to -1 on every race_timer_enter_new_mouse() --
// the host resends it after each NewMouse (per
// docs/preferredMessageSequencesV2.pdf's state-9 sequence), and a stale
// limit from the previous mouse must not carry over if it doesn't.
//
// g_entry_time_s_limit does NOT reset -- once set via <93,xxx>, it
// persists as the starting entry time for every subsequent NewMouse until
// the host sends a new value. Defaults to 600s (docs/updated-state-table.md:
// "Default Entry time set to 600 seconds"), not "unset" -- a real countdown
// is always active, even with no host ever connected, rather than falling
// back to the old raw-elapsed display. Still negative-settable (e.g.
// <93,-1>) as an emergent way for a host to explicitly request "unset"
// behaviour, since race_timer_entry_time_remaining_ms() only requires >= 0.
inline long g_allowed_runs = -1;
inline long g_entry_time_s_limit = 600;

// True once entry_sw has actually started counting down for the current
// mouse's entry -- see race_timer_try_arm(): the entry-time countdown
// only starts on the mouse's first WAITING->ARMED transition, not at
// NewMouse itself (entry_sw stays at 0 -- i.e. the full starting
// duration remains displayed, unchanging -- until then). Reset on every
// race_timer_enter_new_mouse().
inline bool entry_timer_started = false;

// Current mouse's display name -- host-supplied (RATS V2 NewMouse) or the
// canned mouse_names[] pick, decided once in race_timer_enter_new_mouse()
// so every reader (on-screen label, RaceRun.name, leaderboard) just uses
// this unconditionally with no per-site fallback logic.
inline char current_mouse_name[32] = "";

inline RaceState race_timer_get_state() {
  return race_state;
}

inline void race_timer_format_time(uint32_t ms, char *buf, size_t len) {
  uint32_t minutes = ms / 60000;
  uint32_t seconds = (ms / 1000) % 60;
  uint32_t millis_part = ms % 1000;
  snprintf(buf, len, "%02u:%02u.%03u", (unsigned)minutes, (unsigned)seconds, (unsigned)millis_part);
}

inline void race_timer_format_time_seconds(uint32_t ms, char *buf, size_t len) {
  uint32_t minutes = ms / 60000;
  uint32_t seconds = (ms / 1000) % 60;
  snprintf(buf, len, "%02u:%02u", (unsigned)minutes, (unsigned)seconds);
}

//============================================================================
struct LeaderboardEntry {
  uint16_t mouse_id;
  uint32_t best_time_ms;
  char name[32];
};

/**
 * Computes the fastest run for each mouse seen this session, sorted fastest
 * first, into out[0..return value). out must have room for at least
 * race_run_count entries (MAX_RESULTS is always enough).
 *
 * Because each mouse's runs are one contiguous block in race_runs[], a
 * single forward pass finds every mouse's best time: walk the array and
 * close out the previous block's minimum each time mouse_id changes. The
 * resulting per-mouse list is small (one entry per mouse seen this
 * session), so a plain insertion sort is fine.
 */
inline size_t race_timer_compute_leaderboard(LeaderboardEntry *out, size_t max_out) {
  size_t candidate_count = 0;
  for (size_t i = 0; i < race_run_count && candidate_count < max_out;) {
    uint16_t id = race_runs[i].mouse_id;
    uint32_t best = race_runs[i].time_ms;
    size_t j = i + 1;
    while (j < race_run_count && race_runs[j].mouse_id == id) {
      if (race_runs[j].time_ms < best) {
        best = race_runs[j].time_ms;
      }
      j++;
    }
    LeaderboardEntry &entry = out[candidate_count++];
    entry.mouse_id = id;
    entry.best_time_ms = best;
    // Name doesn't change within a mouse's block -- the first run's name
    // (set from current_mouse_name at commit time) applies to the whole
    // mouse.
    strncpy(entry.name, race_runs[i].name, sizeof(entry.name) - 1);
    entry.name[sizeof(entry.name) - 1] = '\0';
    i = j;
  }

  // insertion sort by best_time_ms ascending
  for (size_t i = 1; i < candidate_count; i++) {
    LeaderboardEntry key = out[i];
    size_t j = i;
    while (j > 0 && out[j - 1].best_time_ms > key.best_time_ms) {
      out[j] = out[j - 1];
      j--;
    }
    out[j] = key;
  }
  return candidate_count;
}

// Optional hook, invoked after every race_timer_commit_run() -- lets other
// layers react the instant a new result lands (e.g. net/http-server.h's SSE
// push to the leaderboard page) without this file needing to know what
// HTTP/SSE even are.
inline void (*race_timer_on_run_committed)() = nullptr;

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
    strncpy(r.name, current_mouse_name, sizeof(r.name) - 1);
    r.name[sizeof(r.name) - 1] = '\0';
    if (race_timer_on_run_committed != nullptr) {
      race_timer_on_run_committed();
    }
  }
}

//============================================================================
// Doc's "first time for this mouse" contest_time/run-counter reset is
// consolidated here (NEW_MOUSE unconditionally resets mouse_run_count before
// WAITING/ARMED is ever reached) rather than re-checked again on every ARMED
// entry -- same observable behaviour, simpler state.
// name is the RATS V2 NewMouse-supplied name (see race-command-source.h/
// system-event-queue.h's payload_is_mouse_name) -- nullptr or empty for
// every other producer (local button, HTTP, or a bare legacy <98,0>),
// which falls back to the canned mouse_names[] pick as before.
inline void race_timer_enter_new_mouse(const char *name = nullptr) {
  mouse_id++;
  mouse_id %= NUM_MICE;
  mouse_run_count = 0;
  mouse_first_run_index = race_run_count;
  time_left = RACE_TIME_LIMIT;
  race_state = RaceState::WAITING;
  run_sw.reset();
  entry_sw.reset();
  if (name != nullptr && name[0] != '\0') {
    strncpy(current_mouse_name, name, sizeof(current_mouse_name) - 1);
    current_mouse_name[sizeof(current_mouse_name) - 1] = '\0';
  } else {
    strncpy(current_mouse_name, mouse_names[mouse_id % NUM_MICE], sizeof(current_mouse_name) - 1);
    current_mouse_name[sizeof(current_mouse_name) - 1] = '\0';
  }
  // The host resends AllowedRuns after every NewMouse (per
  // docs/preferredMessageSequencesV2.pdf) -- a limit from the previous
  // mouse must not silently carry over if it doesn't. EntryTimeS is NOT
  // reset here -- it persists as the starting entry time for every
  // subsequent mouse until the host sends a new value (see
  // g_entry_time_s_limit's comment above).
  g_allowed_runs = -1;
  entry_timer_started = false;
}

//============================================================================
// Host-configured AllowedRuns overrides the fixed cap when set (see
// g_allowed_runs above); exposed separately from race_timer_mouse_exhausted()
// so the display (race-timer-display.h's "current/max" run-number label)
// can show the same effective limit without duplicating the fallback logic.
inline long race_timer_allowed_runs() {
  return (g_allowed_runs >= 0) ? g_allowed_runs : (long)MAX_RUNS_PER_MOUSE;
}

inline bool race_timer_mouse_exhausted() {
  return (long)mouse_run_count >= race_timer_allowed_runs();
}

// Remaining entry time in ms for the host-supplied EntryTimeS countdown,
// clamped to 0. Caller must check g_entry_time_s_limit >= 0 first (this
// doesn't handle "unset" itself). Before entry_timer_started, entry_sw
// reads 0 (RESET state -- see race_timer_enter_new_mouse()/
// race_timer_try_arm()), so this naturally returns the full starting
// duration, unchanging, until the first ARM. Once the countdown reaches
// zero, entry_sw is explicitly stopped (mirrors run_sw.stop() on GOAL) so
// elapsed time freezes there instead of drifting past the limit --
// "will stop when it gets to zero" per the design.
inline uint32_t race_timer_entry_time_remaining_ms() {
  long limit_ms = g_entry_time_s_limit * 1000L;
  long elapsed_ms = (long)entry_sw.time();
  long remaining_ms = limit_ms - elapsed_ms;
  if (remaining_ms <= 0) {
    entry_sw.stop();
    return 0;
  }
  return (uint32_t)remaining_ms;
}

// Arms for another run unless this mouse has used up its MAX_RUNS_PER_MOUSE
// attempts, in which case it drops back to WAITING -- the operator must send
// RaceCommand::NEW_MOUSE to continue.
//
// The entry-time countdown (g_entry_time_s_limit) starts here, once --
// the mouse's first successful WAITING->ARMED transition -- not at
// NewMouse itself, and is never restarted again for this mouse's
// subsequent runs (ARMED reached again from GOAL/RUNNING keeps
// entry_timer_started true, so entry_sw just keeps counting through the
// whole entry, same as before this round's changes).
inline void race_timer_try_arm() {
  bool exhausted = race_timer_mouse_exhausted();
  race_state = exhausted ? RaceState::WAITING : RaceState::ARMED;
  if (!exhausted && !entry_timer_started) {
    entry_timer_started = true;
    entry_sw.restart();
  }
}

//============================================================================
// mouse_name is only ever non-null for a RATS V2 NewMouse carrying a real
// name (see system_event_handler(), main.cpp) -- passed straight through
// to race_timer_enter_new_mouse() at every call site below.
inline void race_timer_handle_command(RaceCommand command, const char *mouse_name = nullptr) {
  if (command == RaceCommand::NONE) {
    return;
  }
  // Inactive until the maze timer's been selected off the main menu (see
  // race_timer_active's comment above) -- ignore every command, including
  // the "any state" host overrides below, until then.
  if (!race_timer_active) {
    return;
  }

  // Host overrides that apply regardless of race_state, same reasoning as
  // RESTART below. Mutating race_state/mouse_run_count here (rather than
  // directly from serial-protocol.h's RX task) is what keeps this safe --
  // see race-command-source.h's serial_protocol_handle_info_message() for
  // why these are routed through the Main Event Queue as RaceCommands
  // instead of being applied as a direct write from another task.
  if (command == RaceCommand::ENTER_CALIBRATION) {
    race_state = RaceState::CALIBRATE;
    // docs/updated-state-table.md: Calibrating shows a blank mouse name
    // and "0/5" run count, not whatever the previous mouse left behind.
    // Run Times list is blanked in the display layer instead (see
    // race-timer-display.h) -- mouse_first_run_index is deliberately left
    // alone here so a later RESUME_TIMER doesn't lose track of this
    // mouse's earlier runs for leaderboard/history purposes.
    mouse_run_count = 0;
    g_allowed_runs = -1;
    current_mouse_name[0] = '\0';
    // Abandons whatever attempt was in progress -- stop/reset the current
    // entry's timers too, same fields race_timer_enter_new_mouse() resets
    // for a genuinely new mouse, minus mouse_id/mouse_first_run_index
    // (those stay put so the leaderboard/run history survive the detour).
    entry_timer_started = false;
    run_sw.reset();
    entry_sw.reset();
    time_left = RACE_TIME_LIMIT;
    return;
  }
  if (command == RaceCommand::RESUME_TIMER) {
    race_state = RaceState::WAITING;
    return;
  }
  if (command == RaceCommand::EXTRA_RUN) {
    if (mouse_run_count > 0) {
      mouse_run_count--;
    }
    return;
  }

  switch (race_state) {
    case RaceState::CALIBRATE:
      // RESTART only, not bare NEW_MOUSE -- docs/updated-state-table.md:
      // "Only a NewMouse event drops the controller out of CALIBRATING --
      // either receiving the <98,xxxx> message, or a long press on the A
      // key... T gets no response here." Both of those already normalize
      // to RESTART (race_command_from_serial(), BUTTON_COMMAND_MAP's
      // ARM-hold); a bare NEW_MOUSE only ever comes from BTN_TOUCH's short
      // press ("T"), which this state must now ignore.
      if (command == RaceCommand::RESTART) {
        race_timer_enter_new_mouse(mouse_name);
      }
      break;

    case RaceState::WAITING:
      if (command == RaceCommand::ARM) {
        // entry_sw itself only actually starts on a successful ARM here
        // (race_timer_try_arm(), first time only) -- no longer
        // unconditionally restarted on every WAITING-state command, so
        // the entry-time countdown genuinely begins at first ARM, not at
        // NewMouse or at every WAITING command in between.
        race_timer_try_arm();
      } else if (command == RaceCommand::RESTART) {
        race_timer_enter_new_mouse(mouse_name);
      }
      break;

    case RaceState::ARMED:
      if (command == RaceCommand::START) {
        run_sw.restart();
        mouse_run_count++;
        race_state = RaceState::RUNNING;
      } else if (command == RaceCommand::RESTART) {
        race_timer_enter_new_mouse(mouse_name);
      }
      // Bare NEW_MOUSE (T/BTN_TOUCH short press) deliberately does nothing
      // here -- docs/updated-state-table.md leaves T's meaning in ARMED
      // undecided ("available if we should decide to use it at some
      // point"), so it's disabled rather than left abandoning the armed
      // mouse, until that's actually decided.
      break;

    case RaceState::RUNNING:
      if (command == RaceCommand::GOAL) {
        run_sw.stop();
        race_timer_commit_run(run_sw.time());
        race_state = RaceState::GOAL;
      } else if (command == RaceCommand::ARM) {
        // Manual recovery. Abandon run
        race_timer_try_arm();
      } else if (command == RaceCommand::RESTART) {
        race_timer_enter_new_mouse(mouse_name);
      }
      break;

    case RaceState::GOAL:
      if (command == RaceCommand::ARM) {
        race_timer_try_arm();
      } else if (command == RaceCommand::RESTART) {
        race_timer_enter_new_mouse(mouse_name);
      }
      break;

    case RaceState::NEW_MOUSE:
      // Transient -- race_timer_enter_new_mouse() always leaves this state
      // set to WAITING directly, so it's never observed here.
      break;
  }
}

inline void race_timer_init() {
  race_run_count = 0;
  mouse_run_count = 0;
  mouse_id = 0;
}
