#include <Arduino.h>
#include <Preferences.h>
#include "esp_sleep.h"
#include "driver/rtc_io.h"

const gpio_num_t PIN_A = GPIO_NUM_2;
const gpio_num_t PIN_B = GPIO_NUM_3;
const gpio_num_t BTN_PIN = GPIO_NUM_0;

const uint32_t DEBOUNCE_MS = 30;

Preferences prefs;

void goToSleep() {
  // Latches PIN_A/PIN_B at their current level through deep sleep -- the
  // pads would otherwise float once the digital domain powers down.
  rtc_gpio_hold_en(PIN_A);
  rtc_gpio_hold_en(PIN_B);

  // pinMode(INPUT_PULLUP) doesn't survive deep sleep -- the pull-up has to
  // be set on the RTC domain directly so ext0 sees a clean idle-high level
  // while the digital GPIO block is powered down.
  rtc_gpio_pullup_en(BTN_PIN);
  rtc_gpio_pulldown_dis(BTN_PIN);
  esp_sleep_enable_ext0_wakeup(BTN_PIN, 0);  // wake on LOW
  esp_deep_sleep_start();
}

void setup() {
  rtc_gpio_hold_dis(PIN_A);
  rtc_gpio_hold_dis(PIN_B);

  pinMode(PIN_A, OUTPUT);
  pinMode(PIN_B, OUTPUT);
  pinMode(BTN_PIN, INPUT_PULLUP);
  digitalWrite(PIN_A, LOW);

  // NVS namespace names are capped at 15 chars -- "hesperus-emitter" (16)
  // silently fails prefs.begin(), which made every getBool/putBool below a
  // no-op returning its default (false / always LOW after reset).
  prefs.begin("hesp-emitter", false);
  bool pinBState = prefs.getBool("pinBState", false);

  // Only a press woke us -- a cold boot/power-on has nothing to debounce.
  if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0) {
    delay(DEBOUNCE_MS);
    if (digitalRead(BTN_PIN) == LOW) {
      pinBState = !pinBState;
      prefs.putBool("pinBState", pinBState);
    }
    // Hold off going back to sleep until the button is released, debounced,
    // so the still-low level doesn't immediately wake us straight back up.
    while (digitalRead(BTN_PIN) == LOW) {
      delay(DEBOUNCE_MS);
    }
  }

  digitalWrite(PIN_B, pinBState);
  prefs.end();

  goToSleep();
}

void loop() {
  // Unreached: setup() always ends in deep sleep, which restarts into
  // setup() again on wake.
}
