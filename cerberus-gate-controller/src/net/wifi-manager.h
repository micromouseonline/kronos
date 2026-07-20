#pragma once

#include <Arduino.h>
#include "WiFi.h"

#include <esp_wifi.h>
#include "debug-log.h"
#include "secrets.h"

inline bool is_wifi_active() {
  wifi_mode_t mode;
  // Get the current operating mode of the Wi-Fi hardware
  if (esp_wifi_get_mode(&mode) == ESP_OK) {
    return (mode != WIFI_MODE_NULL);
  }
  return false;
}

inline void wifi_connect(StatusLED& led) {
  WiFi.begin(ssid, password);
  WiFi.setTxPower(WIFI_POWER_11dBm);
  debug_printf("\n[SYSTEM] My MAC Address %s ", WiFi.macAddress().c_str());
  debug_printf("\n[SYSTEM] Connecting to %s ", ssid);
  led.turnOff();
  while (WiFi.status() != WL_CONNECTED) {
    delay(50);
    led.toggle();
    debug_print(".");
  }

  esp_wifi_set_ps(WIFI_PS_NONE);

  debug_println("\n[SYSTEM] Wi-Fi Connected!");
  debug_printf("[SYSTEM] IP: %s  RSSI: %d dBm\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());
}