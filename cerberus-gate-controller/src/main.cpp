#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <freertos/task.h>

#include "status-led/status-led.h"

#include "display/display.h"
#include "display/lvgl-bridge.h"
#include "display/touch-calibration.h"

#include "gpio-buttons.h"
#include "input-events.h"
#include "neokey-buttons.h"
#include "neokey/neokey-pixels.h"

#include "race/race-command-source.h"
#include "race/race-timer-display.h"
#include "race/race-timer.h"

#include "wifi-scan.h"

// lib/ui/ -- EEZ Studio generated.
#include "ui/screens.h"
#include "ui/ui.h"

StatusLED statusIndicator;
static LGFX lcd;

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

// As the input event queue is drained, all events pass through here
// for dispatch.
// The events have been copied from the queue so they are valid through
// the lifetime of this function
void input_event_handler(const InputEvent &evt) {
  race_timer_handle_command(race_command_from_button(evt.id));
  switch (race_state) {
    case RaceState::CALIBRATE:
      neokey_set_colours({NP_OFF, NP_OFF, NP_OFF, NP_OFF});
      // Gate test lights up button when gate activated
      neokey_set_colour(evt.id, NP_YELLOW);
      break;

    case RaceState::NEW_MOUSE:
      neokey_set_colours({NP_MAGENTA, NP_MAGENTA, NP_MAGENTA, NP_MAGENTA});
      break;

    case RaceState::WAITING:
      neokey_set_colours({NP_GREEN, NP_GREEN, NP_GREEN, NP_GREEN});
      break;

    case RaceState::ARMED:
      neokey_set_colours({NP_GREEN, NP_OFF, NP_OFF, NP_OFF});
      break;

    case RaceState::RUNNING:
      neokey_set_colours({NP_OFF, NP_GREEN, NP_OFF, NP_OFF});
      break;

    case RaceState::GOAL:
      neokey_set_colours({NP_OFF, NP_OFF, NP_GREEN, NP_OFF});
      break;

    default:
      neokey_set_colours({NP_BLUE, NP_BLUE, NP_BLUE, NP_BLUE});
      break;
  }
}

//////////////////////////////////////////////////////////////////////

void setup() {
  // get the serial connection kicked off.
  Serial.begin(115200);
  statusIndicator.begin();
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();

  input_queue_init();
  gpio_buttons_init();
  neokey_buttons_init();
  lcd.init();                     // setting up the display takes about 500ms
  lcd.setRotation(LCD_ROTATION);  // USB port on left
  lvgl_display_init(lcd);         // calls lv_init() -- must run before any lv_obj_* call
  lvgl_touch_init(lcd);
  // Put this in your setup/init function after LVGL initialization
  // LVGL v8.4 Runtime Long-Press Modification
  lv_indev_t *indev = lv_indev_get_next(NULL);
  while (indev) {
    // In V8, type is accessed via the driver structure attached to the indev
    if (indev->driver->type == LV_INDEV_TYPE_POINTER) {
      indev->driver->long_press_time = 1500;  // Time in milliseconds (e.g., 1 second)
    }
    indev = lv_indev_get_next(indev);
  }

  ui_init();  // defaults to loadScreen(SCREEN_ID_MENU)
  race_timer_display_init();
#if !HAS_TOUCH_INPUT
  // No touch, and MENU's only way to reach MAIN is a touch-only nav
  // button -- skip straight to the timer screen. Can't edit ui.c itself,
  // it's regenerated wholesale on every EEZ Studio export.
  loadScreen(SCREEN_ID_MAIN);
#endif
  // it may take anything up to 2000ms altogether to get  a serial connection
  while (!Serial && (millis() < 2000)) {
    delay(10);
  }
  uint32_t ready_time = millis();
  // just because the hardware is ready, does not mean the terminal is ready
  // so allow time for that as well
  while (millis() - ready_time < 500) {
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
  Serial.printf("ready after %dms \n", ready_time);
  // Now it is finally safe to fire off the button polling task and run the main loop
  xTaskCreatePinnedToCore(input_poll_task, "input_poll", 4096, nullptr, 1, nullptr, 1);
}

//////////////////////////////////////////////////////////////////////
// In the main loop, all input events are collected by their various monitoring
// tasks and the events added to the input queue.
// The events are all timestamped so there is no particular urgency.
// All we need to do is have an event handler process events from the queue
void loop() {
  input_queue_drain(input_event_handler);
  race_timer_render();
  lvgl_task_handler();
  ui_tick();
  delay(50);  // calls freeRTOS yield so it is safe to use
}
