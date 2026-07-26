// ----------------------------------------------------------------------------
//  wifi-provisioning.h — Config-portal fallback for deploying to a network
//  whose credentials aren't baked into secrets.h. wifi-manager.h calls
//  wifi_provisioning_start() (via the wifi_on_provisioning_needed hook, wired
//  up in main.cpp) once it's been unable to connect for too long, and
//  eez-actions.cpp's action_on_menu_setup wipes saved credentials and reboots
//  into the same flow on demand.
//
//  Runs WIFI_AP-only (no concurrent STA retry) -- deliberately one-way for
//  the duration of this boot: the only way out is submitting the form below
//  (which reboots) or a manual power cycle. Avoids needing separate logic to
//  detect the old network coming back and tear the AP down again.
// ----------------------------------------------------------------------------
#pragma once

#include <ESPAsyncWebServer.h>
#include <WiFi.h>

#include "debug-log.h"
#include "display/display.h"  // LGFX
#include "net/http-server.h"  // http_server, generate_html_head(), COMMON_STYLE
#include "net/wifi-credentials.h"

// Fixed AP name/password for the config portal -- printed on-screen (see
// wifi_provisioning_start() below) so whoever is deploying the unit can read
// it straight off the device, no separate documentation needed.
const char* const WIFI_PROVISIONING_AP_SSID = "CERBERUS-SETUP";
const char* const WIFI_PROVISIONING_AP_PASSWORD = "cerberus-setup";

// Set once wifi_provisioning_start() has drawn its instructions directly to
// the LCD (bypassing LVGL). main.cpp's loop() checks this and stops pumping
// lvgl_task_handler()/ui_tick() once it's true -- otherwise LVGL's next
// flush of whatever screen was active repaints straight over our raw pixels,
// since both are writing to the same panel and nothing else needs LVGL
// running once Wi-Fi/race functionality is moot for the rest of this boot.
inline volatile bool wifi_provisioning_active = false;

inline void wifi_provisioning_handle_form(AsyncWebServerRequest* request) {
  String html;
  html.reserve(1200);
  html += generate_html_head("CERBERUS Wi-Fi Setup");
  html +=
      F("<div class=\"card\"><h1>Wi-Fi Setup</h1>"
        "<form method=\"POST\" action=\"/wifi\">"
        "<p>Network name<br><input name=\"ssid\" maxlength=\"32\" required></p>"
        "<p>Password<br><input name=\"pass\" type=\"password\" maxlength=\"64\"></p>"
        "<p><button type=\"submit\">Save &amp; Reboot</button></p>"
        "</form></div></body></html>");
  request->send(200, "text/html", html);
}

inline void wifi_provisioning_handle_save(AsyncWebServerRequest* request) {
  if (!request->hasArg("ssid")) {
    request->send(400, "text/plain", "Missing ssid");
    return;
  }
  String new_ssid = request->arg("ssid");
  String new_pass = request->arg("pass");
  wifi_credentials_save(new_ssid.c_str(), new_pass.c_str());
  debug_printf("[SYSTEM] Wi-Fi credentials saved for \"%s\", rebooting\n", new_ssid.c_str());

  String html;
  html.reserve(400);
  html += generate_html_head("CERBERUS Wi-Fi Setup");
  html += F("<div class=\"card\"><h1>Saved</h1><p>Rebooting and connecting...</p></div></body></html>");
  request->send(200, "text/html", html);

  // Let the response above actually flush before the reboot tears the
  // connection down.
  delay(500);
  ESP.restart();
}

/// @brief Drops onto a WIFI_AP-only config portal: draws AP name/password/IP
/// directly on the LCD (bypassing LVGL, same as touch-calibration.h's
/// re_calibrate()) and serves a save-and-reboot form on the existing
/// http_server. Returns once the portal is up; from then on the AsyncWebServer
/// runs it independently, so the caller (wifi-manager.h's wifi_connect_task)
/// has nothing further to do and deletes its own task.
inline void wifi_provisioning_start(LGFX& lcd) {
  wifi_provisioning_active = true;

  WiFi.mode(WIFI_AP);
  WiFi.softAP(WIFI_PROVISIONING_AP_SSID, WIFI_PROVISIONING_AP_PASSWORD);

  lcd.fillScreen(TFT_BLACK);
  lcd.setTextColor(TFT_WHITE);
  lcd.setTextSize(2);
  int y = lcd.height() / 2 - 40;
  lcd.drawCenterString("Wi-Fi Setup Needed", lcd.width() / 2, y);
  y += 30;
  lcd.drawCenterString(String("Join: ") + WIFI_PROVISIONING_AP_SSID, lcd.width() / 2, y);
  y += 25;
  lcd.drawCenterString(String("Pass: ") + WIFI_PROVISIONING_AP_PASSWORD, lcd.width() / 2, y);
  y += 25;
  lcd.drawCenterString("Then browse to 192.168.4.1", lcd.width() / 2, y);
  y += 35;
  lcd.setTextSize(1);
  lcd.drawCenterString("Hold TOUCH to cancel & reboot", lcd.width() / 2, y);

  // No http_server_restart() here -- http_server_init() already bound this
  // server to IP_ADDR_ANY back in setup(), before Wi-Fi ever connected, so
  // it keeps listening across the STA->AP interface swap with no restart
  // needed. Calling end()+begin() right after WiFi.mode(WIFI_AP) instead hit
  // a real lwIP/AsyncTCP race (bind error -8 / EADDRINUSE, seen on-device)
  // where the old listening PCB hadn't finished releasing yet, so the
  // rebind failed and nothing was left listening at all.
  http_server.on("/wifi", HTTP_GET, wifi_provisioning_handle_form);
  http_server.on("/wifi", HTTP_POST, wifi_provisioning_handle_save);
}
