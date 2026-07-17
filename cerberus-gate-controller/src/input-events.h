// ----------------------------------------------------------------------------
//  input-events.h — Generic input-event queue shared by every input producer:
//  button device / WiFi messages). Producers call input_queue_post();
//  loop() calls input_queue_drain() once per iteration.
//
//  Drained but not yet dispatched anywhere: this queue used to feed the old
//  Supervisor UI (app-modes.h, removed now that EEZ Studio/LVGL screens own
//  the UI directly). The real consumer will be DESIGN-REQUIREMENT.md's Main
//  Event Queue / race-timing state machine (IMPLEMENTATION-PLAN.md Phase 1),
//  not yet built -- until then, events are posted and discarded.
// ----------------------------------------------------------------------------
#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

enum ButtonID { BTN_ARM, BTN_START, BTN_GOAL, BTN_RESET, NUM_BUTTONS };

enum class InputSource {
  TOUCH,
  GPIO_BUTTON,
  NEOKEY_BUTTON,  // 4-button Adafruit NeoKey 1x4 (seesaw, I2C)
  // WIFI_MESSAGE,  // future: events synthesized from lib/net/messages.h traffic
};

struct InputEvent {
  ButtonID id;
  InputSource source;
  uint32_t timestamp;
  // No `type` field in v1 -- PRESSED is the only event type today. Add one
  // (PRESSED/RELEASED/HELD) if a producer ever needs to distinguish them.
  void debug_print() {
    char buf[32];
    snprintf(buf, 32, "EVT: %2d, %2d, %lu ms\n", int(id), int(source), timestamp);
    Serial.print(buf);
  }
};

inline QueueHandle_t xInputQueue = nullptr;

inline void input_queue_init() {
  xInputQueue = xQueueCreate(8, sizeof(InputEvent));
}

// Thread-safe: usable from a plain polling loop, a FreeRTOS task, or an ISR
// context in the future (via xQueueSendFromISR) without changing consumers.
inline void input_queue_post(ButtonID id, InputSource source) {
  InputEvent evt{id, source, millis()};
  if (xInputQueue != nullptr) {
    xQueueSend(xInputQueue, &evt, 0);
  }
}

// Call once per loop() iteration. Drains all pending events (non-blocking).
// TODO: dispatch to the Main Event Queue / race-timing state machine once
// that exists (see this file's header comment) -- discarded for now.
inline void input_queue_drain() {
  if (xInputQueue == nullptr) {
    return;
  }
  InputEvent evt;
  while (xQueueReceive(xInputQueue, &evt, 0) == pdTRUE) {
    evt.debug_print();
  }
}
