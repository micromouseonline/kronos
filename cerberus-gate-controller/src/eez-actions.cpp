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
#include "actions.h"
#include "screens.h"
#include "ui.h"

#include "display.h"          // LGFX
#include "input-events.h"     // input_queue_post, InputSource, ButtonID

void action_on_timer_arm(lv_event_t *e) {
  input_queue_post(BTN_ARM, InputSource::TOUCH);
}

void action_on_timer_start(lv_event_t *e) {
  input_queue_post(BTN_START, InputSource::TOUCH);
}

void action_on_timer_goal(lv_event_t *e) {
  input_queue_post(BTN_GOAL, InputSource::TOUCH);
}

// Long-press on ARM -> RESET, matching gpio-buttons.h's existing convention
// of making RESET the harder-to-reach action (there it's button C's long
// press; here it's the same idea via the ARM button).
void action_on_timer_arm_long(lv_event_t *e) {
  input_queue_post(BTN_RESET, InputSource::TOUCH);
}

// Menu -> maze timer screen navigation. UI navigation only, not a
// ButtonID/input_queue_post event.
void action_on_menu_maze_timer(lv_event_t *e) {
  loadScreen(SCREEN_ID_MAIN);
}

// TODO: what should tapping/long-pressing btn_touch (main/timer screen) do?
// Not obvious from the generated names alone -- doesn't map to an existing
// ButtonID the way arm/start/goal/arm_long do. Left unimplemented rather
// than guessing.
void action_on_timer_touch(lv_event_t *e) {
}

void action_on_timer_touch_long(lv_event_t *e) {
}
