#include <Arduino.h>
#include <Preferences.h>
#include <freertos/task.h>

// #include "board-config.h"

#include "status-led.h"

#include "display.h"

#include "gpio-buttons.h"
#include "input-events.h"
#include "neokey-buttons.h"
#include "neokey-pixels.h"
#include "touch-calibration.h"

#include "lvgl-bridge.h"

// lib/ui/ -- EEZ Studio generated. Every board gets the display now (see
// boards.ini's feature_lvgl); screens.h is needed here for SCREEN_ID_MAIN,
// used below to skip the touch-only MENU screen on boards with no touch.
#include "screens.h"
#include "ui.h"

StatusLED statusIndicator;
static LGFX lcd;

// Basic palette the NeoKey 1x4 pixels cycle through, once per second, from
// loop() (see neokey-driver.h for the NP_* colour constants).
static constexpr uint32_t NEOKEY_PALETTE[] = {NP_RED, NP_GREEN, NP_BLUE, NP_YELLOW, NP_MAGENTA, NP_CYAN, NP_WHITE, NP_OFF};
static constexpr size_t NEOKEY_PALETTE_LEN = sizeof(NEOKEY_PALETTE) / sizeof(NEOKEY_PALETTE[0]);
static constexpr uint32_t NEOKEY_CYCLE_PERIOD_MS = 1000;

// Local Input Polling Task (Core 1, per DESIGN-REQUIREMENT.md). Owns all
// input-device reads (GPIO + NeoKey; touch is polled internally by LVGL's
// own indev, see lvgl_touch_init()); the main task owns the display and
// must never poll a device from here to avoid double-reads.
static void input_poll_task(void *) {
  const TickType_t period = pdMS_TO_TICKS(INPUT_POLL_PERIOD_MS);
  for (;;) {
    poll_gpio_buttons();
    poll_neokey_buttons();
    vTaskDelay(period);
  }
}

void setup() {
  // get the serial connection kicked off.
  Serial.begin(115200);
  statusIndicator.begin();
  input_queue_init();
  gpio_buttons_init();
  // Non-blocking (see neokey-buttons.h/neokey-driver.h) -- kicks off a
  // background task and returns immediately regardless of whether a
  // physical NeoKey is attached, so it doesn't delay GPIO polling below
  // even in the worst case (~10s detection stall on ESP32-S3 with no
  // module attached).
  init_neokey_buttons();
  lcd.init();  // setting up the display takes 500ms
  lcd.setRotation(LCD_ROTATION);

  ui_init();  // defaults to loadScreen(SCREEN_ID_MENU)
#if !HAS_TOUCH_INPUT
  // No touch, and MENU's only way to reach MAIN is a touch-only nav
  // button -- skip straight to the timer screen. Can't edit ui.c itself,
  // it's regenerated wholesale on every EEZ Studio export.
  loadScreen(SCREEN_ID_MAIN);
#endif
  uint32_t start_time = millis();
  // it may take anything up to 2000ms altogether to get  a serial connection
  while (!Serial && (millis() < 2000)) {
    delay(10);
  }
  uint32_t ready_time = millis();
  // just because the hardware is ready, does not mean the terminal is ready
  // so allow time for that as well
  while (millis() < 2500) {
    yield();
  }
#if HAS_TOUCH_INPUT && TOUCH_NEEDS_CALIBRATION
  // Only resistive touch (XPT2046, both CYD2USB boards) needs this --
  // capacitive touch (FT6336U, CST820) already reports screen-pixel
  // coordinates. Loads stored calibration from NVS, or launches the
  // interactive wizard if none is stored yet. Safe to call here: the input
  // polling task (below) hasn't started yet, so there's no concurrent
  // lcd.getTouch() to race against.
  calibrate(lcd);
#endif
  Serial.println(F("CERBERUS: gate controller"));
  Serial.printf("ready after %dms (display ready at:%dms)\n", ready_time, start_time);
  xTaskCreatePinnedToCore(input_poll_task, "input_poll", 4096, nullptr, 1, nullptr, 1);
}

void loop() {
  yield();
  input_queue_drain();
  lvgl_task_handler();
  ui_tick();

  static uint32_t last_neokey_cycle = millis();
  static size_t neokey_palette_index = 0;
  if (millis() - last_neokey_cycle >= NEOKEY_CYCLE_PERIOD_MS) {
    neokey_set_all(NEOKEY_PALETTE[neokey_palette_index]);
    neokey_palette_index = (neokey_palette_index + 1) % NEOKEY_PALETTE_LEN;
    last_neokey_cycle = millis();
  }

  delay(50);
}
