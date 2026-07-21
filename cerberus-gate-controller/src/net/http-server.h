// ----------------------------------------------------------------------------
//  http-server.h — Asynchronous HTTP Listener (DESIGN-REQUIREMENT.md): an
//  AsyncWebServer on port 80, running on the Wi-Fi/AsyncTCP stack's own
//  background task (Core 0). `GET /` is a liveness stub; `POST /api/event`
//  is the wire contract remote intelligent gates use to report timing
//  events -- JSON body `{gate_id, event, tsf_us, gate_us}`, parsed via
//  AsyncCallbackJsonWebHandler, mapped through race-command-source.h's
//  HTTP_EVENT_COMMAND_MAP, and pushed onto the Main Event Queue
//  (system_event_post()) the same way Serial/local-button producers do.
//  Not the leaderboard page (Stage H replaces the GET / stub) and not
//  gate-controller-python-test-cerberus/server.py, an unrelated GET-based
//  prototype that predates this JSON POST contract.
// ----------------------------------------------------------------------------
#pragma once

#include <AsyncJson.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>

#include "debug-log.h"
#include "race/race-command-source.h"
#include "race/system-event-queue.h"

inline AsyncWebServer http_server(80);

inline void http_handle_root(AsyncWebServerRequest *request) {
  request->send(200, "text/plain", "CERBERUS OK");
}

inline void http_handle_event(AsyncWebServerRequest *request, JsonVariant &json) {
  JsonObject body = json.as<JsonObject>();

  HttpGateEvent evt{};
  strlcpy(evt.gate_id, body["gate_id"] | "", sizeof(evt.gate_id));
  strlcpy(evt.event, body["event"] | "", sizeof(evt.event));
  evt.tsf_us = body["tsf_us"] | 0ULL;
  evt.gate_us = body["gate_us"] | 0ULL;

  RaceCommand cmd = race_command_from_http(evt);
  if (cmd == RaceCommand::NONE) {
    debug_printf("[HTTP] rejected /api/event: gate_id=\"%s\" event=\"%s\"\n", evt.gate_id, evt.event);
    request->send(400, "application/json", "{\"status\":\"error\",\"reason\":\"unrecognised event\"}");
    return;
  }

  // tsf_us is the gate's own TSF reading, not this device's clock -- see
  // DESIGN-REQUIREMENT.md's dual-clock cross-referencing. gate_us rides
  // along in the JSON body for that same drift-compensation purpose but
  // SystemEvent has no field for it yet (not needed until that
  // cross-referencing is actually implemented).
  system_event_post(cmd, evt.tsf_us, evt.gate_id);
  request->send(200, "application/json", "{\"status\":\"ok\"}");
}

inline void http_server_init() {
  http_server.on("/", HTTP_GET, http_handle_root);

  auto *event_handler = new AsyncCallbackJsonWebHandler("/api/event", http_handle_event);
  event_handler->setMethod(HTTP_POST);
  http_server.addHandler(event_handler);

  http_server.begin();
  debug_println("[SYSTEM] HTTP server listening on port 80");
}
