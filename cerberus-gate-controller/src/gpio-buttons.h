// ----------------------------------------------------------------------------
//  gpio-buttons.h — Physical-button input producer for boards with no
//  touchscreen (buttons A/B/C, active-LOW). Currently unused -- no board
//  profile enables HAS_GPIO_BUTTONS.
//
//  Mapping (3 buttons -> BTN_ARM/BTN_START/BTN_GOAL, no BTN_TOUCH -- this
//  board has no touchscreen):
//    A short-press -> BTN_ARM,   A long-press -> BTN_ARM   + HELD
//    B short-press -> BTN_START, B long-press -> BTN_START + HELD
//    C short-press -> BTN_GOAL,  C long-press -> BTN_GOAL  + HELD
//
//  Same dual-post pattern as neokey-buttons.h: PRESSED fires on the press
//  edge, HELD fires once mid-hold if still down past
//  GPIO_BUTTON_LONG_PRESS_MS (see button/button.h's wasLongPressed()).
//  Downstream mapping of HELD to a RaceCommand is race-command-source.h's
//  job, shared with every other producer -- currently only ARM-held does
//  anything (RaceCommand::RESTART, matching NeoKey's ARM-held); START/GOAL
//  held are no-ops, also matching NeoKey.
// ----------------------------------------------------------------------------
#pragma once

#include "config.h"
#include "input-events.h"

#if HAS_GPIO_BUTTONS

#include "button/button.h"

inline DebouncedButton button_a(PIN_BUTTON_A);
inline DebouncedButton button_b(PIN_BUTTON_B);
inline DebouncedButton button_c(PIN_BUTTON_C);

inline void gpio_buttons_init() {
  button_a.begin();
  button_b.begin();
  button_c.begin();
}

inline void poll_gpio_buttons() {
  // EventType defaults to InputEventType::PRESSED if not provided
  if (button_a.wasPressed()) {
    input_queue_post(BTN_ARM, InputSource::GPIO_BUTTON);
  }

  if (button_a.wasLongPressed(GPIO_BUTTON_LONG_PRESS_MS)) {
    input_queue_post(BTN_ARM, InputSource::GPIO_BUTTON, InputEventType::HELD);
  }

  if (button_b.wasPressed()) {
    input_queue_post(BTN_START, InputSource::GPIO_BUTTON);
  }

  if (button_b.wasLongPressed(GPIO_BUTTON_LONG_PRESS_MS)) {
    input_queue_post(BTN_START, InputSource::GPIO_BUTTON, InputEventType::HELD);
  }

  if (button_c.wasPressed()) {
    input_queue_post(BTN_GOAL, InputSource::GPIO_BUTTON);
  }

  if (button_c.wasLongPressed(GPIO_BUTTON_LONG_PRESS_MS)) {
    input_queue_post(BTN_GOAL, InputSource::GPIO_BUTTON, InputEventType::HELD);
  }
}

#else
// if there are no GPIO input buttons, we just have dummy calls.

inline void gpio_buttons_init() {
}

inline void poll_gpio_buttons() {
}

#endif
