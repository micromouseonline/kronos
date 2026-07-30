#include <Arduino.h>

const int TRG_ARM = 7;
const int TRG_START = 6;
const int TRG_GOAL = 5;

const int BTN_IN = 4;
const int BTN_OUT = 2;

// Back to 100 -- trial_double_trigger() below is one repeated per-second
// trial like trial_arm_then_start(), not a self-contained burst.
const int MAX_COUNT = 100;
const uint32_t one_second = 1000000;

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

/// @brief One trial: a single 100ms active-low pulse on TRG_ARM, mimicking
/// one beam-break trigger. Swap the call in loop() to a different trial
/// function (e.g. double-trigger, burst) to change what the 100 repetitions
/// below exercise.
void trial_arm_pulse() {
  digitalWrite(LED_BUILTIN, 1);
  digitalWrite(TRG_ARM, 0);
  delay(100);
  digitalWrite(LED_BUILTIN, 0);
  digitalWrite(TRG_ARM, 1);
}

/// @brief ARM then START, trigger edges 200ms apart -- reproduces a robot
/// crossing both gates in quick succession when one board serves both
/// (NETWORK-TIMING-ISSUE.md #9), which queues START behind ARM's send cycle.
void trial_arm_then_start() {
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
void trial_burst() {
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
void trial_double_trigger() {
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

void setup() {
  pinMode(TRG_ARM, OUTPUT);
  pinMode(TRG_START, OUTPUT);
  pinMode(TRG_GOAL, OUTPUT);
  digitalWrite(TRG_ARM, 0);
  digitalWrite(TRG_START, 0);
  digitalWrite(TRG_GOAL, 0);

  pinMode(BTN_IN, INPUT_PULLUP);
  pinMode(BTN_OUT, OUTPUT);
  digitalWrite(BTN_OUT, 0);

  pinMode(LED_BUILTIN, OUTPUT);

  while (digitalRead(BTN_IN) == 1) {
    yield();
  }
}

int count = 0;
uint32_t last_pulse;
bool started = false;

void loop() {
  if (!started) {
    // Started here, not in setup(), so the first trial lands exactly
    // one_second after the button press -- not after however long it took
    // to press it (setup()'s wait can take arbitrarily long).
    last_pulse = micros();
    started = true;
  }

  if (count >= MAX_COUNT) {
    yield();
    return;
  }
  if (micros() - last_pulse < one_second) {
    return;
  }
  last_pulse += one_second;
  trial_double_trigger();
  count++;
}
