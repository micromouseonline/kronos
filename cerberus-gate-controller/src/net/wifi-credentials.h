// ----------------------------------------------------------------------------
//  wifi-credentials.h — Persists user-provisioned Wi-Fi credentials to NVS
//  (via Preferences), so a network chosen through net/wifi-provisioning.h's
//  config portal survives reboot. Mirrors the Preferences pattern already
//  used by display/touch-calibration.h.
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

/// @brief Wipes any saved credentials, so the next boot falls back to
/// secrets.h and (if that fails to connect) re-enters provisioning.
inline void wifi_credentials_clear() {
  Preferences prefs;
  prefs.begin(WIFI_CREDENTIALS_NAMESPACE, false);
  prefs.clear();
  prefs.end();
}
