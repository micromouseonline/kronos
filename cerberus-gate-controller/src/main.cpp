#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <freertos/task.h>

#include "status-led/status-led.h"

#include "debug-log.h"

#include "display/display.h"
#include "display/lvgl-bridge.h"
#include "display/touch-calibration.h"

#include "gpio-buttons.h"
#include "input-events.h"
#include "neokey-buttons.h"
#include "neokey/neokey-pixels.h"

#include "race/race-command-source.h"
#include "race/race-serial-telemetry.h"
#include "race/race-timer-display.h"
#include "race/race-timer.h"
#include "race/system-event-queue.h"

#include "net/http-server.h"
#include "net/mdns.h"
#include "net/serial-protocol.h"
#include "net/wifi-manager.h"
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

// Reflects race_state onto the NeoKey LEDs. Called after every
// race_timer_handle_command() dispatch, regardless of source, so
// Serial/HTTP-driven transitions stay visible on the physical LEDs exactly
// like local button transitions do -- previously this only ran from
// input_event_handler, so a remote command left the LEDs showing whatever
// state a button had last set, even though race_state itself had moved on.
//
// Key 3 (BTN_TOUCH) is deliberately never touched here -- it's owned
// exclusively by the Wi-Fi status indicator (net/wifi-manager.h's
// WIFI_STATUS_KEY), since none of the target boards has a working onboard
// status LED.
void neokey_reflect_race_state() {
  switch (race_state) {
    case RaceState::CALIBRATE:
      neokey_set_colour(0, NP_OFF);
      neokey_set_colour(1, NP_OFF);
      neokey_set_colour(2, NP_OFF);
      break;

    case RaceState::NEW_MOUSE:
      neokey_set_colour(0, NP_MAGENTA);
      neokey_set_colour(1, NP_MAGENTA);
      neokey_set_colour(2, NP_MAGENTA);
      break;

    case RaceState::WAITING:
      neokey_set_colour(0, NP_GREEN);
      neokey_set_colour(1, NP_GREEN);
      neokey_set_colour(2, NP_GREEN);
      break;

    case RaceState::ARMED:
      neokey_set_colour(0, NP_GREEN);
      neokey_set_colour(1, NP_OFF);
      neokey_set_colour(2, NP_OFF);
      break;

    case RaceState::RUNNING:
      neokey_set_colour(0, NP_OFF);
      neokey_set_colour(1, NP_GREEN);
      neokey_set_colour(2, NP_OFF);
      break;

    case RaceState::GOAL:
      neokey_set_colour(0, NP_OFF);
      neokey_set_colour(1, NP_OFF);
      neokey_set_colour(2, NP_GREEN);
      break;

    default:
      neokey_set_colour(0, NP_BLUE);
      neokey_set_colour(1, NP_BLUE);
      neokey_set_colour(2, NP_BLUE);
      break;
  }
}

// As the input event queue is drained, all events pass through here
// for dispatch.
// The events have been copied from the queue so they are valid through
// the lifetime of this function
void input_event_handler(const InputEvent &evt) {
  // TOUCH held -- from NeoKey, or the touch panel's own LVGL long-press
  // (action_on_timer_touch_long posts here too, same as every other
  // producer): return to the main menu. UI navigation only, not a
  // RaceCommand, so it's handled here rather than through
  // BUTTON_COMMAND_MAP. trigger_touch_lockout() debounces the touch panel
  // for 250ms after the switch regardless of which producer triggered it.
  if (evt.id == BTN_TOUCH && evt.type == InputEventType::HELD) {
    trigger_touch_lockout();
    loadScreen(SCREEN_ID_MENU);
  }
  race_timer_handle_command(race_command_from_button(evt.id, evt.type));
  neokey_reflect_race_state();
  if (race_state == RaceState::CALIBRATE) {
    // Gate test lights up the button when its gate is activated --
    // physical-input-specific (needs evt.id), so it stays here rather than
    // in the shared reflector above.
    neokey_set_colour(evt.id, NP_YELLOW);
  }
}

// SystemEvent handler for the Main Event Queue (Serial/HTTP producers --
// see race/system-event-queue.h). Local buttons don't go through here, they
// call race_timer_handle_command() directly in input_event_handler above;
// both converge on the same state machine entry point.
void system_event_handler(const SystemEvent &evt) {
  race_timer_handle_command(evt.type);
  neokey_reflect_race_state();
}

// Wired to wifi-manager.h's wifi_on_connected hook in setup() below --
// combines every module that needs to react to a Wi-Fi (re)connect, since
// wifi_on_connected is a single function pointer, not a list: assigning it
// separately in both net/http-server.h and net/mdns.h would just have the
// second one silently overwrite the first.
void on_wifi_connected() {
  mdns_start();
  ntp_start();
  http_server_restart();
}

//////////////////////////////////////////////////////////////////////

void setup() {
  // get the serial connection kicked off.
  Serial.begin(115200);
  statusIndicator.begin();
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();

  input_queue_init();
  system_event_queue_init();
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
  wifi_connect_start_async();

  // Binds/listens immediately -- doesn't need an active Wi-Fi connection to
  // start, only to actually be reachable, same reasoning as wifi_connect_
  // start_async() itself not blocking setup().
  http_server_init();
  // mdns_start() (net/mdns.h) needs Wi-Fi to actually have an IP, so it
  // isn't called here inline -- on_wifi_connected() above runs it (and
  // restarts the HTTP server) every time wifi-manager.h's connect loop
  // detects a connect/reconnect, never blocking setup() itself.
  wifi_on_connected = on_wifi_connected;
#if HAS_TOUCH_INPUT && TOUCH_NEEDS_CALIBRATION
  // Only resistive touch (XPT2046, both CYD2USB boards) needs this --
  // capacitive touch (FT6336U, CST820) already reports screen-pixel
  // coordinates. Loads stored calibration from NVS, or launches the
  // interactive wizard if none is stored yet. Safe to call here: the input
  // polling task (below) hasn't started yet, so there's no concurrent
  // lcd.getTouch() to race against.
  calibrate(lcd);
#endif

  debug_println(F("CERBERUS: gate controller"));
  debug_printf("ready after %dms \n", ready_time);
  // Now it is finally safe to fire off the button polling task and run the main loop
  xTaskCreatePinnedToCore(input_poll_task, "input_poll", 4096, nullptr, 1, nullptr, 1);
  // Starts owning the UART for the legacy <type,value> host protocol -- see
  // net/serial-protocol.h. Last, so nothing above needed to worry about
  // sharing Serial with it yet.
  serial_protocol_init();
}

//////////////////////////////////////////////////////////////////////
// In the main loop, all input events are collected by their various monitoring
// tasks and the events added to the input queue.
// The events are all timestamped so there is no particular urgency.
// All we need to do is have an event handler process events from the queue
void loop() {
  input_queue_drain(input_event_handler);
  system_event_queue_drain(system_event_handler);
  race_timer_render();
  race_serial_telemetry_tick();
  lvgl_task_handler();
  ui_tick();
  delay(50);  // calls freeRTOS yield so it is safe to use
}
