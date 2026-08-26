#include <Arduino.h>
#include <JC_Button.h>
#include <Preferences.h>

const int PIN_A = 2;
const int PIN_B = 3;
const int BTN_PIN = 0;

// Active low, internal pull-up: JC_Button's defaults already match.
Button btn(BTN_PIN);
Preferences prefs;
bool pinBState = false;

void setup() {
  pinMode(PIN_A, OUTPUT);
  pinMode(PIN_B, OUTPUT);
  digitalWrite(PIN_A, LOW);

  // NVS namespace names are capped at 15 chars -- "hesperus-emitter" (16)
  // silently fails prefs.begin(), which made every getBool/putBool below a
  // no-op returning its default (false / always LOW after reset).
  prefs.begin("hesp-emitter", false);
  pinBState = prefs.getBool("pinBState", false);
  digitalWrite(PIN_B, pinBState);

  btn.begin();
}

void loop() {
  btn.read();
  if (btn.wasPressed()) {
    pinBState = !pinBState;
    digitalWrite(PIN_B, pinBState);
    prefs.putBool("pinBState", pinBState);
  }
}
