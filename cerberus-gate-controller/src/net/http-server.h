// ----------------------------------------------------------------------------
//  http-server.h — Asynchronous HTTP Listener
// ----------------------------------------------------------------------------
#pragma once

#include <ArduinoJson.h>
#include <AsyncJson.h>
#include <ESPAsyncWebServer.h>
#include <esp_timer.h>
#include <time.h>

#include "debug-log.h"
#include "net/gate-event-dedup.h"
#include "net/gate-liveness.h"
#include "net/wifi-manager.h"
#include "race/race-command-source.h"
#include "race/race-timer.h"
#include "race/system-event-queue.h"

#include "stylesheet.h"

inline AsyncWebServer http_server(80);

// Log one line per HTTP request when `sw_debug_verbose` is enabled (matches
// serial-protocol.h's RX trace). Called directly by each route handler because
// AsyncWebServer lacks a global pre-dispatch hook. `body` is provided only for JSON POSTs.
//
// Note: Runs inside the AsyncWebServer request callback. Relies on non-blocking
// `debug_log_enqueue()` to prevent Serial lock contention from inflating RTT and
// triggering client timeouts.
inline void http_log_request(AsyncWebServerRequest *request, const String &body = String()) {
  if (!g_debug_verbose_enabled) {
    return;
  }
  if (body.length() > 0) {
    debug_log_enqueue("[HTTP] %s %s from %s body=%s", request->methodToString(), request->url().c_str(), request->client()->remoteIP().toString().c_str(),
                      body.c_str());
  } else {
    debug_log_enqueue("[HTTP] %s %s from %s", request->methodToString(), request->url().c_str(), request->client()->remoteIP().toString().c_str());
  }
}

// Pushes one SSE message per new result
inline AsyncEventSource http_events("/events");

// Persistent WebSocket connection for gate boards (see NETWORK-TIMING-LOG.md, rec #1).
// Replaces the per-event TCP connect+POST+close cycle used by /api/event.
//
// Shares the main AsyncWebServer instance/port.
// Re-uses existing Wi-Fi lifecycle hooks, so `http_server_restart()` automatically
// handles reconnects for this route too.
inline AsyncWebSocket http_ws("/ws");

inline void http_notify_leaderboard_changed() {
  http_events.send("update");
}

inline void http_server_restart() {
  http_server.end();
  http_server.begin();
  debug_log_enqueue("[SYSTEM] HTTP server restarted after Wi-Fi (re)connect");
}

// Time / NTP Configuration (Europe/London)
const char *ntpServer = "pool.ntp.org";
const char *timeZone = "GMT0BST,M3.5.0/1,M10.5.0/2";

// Registered in main.cpp's wifi_on_connected handler (not http_server_init()).
//
// `configTzTime()` requires active Wi-Fi/DNS. Running it synchronously during
// server setup can block execution—preventing `http_server.begin()` from being
// reached and causing all HTTP routes to fail (similar to mdns_start()).
inline void ntp_start() {
  configTzTime(timeZone, ntpServer);
}

inline String formatTimeNow() {
  struct timeval tv;
  // gettimeofday gets system epoch seconds AND microseconds
  if (gettimeofday(&tv, NULL) != 0 || tv.tv_sec < 1000000000ULL) {
    return String("Time not available (NTP not synced yet)");
  }

  struct tm timeinfo;
  localtime_r(&tv.tv_sec, &timeinfo);

  // Convert remaining microseconds to milliseconds (0-999)
  uint16_t ms = tv.tv_usec / 1000;

  char buf[40];
  // Format YYYY-MM-DD HH:MM:SS.mmm
  snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d.%03d", timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday, timeinfo.tm_hour,
           timeinfo.tm_min, timeinfo.tm_sec, ms);

  return String(buf);
}

inline void handleTime(AsyncWebServerRequest *request) {
  http_log_request(request);
  request->send(200, "text/plain", formatTimeNow());
}

inline String generate_html_head(const char *title, const char *extra_head = "") {
  String head;
  head.reserve(1024 + (extra_head ? strlen(extra_head) : 0));
  head +=
      F("<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
        "<title>");
  head += title;
  head += F("</title><style>");
  head += String(COMMON_STYLE);
  head += F("</style>");

  if (extra_head && extra_head[0] != '\0') {
    head += extra_head;
  }

  head += F("</head><body>");
  return head;
}

// Checked by http_handle_root() when "/" is requested.
// Set by wifi-provisioning.h once the config portal is active so 192.168.4.1
// serves the setup form instead of the clock page.
//
// Note: "/" is bound here in http_server_init() before Wi-Fi connects. Because
// AsyncWebServer matches the first-registered handler, we use this boolean hook
// rather than trying to re-register the route later.
inline void (*http_handle_root_override)(AsyncWebServerRequest *) = nullptr;

inline void http_handle_root(AsyncWebServerRequest *request) {
  if (http_handle_root_override != nullptr) {
    http_handle_root_override(request);
    return;
  }
  http_log_request(request);
  // Page syncs offset with ESP32 and runs smooth 60 FPS clock locally
  String html;
  html.reserve(1500);  // to prevent fragmentation
  html += generate_html_head("ESP32 TIME");
  html += R"rawliteral(
  <div class="card">
    <div>Current time:</div>
    <div id="clock">Loading...</div>
    <div class="small">Smooth client clock • Synced with ESP32</div>
  </div>

  <script>
    let serverOffsetMs = 0;
    let isSynced = false;

    async function syncTime(){
      try{
        const start = performance.now();
        const r = await fetch('/time', { cache: 'no-store' });
        const text = await r.text();
        
        if (text.includes("not available")) return;

        const roundTripLatency = (performance.now() - start) / 2;
        // Normalize ISO string space separator for JS Date parsing
        const parsedServerTime = new Date(text.replace(' ', 'T')).getTime();
        
        if (!isNaN(parsedServerTime)) {
          serverOffsetMs = (parsedServerTime + roundTripLatency) - Date.now();
          isSynced = true;
        }
      } catch(e){
        // Silently keep using local offset if a periodic resync fails
      }
    }

    function renderClock(){
      const clockEl = document.getElementById('clock');
      if (!isSynced) {
        clockEl.textContent = 'Syncing...';
      } else {
        const now = new Date(Date.now() + serverOffsetMs);
        const pad = (n, len = 2) => String(n).padStart(len, '0');
        
        const yyyy = now.getFullYear();
        const mm = pad(now.getMonth() + 1);
        const dd = pad(now.getDate());
        const hh = pad(now.getHours());
        const min = pad(now.getMinutes());
        const ss = pad(now.getSeconds());
        const ms = pad(now.getMilliseconds(), 3);

        clockEl.textContent = `${yyyy}-${mm}-${dd} ${hh}:${min}:${ss}.${ms}`;
      }
      requestAnimationFrame(renderClock);
    }

    // Initial sync and start render loop
    syncTime();
    renderClock();

    // Re-sync with ESP32 every 10 seconds to compensate for clock drift
    setInterval(syncTime, 10000);
  </script>
</body>
</html>
)rawliteral";

  request->send(200, "text/html", html);
}

inline void http_handle_leaderboard(AsyncWebServerRequest *request) {
  http_log_request(request);
  LeaderboardEntry entries[MAX_RESULTS];
  size_t count = race_timer_compute_leaderboard(entries, MAX_RESULTS);

  const char SSE_RELOAD_SCRIPT[] =
      "<script>"
      "var hadError=false;"
      "var es=new EventSource('/events');"
      "es.onmessage=function(){location.reload();};"
      "es.onerror=function(){hadError=true;};"
      "es.onopen=function(){if(hadError){location.reload();}};"
      "</script>";

  String html;
  html.reserve(800 + count * 64);

  html += generate_html_head("CERBERUS Leaderboard", SSE_RELOAD_SCRIPT);
  html +=
      F("<div class=\"leaderboard\">"
        "<h1>Leaderboard</h1>");

  if (count == 0) {
    html += F("<p class=\"small\">No runs recorded yet.</p>");
  } else {
    html +=
        F("<table>"
          "<thead><tr><th>#</th><th>Mouse</th><th>Best Time</th></tr></thead><tbody>");
    for (size_t i = 0; i < count; i++) {
      char time_str[16];
      race_timer_format_time(entries[i].best_time_ms, time_str, sizeof(time_str));
      // Highlight the leader row (#1)
      if (i == 0) {
        html += "<tr class=\"leader\">";
      } else {
        html += "<tr>";
      }

      html += "<td>" + String(i + 1) + "</td><td>" + entries[i].name + "</td><td>" + time_str + "</td></tr>";
    }
    html += F("</tbody></table></div>");
  }

  html += F("</body></html>");
  request->send(200, "text/html", html);
}

// Shared transport-agnostic core for HTTP POSTs and WS frames.
// Parses the 4-field schema (or RATS V2 info messages) and dispatches to the
// race state machine.
//
// Returns true if handled. `out_tsf_us` is always populated so callers (like WS)
// can ACK duplicates back to the sender.
inline bool handle_gate_event_json(JsonObject &body, String &response_json, int &http_status, uint64_t &out_tsf_us) {
  HttpGateEvent evt{};
  strlcpy(evt.gate_id, body["gate_id"] | "", sizeof(evt.gate_id));
  strlcpy(evt.event, body["event"] | "", sizeof(evt.event));
  evt.tsf_us = body["tsf_us"] | 0ULL;
  evt.gate_us = body["gate_us"] | 0ULL;
  out_tsf_us = evt.tsf_us;

  RaceCommand cmd = race_command_from_http(evt);
  if (cmd != RaceCommand::NONE) {
    // Skip duplicate events (likely retries from lost ACKs), but return 200/true
    // so the caller can re-ACK (see NETWORK-TIMING-LOG.md, rec #6).
    if (!gate_event_is_duplicate(evt.gate_id, evt.event, evt.tsf_us)) {
      system_event_post(cmd, evt.tsf_us, evt.gate_id);
    }
    http_status = 200;
    response_json = "{\"status\":\"ok\"}";
    return true;
  }

  // Fallback: RATS V2 "info message" vocabulary.
  // Reuses serial path logic by constructing a synthetic SerialLine from `event`/`value`.
  int info_type = http_info_message_type(evt.event);
  if (info_type != -1) {
    SerialLine line{};
    line.type = info_type;
    strlcpy(line.value, body["value"] | "", sizeof(line.value));
    serial_protocol_handle_info_message(line, static_cast<uint64_t>(esp_timer_get_time()));
    http_status = 200;
    response_json = "{\"status\":\"ok\"}";
    return true;
  }

  http_status = 400;
  response_json = "{\"status\":\"error\",\"reason\":\"unrecognised event\"}";
  return false;
}

inline void http_handle_event(AsyncWebServerRequest *request, JsonVariant &json) {
  JsonObject body = json.as<JsonObject>();
  String body_str;
  serializeJson(body, body_str);
  http_log_request(request, body_str);

  String response_json;
  int http_status;
  uint64_t tsf_us = 0;
  bool handled = handle_gate_event_json(body, response_json, http_status, tsf_us);
  if (!handled) {
    debug_log_enqueue("[HTTP] rejected /api/event: gate_id=\"%s\" event=\"%s\"", (const char *)(body["gate_id"] | ""), (const char *)(body["event"] | ""));
  }
  request->send(http_status, "application/json", response_json);
}

// Global static handler prevents heap re-allocation
inline AsyncCallbackJsonWebHandler api_event_handler("/api/event", http_handle_event);

// AsyncWebSocket event handler for http_ws ("/ws") — persistent counterpart
// to http_handle_event().
//
// Key behaviors:
// - Acknowledges received events back to the client via WS_EVT_DATA so Hesperus's
//   retry mechanism knows the event landed.
// - Works with gate-event-dedup.h to handle lost ACKs smoothly: retried events
//   are identified and ignored, preventing duplicate processing.
//
// Ref: NETWORK-TIMING-LOG.md (Recommendation #1 & Status Item #1)
inline void ws_event_handler(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len) {
  if (type == WS_EVT_CONNECT) {
    // Auto ping/pong every 5s to detect ungraceful disconnects (power loss, WiFi drop)
    // via WS_EVT_DISCONNECT, which raw TCP sockets miss.
    client->keepAlivePeriod(5);
    // Optimistically re-associate a reconnecting gate with its role by IP
    // before its next event (see gate-liveness.h).
    gate_liveness_note_client_connect(client->id(), client->remoteIP(), debug_timestamp_ms());
    debug_log_enqueue("[WS] client #%u connected from %s", client->id(), client->remoteIP().toString().c_str());
  } else if (type == WS_EVT_DISCONNECT) {
    gate_liveness_mark_client_disconnected(client->id(), debug_timestamp_ms());
    debug_log_enqueue("[WS] client #%u disconnected", client->id());
  } else if (type == WS_EVT_ERROR) {
    gate_liveness_mark_client_disconnected(client->id(), debug_timestamp_ms());
    debug_log_enqueue("[WS] client #%u error", client->id());
  } else if (type == WS_EVT_DATA) {
    // Ack-path timing instrumentation (NETWORK-TIMING-LOG.md) — captured
    // unconditionally so stress tests don't require verbose mode.
    uint32_t t_data_recv_ms = debug_timestamp_ms();
    AwsFrameInfo *info = (AwsFrameInfo *)arg;
    // Single-frame text fast path — event payload fits comfortably within one frame,
    // so multi-frame reassembly is unnecessary.
    if (info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT) {
      data[len] = 0;  // AsyncWebSocket null-terminates single-frame text payloads
      JsonDocument doc;
      DeserializationError err = deserializeJson(doc, (char *)data);
      if (err) {
        debug_log_enqueue("[WS] JSON parse error from client #%u", client->id());
        return;
      }
      JsonObject body = doc.as<JsonObject>();
      // Liveness tracking: infer role from `event` (roles are never sent explicitly,
      // see gate-liveness.h) and mark client as active. Runs before dispatch, as even
      // duplicate events prove the connection is alive.
      int gate_role = gate_role_from_event((const char *)(body["event"] | ""));
      if (gate_role >= 0) {
        gate_liveness_mark_role_connected(static_cast<GateRole>(gate_role), client->id(), (const char *)(body["gate_id"] | ""), client->remoteIP(),
                                          debug_timestamp_ms());
      }
      // Matches http_log_request() formatting so tools/cerberus_log_stats.py
      // parses `body={...}` without modifications.
      if (g_debug_verbose_enabled) {
        String body_str;
        serializeJson(body, body_str);
        debug_log_enqueue("[WS] DATA from %s body=%s", client->remoteIP().toString().c_str(), body_str.c_str());
      }
      String response_json;
      int http_status;
      uint64_t tsf_us = 0;
      bool handled = handle_gate_event_json(body, response_json, http_status, tsf_us);
      if (!handled) {
        debug_log_enqueue("[WS] rejected: gate_id=\"%s\" event=\"%s\"", (const char *)(body["gate_id"] | ""), (const char *)(body["event"] | ""));
      } else {
        // Ack back over the same client to halt Hesperus retries (fresh or duplicate).
        // Uses a hand-built string instead of JsonDocument to avoid allocation overhead on async_tcp.
        char ack[48];
        snprintf(ack, sizeof(ack), "{\"ack_tsf_us\":%llu}", (unsigned long long)tsf_us);
        // TCP-layer state pre-dispatch instrumentation (NETWORK-TIMING-LOG.md).
        // `!canSend()` means a prior write is un-acked by Hesperus (queued/in-flight below WS layer).
        // Note: Avoid AsyncClient::onAck() here as AsyncWebSocketClient uses it internally;
        // registering a custom callback would overwrite theirs and break message delivery.
        AsyncClient *raw_client = client->client();
        bool ack_path_pending = !raw_client->canSend();
        size_t ack_path_space = raw_client->space();
        uint32_t t_ack_dispatch_ms = debug_timestamp_ms();
        client->text(ack);
        uint32_t t_ack_sent_ms = debug_timestamp_ms();
        debug_log_enqueue("[WS-ACK] tsf_us=%llu recv=%u dispatch=%u sent=%u text_ms=%u pending=%d space=%u", (unsigned long long)tsf_us, t_data_recv_ms,
                          t_ack_dispatch_ms, t_ack_sent_ms, (unsigned)(t_ack_sent_ms - t_ack_dispatch_ms), (int)ack_path_pending, (unsigned)ack_path_space);
      }
    }
  }
}

inline void http_server_init() {
  static bool initialized = false;
  configTzTime(timeZone, ntpServer);
  if (!initialized) {
    http_server.on("/", HTTP_GET, http_handle_root);
    http_server.on("/time", HTTP_GET, handleTime);
    http_server.on("/leaderboard", HTTP_GET, http_handle_leaderboard);

    api_event_handler.setMethod(HTTP_POST);
    http_server.addHandler(&api_event_handler);

    http_server.addHandler(&http_events);
    race_timer_on_run_committed = http_notify_leaderboard_changed;

    http_ws.onEvent(ws_event_handler);
    http_server.addHandler(&http_ws);

    initialized = true;
  }

  http_server.begin();
  debug_log_enqueue("[SYSTEM] HTTP server listening on port 80");
}