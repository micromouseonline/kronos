#include "driver/rtc_io.h"
#include "esp_sleep.h"
#include <Adafruit_NeoPixel.h>
#include <Arduino.h>
#include <Preferences.h>

// --- Hardware Pin Configuration ---
// STATUS_LED and NEOPIXEL_COLOR_ORDER come from the per-board build_flags in
// platformio.ini/boards.ini - do not hardcode a pin/order here, it varies
// per target (e.g. GPIO21/NEO_RGB on ESP32-S3-Zero, GPIO48/NEO_GRB on the
// S3 Super Mini).
Adafruit_NeoPixel led(1, STATUS_LED, NEOPIXEL_COLOR_ORDER + NEO_KHZ800);

const gpio_num_t PIN_A = GPIO_NUM_2;
const gpio_num_t PIN_B = GPIO_NUM_3;
const gpio_num_t BTN_PIN = GPIO_NUM_0;
const int ON = 0;
const int OFF = 1;
const uint32_t DEBOUNCE_MS = 30;

uint8_t pinState = 0;

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
  esp_sleep_enable_ext0_wakeup(BTN_PIN, 0); // wake on LOW
  esp_deep_sleep_start();
}

void setup() {
  rtc_gpio_hold_dis(PIN_A);
  rtc_gpio_hold_dis(PIN_B);

  pinMode(PIN_A, OUTPUT);
  pinMode(PIN_B, OUTPUT);
  pinMode(BTN_PIN, INPUT_PULLUP);
  digitalWrite(PIN_A, LOW);

  led.begin();
  led.setPixelColor(0, led.Color(0, 32, 0)); // Bright Green
  led.show();

  // NVS namespace names are capped at 15 chars -- "hesperus-emitter" (16)
  // silently fails prefs.begin(), which made every getBool/putBool below a
  // no-op returning its default (false / always LOW after reset).
  prefs.begin("hesp-emitter", false);
  uint8_t pinState = prefs.getChar("pinState", 0);

  // Only a press woke us -- a cold boot/power-on has nothing to debounce.
  if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0) {
    delay(DEBOUNCE_MS);
    if (digitalRead(BTN_PIN) == LOW) {
      pinState = (pinState + 1) % 4;
      prefs.putChar("pinState", pinState);
    }
    // Hold off going back to sleep until the button is released, debounced,
    // so the still-low level doesn't immediately wake us straight back up.
    while (digitalRead(BTN_PIN) == LOW) {
      delay(DEBOUNCE_MS);
    }
  }
  switch (pinState) {
  case 0:
    digitalWrite(PIN_A, OFF);
    digitalWrite(PIN_B, OFF);
    break;
  case 1:
    digitalWrite(PIN_A, OFF);
    digitalWrite(PIN_B, ON);
    break;
  case 2:
    digitalWrite(PIN_A, ON);
    digitalWrite(PIN_B, OFF);
    break;
  case 3:
    digitalWrite(PIN_A, ON);
    digitalWrite(PIN_B, ON);
    break;
  }

  prefs.end();

  goToSleep();
}

void loop() {
  // Unreached: setup() always ends in deep sleep, which restarts into
  // setup() again on wake.
}
