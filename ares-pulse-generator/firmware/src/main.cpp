#include <Arduino.h>
#include <JC_Button.h>

#include "cli.h"
#include "pulser-commands.h"

const int BTN_IN = 4;
const int BTN_OUT = 2;

Cli cli;
Button btn(BTN_IN);

void setup() {
  // Trigger pins first, before anything else (including Serial.begin(),
  // which can stall waiting on USB CDC enumeration) -- until pinMode+
  // digitalWrite actually run, these sit in their post-reset floating
  // input state, which a connected gate's active-low input can read as an
  // asserted trigger. Driving them high as early as possible minimizes
  // that false-trigger window.
  pinMode(TRG_ARM, OUTPUT);
  pinMode(TRG_START, OUTPUT);
  pinMode(TRG_GOAL, OUTPUT);
  digitalWrite(TRG_ARM, 1);
  digitalWrite(TRG_START, 1);
  digitalWrite(TRG_GOAL, 1);

  Serial.begin(115200);

  pinMode(BTN_OUT, OUTPUT);
  digitalWrite(BTN_OUT, 0);
  btn.begin();

  pinMode(LED_BUILTIN, OUTPUT);

  cli.begin(PULSER_COMMANDS, PULSER_COMMAND_COUNT, PULSER_ALIASES, PULSER_ALIAS_COUNT);
}

void loop() {
  cli.poll();
  btn.read();
  if (btn.wasPressed()) {
    fire_armed_trial();
  }
}
