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

enum ButtonID { BTN_ARM, BTN_START, BTN_GOAL, BTN_TOUCH, NUM_BUTTONS };

enum class InputSource {
  TOUCH,
  GPIO_BUTTON,
  NEOKEY_BUTTON,  // 4-button Adafruit NeoKey 1x4 (seesaw, I2C)
  // WIFI_MESSAGE,  // future: events synthesized from lib/net/messages.h traffic
  NUM_SOURCES
};

struct InputEvent {
  ButtonID id;
  InputSource source;
  uint32_t timestamp;
  // No `type` field in v1 -- PRESSED is the only event type today. Add one
  // (PRESSED/RELEASED/HELD) if a producer ever needs to distinguish them.
  void debug_print() {
    // Lookup tables for names matching enum ordering
    static const char* button_names[] = {"ARM", "START", "GOAL", "TOUCH"};
    static const char* source_names[] = {"TOUCH", "GPIO_BUTTON", "NEOKEY_BUTTON"};

    // Bounds checking to prevent undefined behavior if an invalid enum is passed
    const char* btn_str = (id >= 0 && id < NUM_BUTTONS) ? button_names[id] : "UNKNOWN_BTN";

    int src_idx = int(source);
    const char* src_str = (src_idx >= 0 && src_idx < int(InputSource::NUM_SOURCES)) ? source_names[src_idx] : "UNKNOWN_SRC";

    char buf[64];  // Increased buffer size to prevent truncation
    snprintf(buf, sizeof(buf), "EVT: %s, %s, %lu ms\n", btn_str, src_str, (unsigned long)timestamp);
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
// If handler is non-null, each event is also passed to it after the debug
// print -- e.g. race-timer.h's race_timer_handle_input_event(). Keeps this
// file producer/consumer-agnostic; it doesn't need to know what a "race
// timer" is.
inline void input_queue_drain(void (*handler)(const InputEvent&) = nullptr) {
  if (xInputQueue == nullptr) {
    return;
  }
  InputEvent evt;
  while (xQueueReceive(xInputQueue, &evt, 0) == pdTRUE) {
    evt.debug_print();
    if (handler != nullptr) {
      handler(evt);
    }
  }
}
