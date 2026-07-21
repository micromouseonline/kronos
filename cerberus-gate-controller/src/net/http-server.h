// ----------------------------------------------------------------------------
//  http-server.h — Asynchronous HTTP Listener (DESIGN-REQUIREMENT.md): an
//  AsyncWebServer on port 80, running on the Wi-Fi/AsyncTCP stack's own
//  background task (Core 0). `GET /` is the spectator leaderboard page,
//  server-rendered from the same race-timer.h data the on-screen panel
//  uses. `POST /api/event` is the wire contract remote intelligent gates
//  use to report timing events -- JSON body `{gate_id, event, tsf_us,
//  gate_us}`, parsed via AsyncCallbackJsonWebHandler, mapped through
//  race-command-source.h's HTTP_EVENT_COMMAND_MAP, and pushed onto the
//  Main Event Queue (system_event_post()) the same way Serial/local-button
//  producers do. Not gate-controller-python-test-cerberus/server.py, an
//  unrelated GET-based prototype that predates this JSON POST contract.
// ----------------------------------------------------------------------------
#pragma once

#include <AsyncJson.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>

#include "debug-log.h"
#include "race/race-command-source.h"
#include "race/race-timer.h"
#include "race/system-event-queue.h"

inline AsyncWebServer http_server(80);

// Pushes one SSE message per new result -- see race_timer_on_run_committed's
// wiring in http_server_init() below. No per-message payload needed: the
// page's own script just reloads on any message, so an empty body is fine.
inline AsyncEventSource http_events("/events");

inline void http_notify_leaderboard_changed() {
  http_events.send("update");
}

// Full standings (not just race-timer-display.h's on-screen top 5 -- that
// cap exists purely for physical screen space, which doesn't apply here),
// sorted fastest-first same as the on-screen panel, so the on-screen top 5
// is always a prefix of this page. Pushed live via Server-Sent Events
// (http_events above) instead of polling/meta-refresh -- reloads the
// instant a new run is committed, not on a fixed timer -- while still
// staying plain server-rendered HTML, no client-side templating/framework.
inline void http_handle_root(AsyncWebServerRequest *request) {
  LeaderboardEntry entries[MAX_RESULTS];
  size_t count = race_timer_compute_leaderboard(entries, MAX_RESULTS);

  String html;
  html.reserve(384 + count * 64);
  html += F("<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
            "<title>CERBERUS Leaderboard</title>"
            "<script>new EventSource('/events').onmessage=function(){location.reload();};</script>"
            "</head><body>"
            "<h1>CERBERUS Leaderboard</h1>");
  if (count == 0) {
    html += F("<p>No runs recorded yet.</p>");
  } else {
    html += F("<table border=\"1\" cellpadding=\"4\" cellspacing=\"0\">"
              "<tr><th>#</th><th>Mouse</th><th>Best Time</th></tr>");
    for (size_t i = 0; i < count; i++) {
      char time_str[16];
      race_timer_format_time(entries[i].best_time_ms, time_str, sizeof(time_str));
      html += "<tr><td>" + String(i + 1) + "</td><td>" + mouse_names[entries[i].mouse_id % NUM_MICE] + "</td><td>" + time_str +
              "</td></tr>";
    }
    html += F("</table>");
  }
  html += F("</body></html>");
  request->send(200, "text/html", html);
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

  http_server.addHandler(&http_events);
  race_timer_on_run_committed = http_notify_leaderboard_changed;

  http_server.begin();
  debug_println("[SYSTEM] HTTP server listening on port 80");
}
