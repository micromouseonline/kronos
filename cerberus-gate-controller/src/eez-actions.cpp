// ----------------------------------------------------------------------------
//  eez-actions.cpp — Implements the action_* callbacks EEZ Studio's
//  generated lib/ui/actions.h declares (button clicks etc. from the
//  EEZ-designed screens). Hand-written and never touched by an EEZ Studio
//  re-export, unlike everything under lib/ui/.
//
//  Unconditional, even on boards with no touch panel (e.g. M5 Core):
//  lib/ui/screens.c registers these as event callbacks at screen-creation
//  time regardless of whether touch input ever actually fires them, so the
//  linker needs them defined on every board once lib/ui/ is referenced.
// ----------------------------------------------------------------------------
#include "ui/actions.h"
#include "ui/screens.h"
#include "ui/ui.h"

#include "display/display.h"  // LGFX
#include "display/lvgl-bridge.h"
#include "input-events.h"  // input_queue_post, InputSource, ButtonID

void action_on_timer_arm(lv_event_t *e) {
  input_queue_post(BTN_ARM, InputSource::TOUCH);
}

void action_on_timer_start(lv_event_t *e) {
  input_queue_post(BTN_START, InputSource::TOUCH);
}

void action_on_timer_goal(lv_event_t *e) {
  input_queue_post(BTN_GOAL, InputSource::TOUCH);
}

void action_on_timer_touch(lv_event_t *e) {
  input_queue_post(BTN_TOUCH, InputSource::TOUCH);
}

void action_on_timer_touch_long(lv_event_t *e) {
  trigger_touch_lockout();
  loadScreen(SCREEN_ID_MENU);
}

// Menu -> maze timer screen navigation. UI navigation only, not a
// ButtonID/input_queue_post event.
void action_on_menu_maze_timer(lv_event_t *e) {
  loadScreen(SCREEN_ID_MAIN);
}

void action_on_menu_calibrate(lv_event_t *e) {
  Serial.println("CALIBRATE!");
}
