// ----------------------------------------------------------------------------
//  pulser-commands.h -- `list`, `run`, `arm` and `status` serial commands
//  (`help`/`?` alias a command listing, not `list`), registered with
//  cli.h's Cli class, plus the pulse-sequence trials `run`/`arm` select
//  between. Bench testing only: connect over USB serial, type `help` (or
//  `?`) to see available commands, `list` to see available trial names with
//  a description of each, `run <name> [count] [interval_ms]` to fire one
//  immediately, or `arm <name> [count] [interval_ms]` so the physical
//  button fires it (repeated `count` times, `interval_ms` apart) on the
//  next press.
// ----------------------------------------------------------------------------
#pragma once

#include <Arduino.h>
#include <stdlib.h>
#include <string.h>

#include "cli.h"

const int TRG_ARM = 7;
const int TRG_START = 6;
const int TRG_GOAL = 5;

const int BURST_COUNT = 40;
const uint32_t BURST_INTERVAL_MS = 90;  // above DEBOUNCE_US (50ms, hesperus
                                        // main.cpp) so the ISR doesn't
                                        // discard these as bounce, but well
                                        // under the ~250-270ms per-event
                                        // send cost (NETWORK-TIMING-ISSUE.md
                                        // #8), so the burst is expected to
                                        // queue.
const uint32_t BURST_PULSE_MS = 10;

const uint32_t DOUBLE_TRIGGER_GAP_MS = 150;  // edge-to-edge; above
                                             // DEBOUNCE_US (50ms) so
                                             // hesperus's ISR treats these
                                             // as two distinct triggers,
                                             // not bounce.

const uint32_t RUN_DURATION_MS = 3000;     // START-to-GOAL, leading-edge-to-
                                           // leading-edge.
const uint32_t ARM_TO_START_GAP_MS = 200;  // leading-edge-to-leading-edge,
                                           // same convention as
                                           // trial_arm_then_start().

/// @brief One trial: a single 100ms active-low pulse on TRG_ARM, mimicking
/// one beam-break trigger.
inline void trial_arm_pulse() {
  digitalWrite(LED_BUILTIN, 1);
  digitalWrite(TRG_ARM, 0);
  delay(100);
  digitalWrite(LED_BUILTIN, 0);
  digitalWrite(TRG_ARM, 1);
}

/// @brief Single 100ms active-low pulse on TRG_GOAL alone, mirroring
/// trial_arm_pulse(). Repeated at high count/short interval (e.g.
/// `trial goal_pulse 10000 250`), this is the single-gate GOAL-only steady-
/// state traffic NETWORK-TIMING-ISSUE.md's WS-jitter characterization test
/// proposes, to hunt for periodicity/clustering in a long, steady run
/// (see the "Unexplained minor WS jitter" issue there).
inline void trial_goal_pulse() {
  digitalWrite(LED_BUILTIN, 1);
  digitalWrite(TRG_GOAL, 0);
  delay(100);
  digitalWrite(LED_BUILTIN, 0);
  digitalWrite(TRG_GOAL, 1);
}

/// @brief ARM then START, trigger edges 200ms apart -- reproduces a robot
/// crossing both gates in quick succession when one board serves both
/// (NETWORK-TIMING-ISSUE.md #9), which queues START behind ARM's send cycle.
inline void trial_arm_then_start() {
  digitalWrite(LED_BUILTIN, 1);
  digitalWrite(TRG_ARM, 0);
  delay(100);
  digitalWrite(TRG_ARM, 1);
  delay(100);
  digitalWrite(TRG_START, 0);
  delay(100);
  digitalWrite(LED_BUILTIN, 0);
  digitalWrite(TRG_START, 1);
}

/// @brief BURST_COUNT pulses on TRG_GOAL, BURST_INTERVAL_MS apart -- a
/// controlled, repeatable version of the earlier manual rapid-fire test
/// (NETWORK-TIMING-ISSUE.md #8). Checks whether hesperus's depth-10
/// `networkQueue` actually overflows and silently drops events at a known
/// rate, instead of just inferring it from timing alone: compare
/// BURST_COUNT against how many GOAL events cerberus's log actually shows
/// -- a dropped event never reaches cerberus at all, so it shows up as a
/// missing one, not a long latency.
inline void trial_burst() {
  for (int i = 0; i < BURST_COUNT; i++) {
    digitalWrite(LED_BUILTIN, 1);
    digitalWrite(TRG_GOAL, 0);
    delay(BURST_PULSE_MS);
    digitalWrite(LED_BUILTIN, 0);
    digitalWrite(TRG_GOAL, 1);
    delay(BURST_INTERVAL_MS - BURST_PULSE_MS);
  }
}

/// @brief Two GOAL pulses on the same pin, DOUBLE_TRIGGER_GAP_MS apart --
/// simulates a robot with a gapped/slotted structure breaking one gate's
/// beam twice during what should count as a single crossing
/// (NETWORK-TIMING-ISSUE.md #9). The race state machine is expected to
/// tolerate the duplicate once it's left RUNNING; this checks the network
/// side doesn't do anything worse with the second trigger than the
/// ARM/START case did.
inline void trial_double_trigger() {
  digitalWrite(LED_BUILTIN, 1);
  digitalWrite(TRG_GOAL, 0);
  delay(100);
  digitalWrite(TRG_GOAL, 1);
  delay(DOUBLE_TRIGGER_GAP_MS - 100);
  digitalWrite(TRG_GOAL, 0);
  delay(100);
  digitalWrite(LED_BUILTIN, 0);
  digitalWrite(TRG_GOAL, 1);
}

/// @brief One full ARM-START-GOAL run: ARM and START edges
/// ARM_TO_START_GAP_MS apart, START-to-GOAL RUN_DURATION_MS -- exercises
/// the leaderboard-facing path end-to-end (does the committed time come out
/// exact) rather than a single stress dimension. Each pulse's own PULSE_MS
/// width is subtracted out of the delay that follows it so the edges land
/// exactly on the configured spacing, same convention as
/// trial_arm_then_start(). Fire several back-to-back with the `run`
/// command's `[count] [interval_ms]` (e.g. `run full_run 4`) rather than
/// looping internally.
inline void trial_full_run() {
  const uint32_t PULSE_MS = 100;
  digitalWrite(LED_BUILTIN, 1);
  digitalWrite(TRG_ARM, 0);
  delay(PULSE_MS);
  digitalWrite(TRG_ARM, 1);
  delay(ARM_TO_START_GAP_MS - PULSE_MS);

  digitalWrite(TRG_START, 0);
  delay(PULSE_MS);
  digitalWrite(TRG_START, 1);
  delay(RUN_DURATION_MS - PULSE_MS);

  digitalWrite(TRG_GOAL, 0);
  delay(PULSE_MS);
  digitalWrite(LED_BUILTIN, 0);
  digitalWrite(TRG_GOAL, 1);
}

const uint32_t WAKE_SWEEP_GAPS_MS[] = {1000, 5000, 15000, 30000, 60000};  // idle
                                                                          // gap before each pulse, spanning
                                                                          // realistic ARM-to-START/START-to-GOAL
                                                                          // spacing up past a modem-sleep beacon
                                                                          // interval.
constexpr size_t WAKE_SWEEP_COUNT = sizeof(WAKE_SWEEP_GAPS_MS) / sizeof(WAKE_SWEEP_GAPS_MS[0]);

/// @brief One ARM pulse fired after each of WAKE_SWEEP_GAPS_MS's idle gaps
/// in turn (1s, 5s, 15s, 30s, 60s) -- the wake-to-first-byte latency sweep
/// NETWORK-TIMING-ISSUE.md's Wi-Fi power-save issue proposes, to compare
/// WIFI_PS_NONE/MIN_MODEM/MAX_MODEM modem-sleep wake cost against a range of
/// realistic idle durations in one pass. Prints the gap before each pulse so
/// it can be matched against cerberus's receipt-time log. Takes just over
/// 111s (the sum of the gaps) to complete -- expect to wait.
inline void trial_wake_sweep() {
  for (size_t i = 0; i < WAKE_SWEEP_COUNT; i++) {
    Serial.print("WAKE_SWEEP gap=");
    Serial.print(WAKE_SWEEP_GAPS_MS[i]);
    Serial.println("ms");
    delay(WAKE_SWEEP_GAPS_MS[i]);
    digitalWrite(LED_BUILTIN, 1);
    digitalWrite(TRG_ARM, 0);
    delay(100);
    digitalWrite(LED_BUILTIN, 0);
    digitalWrite(TRG_ARM, 1);
  }
}

struct TrialSequence {
  const char *name;
  const char *description;
  void (*fn)();
};

// clang-format off
constexpr TrialSequence TRIALS[] = {
    {"arm_pulse",       "single 100ms pulse on TRG_ARM",             trial_arm_pulse},
    {"goal_pulse",      "single 100ms pulse on TRG_GOAL",            trial_goal_pulse},
    {"arm_then_start",  "TRG_ARM then TRG_START, 200ms apart",       trial_arm_then_start},
    {"burst",           "40 pulses on TRG_GOAL, 90ms apart",         trial_burst},
    {"double_trigger",  "two TRG_GOAL pulses, 150ms apart",          trial_double_trigger},
    {"full_run",        "one ARM-START-GOAL run, 3s duration",       trial_full_run},
    {"wake_sweep",      "ARM pulse after 1/5/15/30/60s idle gaps",   trial_wake_sweep},
};
constexpr size_t TRIAL_COUNT = sizeof(TRIALS) / sizeof(TRIALS[0]);
// clang-format on

constexpr uint32_t DEFAULT_REPEAT_COUNT = 1;
constexpr uint32_t DEFAULT_REPEAT_INTERVAL_MS = 1000;

inline int trial_index_by_name(const char *name) {
  for (size_t i = 0; i < TRIAL_COUNT; i++) {
    if (strcmp(name, TRIALS[i].name) == 0) {
      return (int)i;
    }
  }
  return -1;
}

/// @brief Runs TRIALS[idx] `count` times, `intervalMs` apart (no trailing
/// delay after the last rep), printing one `tag <name>` line per rep and a
/// final `OK <name>` line. Shared by cmd_run (immediate serial trigger) and
/// fire_armed_trial (button press), so both report progress the same way.
inline void fire_trial(int idx, uint32_t count, uint32_t intervalMs, const char *tag) {
  for (uint32_t i = 0; i < count; i++) {
    Serial.print(tag);
    Serial.print(' ');
    Serial.println(TRIALS[idx].name);
    TRIALS[idx].fn();
    if (i + 1 < count) {
      delay(intervalMs);
    }
  }
  Serial.print("OK ");
  Serial.println(TRIALS[idx].name);
}

inline size_t armedIndex = 0;
inline uint32_t armedCount = DEFAULT_REPEAT_COUNT;
inline uint32_t armedIntervalMs = DEFAULT_REPEAT_INTERVAL_MS;

/// @brief Fires whichever trial is currently armed, `armedCount` times,
/// `armedIntervalMs` apart. Called from the button press path in
/// main.cpp's loop().
inline void fire_armed_trial() {
  fire_trial((int)armedIndex, armedCount, armedIntervalMs, "BTN");
}

inline void cmd_list(int argc, char **argv) {
  size_t nameWidth = 0;
  for (size_t i = 0; i < TRIAL_COUNT; i++) {
    size_t len = strlen(TRIALS[i].name);
    if (len > nameWidth) {
      nameWidth = len;
    }
  }
  for (size_t i = 0; i < TRIAL_COUNT; i++) {
    Serial.printf("%-*s  %s\n", (int)nameWidth, TRIALS[i].name, TRIALS[i].description);
  }
}

struct CommandHelp {
  const char *name;
  const char *description;
};

// clang-format off
constexpr CommandHelp COMMAND_HELP[] = {
  {"list                              ", "list available trial names"},
  {"run <name> [count] [interval_ms]  ", "fire a trial immediately"},
  {"arm <name> [count] [interval_ms]  ", "arm a trial for the next button press"},
  {"status                            ", "show what's currently armed"},
  {"help / ?                          ", "show this command list"},
};
// clang-format on
constexpr size_t COMMAND_HELP_COUNT = sizeof(COMMAND_HELP) / sizeof(COMMAND_HELP[0]);

inline void cmd_help(int argc, char **argv) {
  for (size_t i = 0; i < COMMAND_HELP_COUNT; i++) {
    Serial.print(COMMAND_HELP[i].name);
    Serial.print(" - ");
    Serial.println(COMMAND_HELP[i].description);
  }
}

/// @brief Parses the optional `[count] [interval_ms]` tail shared by `run`
/// and `arm`. `argv[argOffset]` is `count` if present, `argv[argOffset+1]`
/// is `interval_ms` if present; either or both may be omitted, defaulting
/// to DEFAULT_REPEAT_COUNT / DEFAULT_REPEAT_INTERVAL_MS.
inline void parse_repeat_args(int argc, char **argv, int argOffset, uint32_t &count, uint32_t &intervalMs) {
  count = (argc > argOffset) ? (uint32_t)strtoul(argv[argOffset], nullptr, 10) : DEFAULT_REPEAT_COUNT;
  intervalMs = (argc > argOffset + 1) ? (uint32_t)strtoul(argv[argOffset + 1], nullptr, 10) : DEFAULT_REPEAT_INTERVAL_MS;
  if (count < 1) {
    count = 1;
  }
}

inline void cmd_run(int argc, char **argv) {
  if (argc < 2 || argc > 4) {
    Serial.println("usage: run <name> [count] [interval_ms]");
    return;
  }
  int idx = trial_index_by_name(argv[1]);
  if (idx < 0) {
    Serial.print("ERR unknown trial: ");
    Serial.println(argv[1]);
    return;
  }
  uint32_t count, intervalMs;
  parse_repeat_args(argc, argv, 2, count, intervalMs);
  fire_trial(idx, count, intervalMs, "RUN");
}

inline void cmd_arm(int argc, char **argv) {
  if (argc < 2 || argc > 4) {
    Serial.println("usage: arm <name> [count] [interval_ms]");
    return;
  }
  int idx = trial_index_by_name(argv[1]);
  if (idx < 0) {
    Serial.print("ERR unknown trial: ");
    Serial.println(argv[1]);
    return;
  }
  armedIndex = (size_t)idx;
  parse_repeat_args(argc, argv, 2, armedCount, armedIntervalMs);
  Serial.print("ARMED ");
  Serial.print(TRIALS[armedIndex].name);
  Serial.print(" x");
  Serial.print(armedCount);
  Serial.print(" every ");
  Serial.print(armedIntervalMs);
  Serial.println("ms");
}

inline void cmd_status(int argc, char **argv) {
  Serial.print("ARMED ");
  Serial.print(TRIALS[armedIndex].name);
  Serial.print(" x");
  Serial.print(armedCount);
  Serial.print(" every ");
  Serial.print(armedIntervalMs);
  Serial.println("ms");
}

constexpr CliCommand PULSER_COMMANDS[] = {
    {"list", cmd_list},
    {"run", cmd_run},
    {"arm", cmd_arm},
    {"status", cmd_status},
};
constexpr size_t PULSER_COMMAND_COUNT = sizeof(PULSER_COMMANDS) / sizeof(PULSER_COMMANDS[0]);

constexpr CliAlias PULSER_ALIASES[] = {
    {"help", cmd_help},
    {"?", cmd_help},
};
constexpr size_t PULSER_ALIAS_COUNT = sizeof(PULSER_ALIASES) / sizeof(PULSER_ALIASES[0]);
