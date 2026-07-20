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

#include "debug-log.h"
#include "display/display.h"  // LGFX
#include "input-events.h"     // input_queue_post, InputSource, ButtonID

// EventType defaults to InputEventType::PRESSED if not provided
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

// Long-press callbacks (registered on btn_arm/btn_start/btn_goal's
// LV_EVENT_LONG_PRESSED in screens.c): post into the shared queue like
// every other producer's hold gesture. BUTTON_COMMAND_MAP
// (race/race-command-source.h) decides what each HELD event means
void action_on_timer_arm_long(lv_event_t *e) {
  input_queue_post(BTN_ARM, InputSource::TOUCH, InputEventType::HELD);
}

void action_on_timer_start_long(lv_event_t *e) {
  input_queue_post(BTN_START, InputSource::TOUCH, InputEventType::HELD);
}

void action_on_timer_goal_long(lv_event_t *e) {
  input_queue_post(BTN_GOAL, InputSource::TOUCH, InputEventType::HELD);
}

void action_on_timer_touch_long(lv_event_t *e) {
  input_queue_post(BTN_TOUCH, InputSource::TOUCH, InputEventType::HELD);
}

// Menu -> maze timer screen navigation. UI navigation only, not a
// ButtonID/input_queue_post event.
void action_on_menu_maze_timer(lv_event_t *e) {
  loadScreen(SCREEN_ID_MAIN);
}

void action_on_menu_calibrate(lv_event_t *e) {
  debug_println("CALIBRATE!");
}
