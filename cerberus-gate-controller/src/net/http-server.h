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

inline void http_handle_event(AsyncWebServerRequest *request, JsonVariant &json) {
  JsonObject body = json.as<JsonObject>();
  String body_str;
  serializeJson(body, body_str);
  http_log_request(request, body_str);

  HttpGateEvent evt{};
  strlcpy(evt.gate_id, body["gate_id"] | "", sizeof(evt.gate_id));
  strlcpy(evt.event, body["event"] | "", sizeof(evt.event));
  evt.tsf_us = body["tsf_us"] | 0ULL;
  evt.gate_us = body["gate_us"] | 0ULL;

  RaceCommand cmd = race_command_from_http(evt);
  if (cmd != RaceCommand::NONE) {
    system_event_post(cmd, evt.tsf_us, evt.gate_id);
    request->send(200, "application/json", "{\"status\":\"ok\"}");
    return;
  }

  // Not a gate/race event -- try the RATS V2 "info message" vocabulary
  // (ContestName, EventName, AllowedRuns, EntryTimeS, ExtraRun, SetMode,
  // RequestType), the same set net/serial-protocol.h's RX task hands to
  // serial_protocol_handle_info_message(). Reuses that function unchanged:
  // an HTTP SerialLine is built from `event`/`value` instead of parsed off
  // the UART, everything downstream is identical to the serial path.
  int info_type = http_info_message_type(evt.event);
  if (info_type != -1) {
    SerialLine line{};
    line.type = info_type;
    strlcpy(line.value, body["value"] | "", sizeof(line.value));
    serial_protocol_handle_info_message(line, static_cast<uint64_t>(esp_timer_get_time()));
    request->send(200, "application/json", "{\"status\":\"ok\"}");
    return;
  }

  debug_log_enqueue("[HTTP] rejected /api/event: gate_id=\"%s\" event=\"%s\"", evt.gate_id, evt.event);
  request->send(400, "application/json", "{\"status\":\"error\",\"reason\":\"unrecognised event\"}");
}

// Global static handler prevents heap re-allocation
inline AsyncCallbackJsonWebHandler api_event_handler("/api/event", http_handle_event);

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

    initialized = true;
  }

  http_server.begin();
  debug_log_enqueue("[SYSTEM] HTTP server listening on port 80");
}