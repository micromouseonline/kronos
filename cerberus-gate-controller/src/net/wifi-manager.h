#pragma once

#include <Arduino.h>
#include "WiFi.h"

#include <esp_wifi.h>
#include "debug-log.h"
#include "neokey/neokey-pixels.h"
#include "secrets.h"

inline bool is_wifi_active() {
  wifi_mode_t mode;
  // Get the current operating mode of the Wi-Fi hardware
  if (esp_wifi_get_mode(&mode) == ESP_OK) {
    return (mode != WIFI_MODE_NULL);
  }
  return false;
}

// NeoKey key 3 (the BTN_TOUCH position) doubles as the Wi-Fi status light --
// none of the 5 target boards has a working onboard status LED (three have
// STATUS_LED=-1, one is missing HAS_LED, and the nominal HAS_NEOPIXEL board's
// pin doesn't light in practice; see boards.ini). main.cpp's
// neokey_reflect_race_state() deliberately leaves key 3 alone so this owns it
// exclusively.
constexpr uint8_t WIFI_STATUS_KEY = 3;

// Optional hook, invoked every time Wi-Fi transitions into the connected
// state -- including the very first connect at boot, not just reconnects.
// Lets other layers react without this file needing to know what they are.
// Currently used by net/http-server.h to force-restart the AsyncWebServer:
// AsyncServer::begin() (AsyncTCP/src/AsyncTCP.cpp) silently no-ops while its
// internal listening PCB is still non-null, so a Wi-Fi drop/reconnect can
// leave the HTTP server's original listening socket orphaned against the
// old network interface state -- observed as the server simply not
// responding after the link came back, same IP or not. Only an explicit
// end() + begin() forces a fresh bind/listen.
inline void (*wifi_on_connected)() = nullptr;

// Core-0 background task: connects, then keeps monitoring and reconnects if
// the link drops, so a race in progress never blocks on Wi-Fi and never
// needs a reboot to rejoin. Runs forever -- created once from setup() by
// wifi_connect_start_async() below. WiFi.mode(WIFI_STA) is main.cpp's
// setup()'s job (called before this task is spawned), not repeated here --
// one owner for Wi-Fi mode avoids the two drifting apart if it's ever
// changed in only one place.
//
// Polls WiFi.status() on a flat 250ms tick and reacts to *edges*
// (was_connected vs connected), rather than "monitor for up to 10s, then
// check once" -- an earlier version's monitor-then-check structure missed
// real connects two ways: (1) if Wi-Fi was already associated (ESP32 caches
// the last AP in NVS) before this task's first check, the whole
// connecting/connected block was skipped, so the very first connect never
// printed anything; (2) if the AP finished associating a moment after the
// 10s monitoring window gave up, the "timed out" branch fired and the
// actual connect then completed silently during the 1s gap before the next
// check -- which then saw an already-connected link and skipped the block
// forever, so that reconnect was never logged either. Edge detection has
// no such blind spot: every transition to connected is caught on whichever
// 250ms tick it happens to land on.
//
// Reconnects use WiFi.reconnect(), not another WiFi.begin(ssid, password) --
// credentials are already applied by the one begin() call below, so
// reconnect() just re-triggers esp_wifi_connect() against the existing
// config instead of re-applying it every retry. Arduino-ESP32 also runs its
// own internal auto-reconnect (visible on serial as "WiFi AutoReconnect
// Running" with CORE_DEBUG_LEVEL raised) independently every ~2.5s once
// begin() has been called once, regardless of anything this task does --
// confirmed by a captured reconnect log where the actual auth+DHCP
// (`WL_CONNECTED` to `Got Same IP`) completed in under a second once the AP
// was reachable again. So the 10s figure below isn't "time needed for a
// handshake" (that's fast); it's just how often this task nudges its own
// extra reconnect() while the AP is genuinely absent, on top of the
// library's own faster retry.
inline void wifi_connect_task(void*) {
  debug_printf("[SYSTEM] My MAC Address %s\n", WiFi.macAddress().c_str());
  debug_printf("[SYSTEM] Connecting to %s\n", ssid);
  WiFi.begin(ssid, password);

  bool was_connected = false;
  bool blink_state = false;
  uint32_t attempt_start = millis();

  for (;;) {
    bool connected = (WiFi.status() == WL_CONNECTED);

    if (connected && !was_connected) {
      esp_wifi_set_ps(WIFI_PS_NONE);
      neokey_set_colour(WIFI_STATUS_KEY, NP_OFF);
      debug_println("[SYSTEM] Wi-Fi Connected!");
      debug_printf("[SYSTEM] IP: %s  RSSI: %d dBm\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());
      if (wifi_on_connected != nullptr) {
        wifi_on_connected();
      }
    } else if (!connected && was_connected) {
      debug_println("[SYSTEM] Wi-Fi connection lost, reconnecting...");
      WiFi.reconnect();
      attempt_start = millis();
    } else if (!connected) {
      blink_state = !blink_state;
      neokey_set_colour(WIFI_STATUS_KEY, blink_state ? NP_CYAN : NP_OFF);
      if (millis() - attempt_start > 10000) {
        debug_println("[SYSTEM] Wi-Fi connect attempt timed out, retrying...");
        WiFi.reconnect();
        attempt_start = millis();
      }
    }

    was_connected = connected;
    vTaskDelay(pdMS_TO_TICKS(250));
  }
}

/// @brief Starts Wi-Fi connect/reconnect as a non-blocking Core-0 task.
/// Call once from setup(); never blocks the caller. Status shows on NeoKey
/// key WIFI_STATUS_KEY (blinking cyan while searching, off once connected).
inline void wifi_connect_start_async() {
  WiFi.setTxPower(WIFI_POWER_11dBm);
  xTaskCreatePinnedToCore(wifi_connect_task, "wifi_connect", 4096, nullptr, 1, nullptr, 0);
}
