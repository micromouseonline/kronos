// ----------------------------------------------------------------------------
//  input-events.h — Event queue for *local hardware* input producers only
//  (touch, GPIO buttons, NeoKey -- see InputSource below). Producers call
//  input_queue_post(); main.cpp's loop() calls input_queue_drain
//  (input_event_handler) once per iteration, which maps ButtonID/
//  InputEventType to a RaceCommand via race_command_from_button()
//  (race/race-command-source.h) and calls race_timer_handle_command()
//  (race/race-timer.h) directly.
//
//  Remote producers (the legacy serial protocol, HTTP) do NOT go through
//  this queue or InputSource -- they carry payload types (mouse name,
//  gate_id, remote timestamp) this file's ButtonID/InputSource pair can't
//  hold, and post through a separate Main Event Queue instead (SystemEvent,
//  race/system-event-queue.h; see net/serial-protocol.h and
//  net/http-server.h for the two producers). Both queues converge on the
//  same race_timer_handle_command() entry point; the state machine itself
//  never needs to know which queue an event came from.
// ----------------------------------------------------------------------------
#pragma once

#include <Arduino.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include "debug-log.h"

enum ButtonID { BTN_ARM, BTN_START, BTN_GOAL, BTN_TOUCH, NUM_BUTTONS };

enum class InputSource {
  TOUCH,
  GPIO_BUTTON,
  NEOKEY_BUTTON,  // 4-button Adafruit NeoKey 1x4 (seesaw, I2C)
  NUM_SOURCES
};

// PRESSED: a normal short press edge (existing behaviour). HELD: the button
// has been down continuously for at least a producer-defined hold threshold
// (e.g. NEOKEY_LONG_PRESS_MS) -- fired once per hold, not repeated per poll.
enum class InputEventType { PRESSED, HELD };

struct InputEvent {
  ButtonID id;
  InputSource source;
  InputEventType type;
  // Microsecond-resolution, matching the eventual Serial/HTTP producers'
  // gate-hardware clocks (a local free-running counter and a
  // WiFi-TSF-synced counter -- millisecond accuracy is all the race timer
  // needs, but the full range is kept here so nothing is lost before it's
  // divided down). For every current (local) producer these are
  // identical, both read from esp_timer_get_time(): there's no TSF/local
  // distinction to make until a remote producer exists that can actually
  // supply divergent values.
  uint64_t tsf_time;
  uint64_t local_time;
  void debug_print() {
    // Lookup tables for names matching enum ordering
    static const char* button_names[] = {"ARM", "START", "GOAL", "TOUCH"};
    static const char* source_names[] = {"TOUCH", "GPIO_BUTTON", "NEOKEY_BUTTON"};
    static const char* type_names[] = {"PRESSED", "HELD"};

    // Bounds checking to prevent undefined behavior if an invalid enum is passed
    const char* btn_str = (id >= 0 && id < NUM_BUTTONS) ? button_names[id] : "UNKNOWN_BTN";

    int src_idx = int(source);
    const char* src_str = (src_idx >= 0 && src_idx < int(InputSource::NUM_SOURCES)) ? source_names[src_idx] : "UNKNOWN_SRC";

    const char* type_str = type_names[int(type)];

    char buf[96];  // Increased buffer size to prevent truncation
    snprintf(buf, sizeof(buf), "EVT: %s, %s, %s, tsf=%llu local=%llu us\n", btn_str, src_str, type_str,
             (unsigned long long)tsf_time, (unsigned long long)local_time);
    // ::-qualified: this is a member function also named debug_print(), so
    // unqualified lookup would find itself (0-arg) instead of the global
    // free function (1-arg) and fail to compile.
    ::debug_print(buf);
  }
};

inline QueueHandle_t xInputQueue = nullptr;

inline void input_queue_init() {
  xInputQueue = xQueueCreate(8, sizeof(InputEvent));
}

// Thread-safe: usable from a plain polling loop, a FreeRTOS task, or an ISR
// context in the future (via xQueueSendFromISR) without changing consumers.
inline void input_queue_post(ButtonID id, InputSource source, InputEventType type = InputEventType::PRESSED) {
  uint64_t now = static_cast<uint64_t>(esp_timer_get_time());
  InputEvent evt{id, source, type, now, now};
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
