// ----------------------------------------------------------------------------
//  gpio-buttons.h — Physical-button input producer for boards with no
//  touchscreen (M5Stack Core: buttons A/B/C, active-LOW).
//
//  Mapping (3 buttons -> 4 logical actions):
//    A short-press -> BTN_ARM
//    B short-press -> BTN_START
//    C short-press -> BTN_GOAL
//    C long-press   -> BTN_RESET  (deliberately the harder-to-reach action)
// ----------------------------------------------------------------------------
#pragma once

#include "config.h"
#include "input-events.h"

#if HAS_GPIO_BUTTONS

#include "button.h"

inline DebouncedButton button_a(PIN_BUTTON_A);
inline DebouncedButton button_b(PIN_BUTTON_B);

inline bool c_was_down = false;
inline unsigned long c_press_start_ms = 0;

inline void gpio_buttons_init() {
  button_a.begin();
  button_b.begin();
  pinMode(PIN_BUTTON_C, INPUT_PULLUP);
}

inline void poll_gpio_buttons() {
  if (button_a.wasPressed()) {
    input_queue_post(BTN_ARM, InputSource::GPIO_BUTTON);
  }
  if (button_b.wasPressed()) {
    input_queue_post(BTN_START, InputSource::GPIO_BUTTON);
  }

  // Button C is manually tracked (not via DebouncedButton) so we can time
  // the hold duration and distinguish short-press (GOAL) from long-press (RESET).
  bool c_down = (digitalRead(PIN_BUTTON_C) == LOW);
  if (c_down && !c_was_down) {
    c_press_start_ms = millis();
    c_was_down = true;
  } else if (!c_down && c_was_down) {
    unsigned long held_ms = millis() - c_press_start_ms;
    c_was_down = false;
    if (held_ms >= BUTTON_C_LONG_PRESS_MS) {
      input_queue_post(BTN_RESET, InputSource::GPIO_BUTTON);
    } else {
      input_queue_post(BTN_GOAL, InputSource::GPIO_BUTTON);
    }
  }
}

#else
// if there are no GPIO input buttons, we just have dummy calls.

inline void gpio_buttons_init() {
}

inline void poll_gpio_buttons() {
}

#endif
