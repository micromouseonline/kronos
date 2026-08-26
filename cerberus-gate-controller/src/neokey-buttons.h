// ----------------------------------------------------------------------------
//  neokey-buttons.h — NeoKey 1x4 input producer. Detects key-press edges via
//  neokey-driver.h's shared device instance and posts InputEvents. Polled
//  from the Local Input Polling Task (Core 1, see main.cpp), same contract
//  as gpio-buttons.h.
// ----------------------------------------------------------------------------
#pragma once

#include "config.h"
#include "input-events.h"

#if HAS_NEOKEY_BUTTONS

#include "neokey/neokey-driver.h"

// Non-blocking: hands neokey_init_task() to a background FreeRTOS task
// instead of calling init_neokey_device() directly, so a board with no
// physical NeoKey attached never delays app_setup() (see neokey-driver.h).
inline void neokey_buttons_init() {
  xTaskCreate(neokey_init_task, "neokey_init", 4096, nullptr, 1, nullptr);
}

inline void poll_neokey_buttons() {
  // Checked before taking the lock, not after -- while the background init
  // task is still detecting (isAvailable() still false), it holds
  // neokey_bus_mutex for the full probe/handshake duration. Locking here
  // unconditionally would block this call, and since this runs from the
  // shared Core-1 input_poll_task loop (application.cpp), that stalled
  // every other producer sharing that loop -- confirmed on real hardware
  // as "screen unresponsive" for several seconds after boot with no NeoKey
  // attached, even though the Supervisor screen itself painted immediately.
  if (!neokey_device.isAvailable()) {
    return;
  }
  neokey_bus_lock();
  neokey_device.update();
  neokey_bus_unlock();
  for (int i = 0; i < NUM_BUTTONS; i++) {
    if (neokey_device.wasPressed(i)) {
      input_queue_post(static_cast<ButtonID>(i), InputSource::NEOKEY_BUTTON);
    }
    // Long-press gesture (TOUCH -> menu, ARM -> new mouse; see main.cpp's
    // input_event_handler and race-command-source.h). Checked for all 4
    // keys generically; START/GOAL held is currently a no-op downstream.
    if (neokey_device.wasLongPressed(i, NEOKEY_LONG_PRESS_MS)) {
      input_queue_post(static_cast<ButtonID>(i), InputSource::NEOKEY_BUTTON, InputEventType::HELD);
    }
  }

  // TOUCH's short-press action (main.cpp's input_event_handler) must fire
  // only on a genuine short press, not on the press-down edge a long press
  // (-> main menu) shares with it -- wasPressed() above fires immediately
  // on press-down, before it's known whether the hold will cross
  // NEOKEY_LONG_PRESS_MS. wasReleased() + !wasLongReleased() fires exactly
  // when release happened before that threshold, i.e. exactly the presses
  // that never triggered HELD above. TOUCH-only, not folded into the loop
  // above: race_command_from_button()'s ternary treats anything that isn't
  // HELD as on_press, so a RELEASED event on ARM/START/GOAL would fire
  // that button's press command a second time on release.
  if (neokey_device.wasReleased(BTN_TOUCH) && !neokey_device.wasLongReleased(BTN_TOUCH, NEOKEY_LONG_PRESS_MS)) {
    input_queue_post(BTN_TOUCH, InputSource::NEOKEY_BUTTON, InputEventType::RELEASED);
  }
}

#else

inline void neokey_buttons_init() {
}
inline void poll_neokey_buttons() {
}  // no NeoKey on this board

#endif
