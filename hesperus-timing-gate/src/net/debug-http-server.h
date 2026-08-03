// ----------------------------------------------------------------------------
//  debug-http-server.h — On-demand diagnostics HTTP server
// ----------------------------------------------------------------------------
//  Purely additive: does not touch the event-reporting path (that stays on
//  feature_ws_client's WebSocketsClient, see main.cpp). Backed by
//  feature_http (esp32async/AsyncTCP + ESPAsyncWebServer, #if HAS_HTTP),
//  the same stack already proven by cerberus/ares -- used here only for a
//  small read-only diagnostics surface: GET /logs (recent debug-log lines,
//  captured via debug-log.h's debug_log_line_hook) and GET /status (uptime,
//  RSSI, queue depth, overflow count).
//
//  No write/MAINTENANCE-mode routes -- unlike cerberus's still-unimplemented
//  planned log-streaming feature (docs/PLANNED-UPDATES.md), hesperus has no
//  SD card to protect, so there's nothing that needs locking out.
// ----------------------------------------------------------------------------
#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include "debug-log.h"  // serial_write_lock/unlock, debug_log_line_hook

constexpr size_t DEBUG_LOG_RING_LINES = 150;      // ~19KB at LINE_MAX below -- see
                                                   // platformio.ini's Track-2 RAM sizing note;
                                                   // conservative pending a measured check on
                                                   // the (non-PSRAM) C3 envs.
constexpr size_t DEBUG_LOG_RING_LINE_MAX = 128;

struct DebugLogRing {
  char lines[DEBUG_LOG_RING_LINES][DEBUG_LOG_RING_LINE_MAX];
  size_t next_slot = 0;
  size_t count = 0;
};
inline DebugLogRing g_debug_log_ring;

/// @brief Appends one already-timestamp-prefixed line (debug_log_line_hook's
/// contract, see debug-log.h). Called from inside debug_log_emit()'s
/// existing serial_write_mutex critical section, so no separate lock here.
inline void debug_log_ring_append(const char *line) {
  strlcpy(g_debug_log_ring.lines[g_debug_log_ring.next_slot], line, DEBUG_LOG_RING_LINE_MAX);
  g_debug_log_ring.next_slot = (g_debug_log_ring.next_slot + 1) % DEBUG_LOG_RING_LINES;
  if (g_debug_log_ring.count < DEBUG_LOG_RING_LINES) {
    g_debug_log_ring.count++;
  }
}

// Registered from setup() rather than extern-declared, so this header has no
// compile-time dependency on main.cpp's exact global layout.
inline const char *g_status_gate_id = "";
inline QueueHandle_t g_status_network_queue = nullptr;
inline volatile uint32_t *g_status_overflow_count = nullptr;

/// @brief Wires this header's /status route to main.cpp's live state and
/// installs the ring-buffer capture as debug-log.h's line hook. Call once
/// from setup(), after gate_id/networkQueue/networkq_overflow_count exist.
inline void debug_http_server_register_status_sources(const char *gate_id, QueueHandle_t network_queue,
                                                        volatile uint32_t *overflow_count) {
  g_status_gate_id = gate_id;
  g_status_network_queue = network_queue;
  g_status_overflow_count = overflow_count;
  debug_log_line_hook = debug_log_ring_append;
}

inline void handle_debug_logs(AsyncWebServerRequest *request) {
  String out;
  out.reserve(DEBUG_LOG_RING_LINES * 48);
  serial_write_lock();  // same mutex debug-log.h already uses for Serial writes
  size_t start = (g_debug_log_ring.count < DEBUG_LOG_RING_LINES) ? 0 : g_debug_log_ring.next_slot;
  for (size_t i = 0; i < g_debug_log_ring.count; i++) {
    size_t idx = (start + i) % DEBUG_LOG_RING_LINES;
    out += g_debug_log_ring.lines[idx];
    out += "\n";
  }
  serial_write_unlock();
  request->send(200, "text/plain", out);
}

inline void handle_debug_status(AsyncWebServerRequest *request) {
  JsonDocument doc;
  doc["gate_id"] = g_status_gate_id;
  doc["uptime_ms"] = millis();
  doc["rssi"] = WiFi.RSSI();
  doc["network_queue_depth"] = g_status_network_queue ? uxQueueMessagesWaiting(g_status_network_queue) : 0;
  doc["network_overflow_count"] = g_status_overflow_count ? *g_status_overflow_count : 0;
  String out;
  serializeJson(doc, out);
  request->send(200, "application/json", out);
}

inline AsyncWebServer debug_http_server(80);

/// @brief Registers routes (once) and (re)starts listening. Call on every
/// Wi-Fi-connect edge, not just once -- AsyncServer::begin() silently no-ops
/// if its internal listening PCB is still non-null after a Wi-Fi drop (same
/// gotcha cerberus's net/wifi-manager.h documents for its own server
/// restart), so a drop/reconnect without a re-begin() would leave this
/// server unresponsive after the first drop.
inline void debug_http_server_init() {
  static bool initialized = false;
  if (!initialized) {
    debug_http_server.on("/logs", HTTP_GET, handle_debug_logs);
    debug_http_server.on("/status", HTTP_GET, handle_debug_status);
    initialized = true;
  }
  debug_http_server.begin();
}
