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
#include "net/wifi-manager.h"
#include "race/race-command-source.h"
#include "race/race-timer.h"
#include "race/system-event-queue.h"

#include "stylesheet.h"

inline AsyncWebServer http_server(80);

// One line per incoming request, gated behind the same sw_debug_verbose
// switch as serial-protocol.h's RX echo -- this is the HTTP-side equivalent
// of that trace. Called from every route handler below (AsyncWebServer has
// no single before-dispatch hook to do this in one place). `body` is only
// passed by the JSON POST handler (http_handle_event) -- GET routes have
// nothing to serialize, so it stays empty for those.
//
// This runs inside the AsyncWebServer request callback, so it relies on
// debug_log_enqueue() being non-blocking -- a blocking Serial write here
// (e.g. serial_write_mutex contended by another task's own debug output)
// adds straight to the client's round-trip time, which can be enough on
// its own to blow through a gate's HTTP client timeout and trigger a
// retry it didn't otherwise need.
inline void http_log_request(AsyncWebServerRequest *request, const String &body = String()) {
  if (!g_debug_verbose_enabled) {
    return;
  }
  if (body.length() > 0) {
    debug_log_enqueue("[HTTP] %s %s from %s body=%s", request->methodToString(), request->url().c_str(),
                       request->client()->remoteIP().toString().c_str(), body.c_str());
  } else {
    debug_log_enqueue("[HTTP] %s %s from %s", request->methodToString(), request->url().c_str(),
                       request->client()->remoteIP().toString().c_str());
  }
}

// Pushes one SSE message per new result
inline AsyncEventSource http_events("/events");

// Persistent connection for gate boards (NETWORK-TIMING-ISSUE.md
// recommendation 1) -- replaces the per-event TCP connect+POST+close cycle
// /api/event still serves. Rides the same AsyncWebServer instance/port, so
// no separate mDNS service or wifi_on_connected hook is needed: http_server's
// existing restart-on-reconnect (http_server_restart() below) carries this
// route through too.
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

// Wired into main.cpp's combined wifi_on_connected handler, not called
// inline in http_server_init() -- configTzTime()'s SNTP setup needs Wi-Fi/
// DNS to actually be reachable, and previously ran synchronously in
// setup() before that was guaranteed. Same category of bug as
// net/mdns.h's mdns_start() (see its header comment): if it ever blocks
// waiting on the network, http_server_init() never reaches
// http_server.begin(), so no route responds at all, not just /time.
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

inline void http_handle_root(AsyncWebServerRequest *request) {
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

// Transport-agnostic core shared by http_handle_event() (below) and
// ws_event_handler()'s WS_EVT_DATA case -- parses the same 4-field
// gate_id/event/tsf_us/gate_us schema (or the RATS V2 info-message
// vocabulary) and dispatches into the race state machine exactly the same
// way regardless of whether it arrived over a POST body or a WS text frame.
// Returns true if the event was recognised (either a race command or an
// info message) and handled. out_tsf_us always reflects the parsed tsf_us
// field regardless of outcome -- callers that ack a specific event back to
// its sender (ws_event_handler()) need this even on a recognised duplicate,
// since the caller can't otherwise see inside evt.
inline bool handle_gate_event_json(JsonObject &body, String &response_json, int &http_status, uint64_t &out_tsf_us) {
  HttpGateEvent evt{};
  strlcpy(evt.gate_id, body["gate_id"] | "", sizeof(evt.gate_id));
  strlcpy(evt.event, body["event"] | "", sizeof(evt.event));
  evt.tsf_us = body["tsf_us"] | 0ULL;
  evt.gate_us = body["gate_us"] | 0ULL;
  out_tsf_us = evt.tsf_us;

  RaceCommand cmd = race_command_from_http(evt);
  if (cmd != RaceCommand::NONE) {
    // A duplicate (gate_id, event, tsf_us) is most likely a retried event
    // whose original ack was lost, not a new event -- skip re-dispatching
    // into the race state machine, but still report success so the caller
    // acks it (see NETWORK-TIMING-ISSUE.md recommendation 6).
    if (!gate_event_is_duplicate(evt.gate_id, evt.event, evt.tsf_us)) {
      system_event_post(cmd, evt.tsf_us, evt.gate_id);
    }
    http_status = 200;
    response_json = "{\"status\":\"ok\"}";
    return true;
  }

  // Not a gate/race event -- try the RATS V2 "info message" vocabulary
  // (ContestName, EventName, AllowedRuns, EntryTimeS, ExtraRun, SetMode,
  // RequestType), the same set net/serial-protocol.h's RX task hands to
  // serial_protocol_handle_info_message(). Reuses that function unchanged:
  // a SerialLine is built from `event`/`value` instead of parsed off the
  // UART, everything downstream is identical to the serial path.
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
    debug_log_enqueue("[HTTP] rejected /api/event: gate_id=\"%s\" event=\"%s\"",
                       (const char *)(body["gate_id"] | ""), (const char *)(body["event"] | ""));
  }
  request->send(http_status, "application/json", response_json);
}

// Global static handler prevents heap re-allocation
inline AsyncCallbackJsonWebHandler api_event_handler("/api/event", http_handle_event);

// AsyncWebSocket event handler for http_ws ("/ws") -- persistent-connection
// counterpart to http_handle_event() above (see NETWORK-TIMING-ISSUE.md
// recommendation 1). Acks a handled event back over the same client (see
// the WS_EVT_DATA branch below) so hesperus's retry mechanism (that doc's
// status section item 1) knows the event landed; combined with
// gate-event-dedup.h's de-duplication, a resend after a lost ack is
// recognised rather than double-processed.
inline void ws_event_handler(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg,
                              uint8_t *data, size_t len) {
  if (type == WS_EVT_CONNECT) {
    // Auto ping/pong every 5s -- gives WS_EVT_DISCONNECT-based detection of
    // a peer that vanishes without a clean close (power loss, WiFi drop),
    // which a plain held-open TCP socket doesn't provide on its own.
    client->keepAlivePeriod(5);
    debug_log_enqueue("[WS] client #%u connected from %s", client->id(), client->remoteIP().toString().c_str());
  } else if (type == WS_EVT_DISCONNECT) {
    debug_log_enqueue("[WS] client #%u disconnected", client->id());
  } else if (type == WS_EVT_ERROR) {
    debug_log_enqueue("[WS] client #%u error", client->id());
  } else if (type == WS_EVT_DATA) {
    // Ack-path timing instrumentation (NETWORK-TIMING-ISSUE.md, "acks not
    // arriving back at hesperus in time" issue) -- captured unconditionally
    // (not gated on g_debug_verbose_enabled) so a beacon-spam stress run
    // doesn't need verbose mode on to get this data.
    uint32_t t_data_recv_ms = debug_timestamp_ms();
    AwsFrameInfo *info = (AwsFrameInfo *)arg;
    // Single-frame text fast path only -- the 4-field event payload is well
    // under any frame-size concern, so no multi-frame reassembly is needed.
    if (info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT) {
      data[len] = 0;  // AsyncWebSocket null-terminates single-frame text payloads
      JsonDocument doc;
      DeserializationError err = deserializeJson(doc, (char *)data);
      if (err) {
        debug_log_enqueue("[WS] JSON parse error from client #%u", client->id());
        return;
      }
      JsonObject body = doc.as<JsonObject>();
      // Same "body=" shape http_log_request() uses -- tools/cerberus_log_stats.py's
      // LINE_RE matches on "body={...}" regardless of the preceding tag, so
      // this keeps that tool working unmodified.
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
        debug_log_enqueue("[WS] rejected: gate_id=\"%s\" event=\"%s\"", (const char *)(body["gate_id"] | ""),
                           (const char *)(body["event"] | ""));
      } else {
        // Ack back over the same client so hesperus's retry mechanism
        // (NETWORK-TIMING-ISSUE.md status section item 1) knows this event
        // landed -- sent whether this was a fresh dispatch or a recognised
        // duplicate (handle_gate_event_json()'s dedup check), since either
        // way cerberus genuinely has this event now. Hand-built literal
        // rather than JsonDocument/serializeJson: a single scalar field, on
        // the shared async_tcp task, doesn't need it.
        char ack[48];
        snprintf(ack, sizeof(ack), "{\"ack_tsf_us\":%llu}", (unsigned long long)tsf_us);
        uint32_t t_ack_dispatch_ms = debug_timestamp_ms();
        client->text(ack);
        uint32_t t_ack_sent_ms = debug_timestamp_ms();
        debug_log_enqueue("[WS-ACK] tsf_us=%llu recv=%u dispatch=%u sent=%u text_ms=%u",
                           (unsigned long long)tsf_us, t_data_recv_ms, t_ack_dispatch_ms, t_ack_sent_ms,
                           (unsigned)(t_ack_sent_ms - t_ack_dispatch_ms));
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