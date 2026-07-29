// ----------------------------------------------------------------------------
//  wifi-credentials.h — Persists a serial-provisioned ssid/password to NVS,
//  so `wifi <ssid> <pass>` survives reboot. Mirrors the Preferences pattern
//  used by cerberus-gate-controller/src/net/wifi-credentials.h.
// ----------------------------------------------------------------------------
#pragma once

#include <Arduino.h>
#include <Preferences.h>

const char* const WIFI_CREDENTIALS_NAMESPACE = "wifi-cred";

/// @brief Loads a saved ssid/password from NVS into the given buffers.
/// @return false if no credentials have been saved yet (buffers untouched).
inline bool wifi_credentials_load(char* ssid_out, size_t ssid_len, char* pass_out, size_t pass_len) {
  Preferences prefs;
  prefs.begin(WIFI_CREDENTIALS_NAMESPACE, true);
  bool found = prefs.isKey("ssid");
  if (found) {
    prefs.getString("ssid", ssid_out, ssid_len);
    prefs.getString("pass", pass_out, pass_len);
  }
  prefs.end();
  return found;
}

/// @brief Saves an ssid/password pair to NVS, overwriting anything previous.
inline void wifi_credentials_save(const char* ssid, const char* pass) {
  Preferences prefs;
  prefs.begin(WIFI_CREDENTIALS_NAMESPACE, false);
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
  prefs.end();
}
