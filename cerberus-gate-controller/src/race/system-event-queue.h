// ----------------------------------------------------------------------------
//  system-event-queue.h — Main Event Queue for remote (Serial/HTTP)
//  producers, feeding the same race_timer_handle_command() entry point that
//  local buttons already call directly (see main.cpp's input_event_handler).
//  Local buttons are NOT rerouted through this queue -- both paths converge
//  on the same state machine, so it doesn't need to know which queue an
//  event came from. Same producer/consumer shape as input-events.h's
//  xInputQueue, applied to SystemEvent instead of InputEvent.
// ----------------------------------------------------------------------------
#pragma once

#include <Arduino.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include "race-timer.h"

struct SystemEvent {
  RaceCommand type;
  uint64_t timestamp_us;
  // Mouse name (RaceCommand::NEW_MOUSE) or gate_id (HTTP), depending on
  // source. TODO(mouse-name): not yet threaded through to
  // race_timer_enter_new_mouse() -- see net/serial-protocol.h.
  char payload[32];
};

inline QueueHandle_t xSystemEventQueue = nullptr;

inline void system_event_queue_init() {
  xSystemEventQueue = xQueueCreate(32, sizeof(SystemEvent));
}

// Thread/ISR-safe producer, usable from the Serial RX task, the HTTP
// server's own task, or an ISR context via xQueueSendFromISR.
inline void system_event_post(RaceCommand type, uint64_t timestamp_us, const char *payload = "") {
  SystemEvent evt{};
  evt.type = type;
  evt.timestamp_us = timestamp_us;
  strncpy(evt.payload, payload, sizeof(evt.payload) - 1);
  if (xSystemEventQueue != nullptr) {
    xQueueSend(xSystemEventQueue, &evt, 0);
  }
}

// Call once per loop() iteration. Drains all pending events (non-blocking).
inline void system_event_queue_drain(void (*handler)(const SystemEvent &) = nullptr) {
  if (xSystemEventQueue == nullptr) {
    return;
  }
  SystemEvent evt;
  while (xQueueReceive(xSystemEventQueue, &evt, 0) == pdTRUE) {
    if (handler != nullptr) {
      handler(evt);
    }
  }
}
