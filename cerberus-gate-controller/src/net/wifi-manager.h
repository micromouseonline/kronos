#pragma once

#include <Arduino.h>
#include "WiFi.h"

#include <esp_wifi.h>
#include "debug-log.h"
#include "neokey/neokey-pixels.h"
#include "net/wifi-credentials.h"
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

// Manual-testing aid only -- toggle off to silence the repeating 5s "IP:
// ... RSSI: ..." line while eyeballing a serial terminal by hand (see
// docs/TESTING-SERIAL.md). Doesn't affect the one-shot connect/disconnect
// messages, only the periodic re-report further down in wifi_connect_task.
inline bool g_wifi_rssi_report_enabled = true;

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

// Fired once per boot if the link hasn't connected within
// WIFI_PROVISIONING_TIMEOUT_MS -- wired up in main.cpp to
// wifi_provisioning_start() (net/wifi-provisioning.h), which owns the lcd
// instance this file doesn't have access to. Same "hook set by main.cpp"
// shape as wifi_on_connected above.
inline void (*wifi_on_provisioning_needed)() = nullptr;

// Set by wifi_request_provisioning() (called from eez-actions.cpp's
// action_on_menu_setup) to force provisioning open immediately, regardless
// of whether the current network is connected -- the 60s timeout below only
// covers "can't connect at all", which never fires if the compiled-in
// secrets.h network is still in range (e.g. on a dev bench), so a manual
// request needs its own unconditional path into the same hand-off.
inline volatile bool wifi_provisioning_requested = false;

/// @brief Forces Wi-Fi provisioning mode open on wifi_connect_task's next
/// poll tick (within WIFI_POLL_PERIOD_MS below), whether or not Wi-Fi is
/// currently connected.
inline void wifi_request_provisioning() {
  wifi_provisioning_requested = true;
}

// How long to keep retrying the stored/secrets.h network before giving up
// and dropping into the config portal. Generous enough to ride out a router
// reboot or the venue AP taking a while to come up, short enough that a
// genuinely wrong/absent network doesn't leave the device stuck blinking
// indefinitely with no recovery path other than the physical Setup button.
constexpr uint32_t WIFI_PROVISIONING_TIMEOUT_MS = 60000;

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
  // Saved credentials (net/wifi-credentials.h) take priority over
  // secrets.h's compiled-in default -- once a config-portal submission has
  // stored a real venue network, that's the one to use, not "juno".
  static char stored_ssid[33];
  static char stored_pass[65];
  const char* connect_ssid = ssid;
  const char* connect_pass = password;
  if (wifi_credentials_load(stored_ssid, sizeof(stored_ssid), stored_pass, sizeof(stored_pass))) {
    connect_ssid = stored_ssid;
    connect_pass = stored_pass;
    debug_println("[SYSTEM] Using saved Wi-Fi credentials from NVS");
  }

  debug_printf("[SYSTEM] My MAC Address %s\n", WiFi.macAddress().c_str());
  debug_printf("[SYSTEM] Connecting to %s\n", connect_ssid);
  WiFi.begin(connect_ssid, connect_pass);

  bool was_connected = false;
  bool blink_state = false;
  uint32_t attempt_start = millis();
  uint32_t disconnected_since = millis();
  uint32_t last_ip_report = 0;

  for (;;) {
    bool connected = (WiFi.status() == WL_CONNECTED);

    // Single hand-off point into the config portal (net/wifi-provisioning.h),
    // reached either by the disconnected-too-long timeout below or by a
    // manual wifi_request_provisioning() call -- the latter fires
    // unconditionally, even while currently connected, since
    // action_on_menu_setup's whole point is switching to a *different*
    // network on demand, not waiting for this one to fail first. That call
    // switches WiFi.mode() to AP-only, so this task has nothing further to
    // do; deleting it rather than looping avoids it fighting the portal with
    // pointless reconnect() calls against a now-AP-mode radio.
    if (wifi_provisioning_requested || (!connected && millis() - disconnected_since > WIFI_PROVISIONING_TIMEOUT_MS)) {
      debug_println("[SYSTEM] Entering Wi-Fi provisioning mode");
      if (wifi_on_provisioning_needed != nullptr) {
        wifi_on_provisioning_needed();
      }
      vTaskDelete(nullptr);
    }

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
      disconnected_since = millis();
    } else if (!connected) {
      blink_state = !blink_state;
      neokey_set_colour(WIFI_STATUS_KEY, blink_state ? NP_CYAN : NP_OFF);
      if (millis() - attempt_start > 10000) {
        debug_println("[SYSTEM] Wi-Fi connect attempt timed out, retrying...");
        WiFi.reconnect();
        attempt_start = millis();
      }
    }

    // Repeats every 5s for as long as the link stays up -- a one-shot print
    // on the connect edge above is easy to miss if a serial monitor
    // reattaches a moment late after a reboot (e.g. ESP32-S3 native USB CDC
    // re-enumerating), which is exactly when confirming "did it actually
    // join the new network" matters most.
    if (connected && g_wifi_rssi_report_enabled) {
      if (millis() - last_ip_report > 5000) {
        debug_printf("[SYSTEM] IP: %s  RSSI: %d dBm\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());
        last_ip_report = millis();
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
