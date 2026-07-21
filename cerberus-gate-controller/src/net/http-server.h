// ----------------------------------------------------------------------------
//  http-server.h — Asynchronous HTTP Listener
// ----------------------------------------------------------------------------
#pragma once

#include <ArduinoJson.h>
#include <AsyncJson.h>
#include <ESPAsyncWebServer.h>
#include <time.h>

#include "debug-log.h"
#include "net/wifi-manager.h"
#include "race/race-command-source.h"
#include "race/race-timer.h"
#include "race/system-event-queue.h"

inline AsyncWebServer http_server(80);

// Pushes one SSE message per new result
inline AsyncEventSource http_events("/events");

inline void http_notify_leaderboard_changed() {
  http_events.send("update");
}

inline void http_server_restart() {
  http_server.end();
  http_server.begin();
  debug_println("[SYSTEM] HTTP server restarted after Wi-Fi (re)connect");
}

// Time / NTP Configuration (Europe/London)
const char *ntpServer = "pool.ntp.org";
const char *timeZone = "GMT0BST,M3.5.0/1,M10.5.0/2";

inline String formatTimeNow() {
  struct tm timeinfo;
  // Pass 0 timeout so it never blocks the AsyncWebServer background thread
  if (!getLocalTime(&timeinfo, 0)) {
    return String("Time not available (NTP not synced yet)");
  }

  char buf[32];
  // YYYY-MM-DD HH:MM:SS
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
  return String(buf);
}

inline void handleTime(AsyncWebServerRequest *request) {
  request->send(200, "text/plain", formatTimeNow());
}

const char COMMON_STYLE[] PROGMEM = R"rawliteral(
body { font-family: Arial, sans-serif; margin: 24px; }
.card { padding: 16px; border: 1px solid #ddd; border-radius: 12px; max-width: 420px; }
#t { font-size: 1.6rem; font-weight: 700; }
.small { color: #666; margin-top: 8px; }
)rawliteral";

inline void http_handle_css(AsyncWebServerRequest *request) {
  // Cast to uint8_t* and provide exact length to safely serve PROGMEM data
  AsyncWebServerResponse *response = request->beginResponse(200, "text/css", (const uint8_t *)COMMON_STYLE, sizeof(COMMON_STYLE) - 1);
  response->addHeader("Cache-Control", "max-age=86400");  // Cache in browser for 1 day
  request->send(response);
}

inline void http_handle_root(AsyncWebServerRequest *request) {
  // Simple HTML page that fetches /time every second
  String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>ESP32 Time</title>
  <link rel="stylesheet" href="/style.css">
</head>
<body>
  <div class="card">
    <div>Current time:</div>
    <div id="t">Loading...</div>
    <div class="small">Updates every second</div>
  </div>

  <script>
    async function updateTime(){
      try{
        const r = await fetch('/time', { cache: 'no-store' });
        const text = await r.text();
        document.getElementById('t').textContent = text;
      } catch(e){
        document.getElementById('t').textContent = 'Error';
      }
    }
    updateTime();
    setInterval(updateTime, 1000);
  </script>
</body>
</html>
)rawliteral";

  request->send(200, "text/html", html);
}

inline void http_handle_leaderboard(AsyncWebServerRequest *request) {
  LeaderboardEntry entries[MAX_RESULTS];
  size_t count = race_timer_compute_leaderboard(entries, MAX_RESULTS);

  String html;
  html.reserve(384 + count * 64);
  html +=
      F("<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
        "<title>CERBERUS Leaderboard</title>"
        "<link rel=\"stylesheet\" href=\"/style.css\">"
        "<script>"
        "var hadError=false;"
        "var es=new EventSource('/events');"
        "es.onmessage=function(){location.reload();};"
        "es.onerror=function(){hadError=true;};"
        "es.onopen=function(){if(hadError){location.reload();}};"
        "</script>"
        "</head><body>"
        "<h1>CERBERUS Leaderboard</h1>");
  if (count == 0) {
    html += F("<p>No runs recorded yet.</p>");
  } else {
    html +=
        F("<div class=\"card\">"
          "<table border=\"1\" cellpadding=\"4\" cellspacing=\"0\">"
          "<tr><th>#</th><th>Mouse</th><th>Best Time</th></tr>");
    for (size_t i = 0; i < count; i++) {
      char time_str[16];
      race_timer_format_time(entries[i].best_time_ms, time_str, sizeof(time_str));
      html += "<tr><td>" + String(i + 1) + "</td><td>" + mouse_names[entries[i].mouse_id % NUM_MICE] + "</td><td>" + time_str + "</td></tr>";
    }
    html += F("</table></div>");
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

  system_event_post(cmd, evt.tsf_us, evt.gate_id);
  request->send(200, "application/json", "{\"status\":\"ok\"}");
}

inline void http_server_init() {
  configTzTime(timeZone, ntpServer);

  http_server.on("/style.css", HTTP_GET, http_handle_css);
  http_server.on("/", HTTP_GET, http_handle_root);
  http_server.on("/time", HTTP_GET, handleTime);
  http_server.on("/leaderboard", HTTP_GET, http_handle_leaderboard);

  auto *event_handler = new AsyncCallbackJsonWebHandler("/api/event", http_handle_event);
  event_handler->setMethod(HTTP_POST);
  http_server.addHandler(event_handler);

  http_server.addHandler(&http_events);
  race_timer_on_run_committed = http_notify_leaderboard_changed;
  wifi_on_connected = http_server_restart;

  http_server.begin();
  debug_println("[SYSTEM] HTTP server listening on port 80");
}