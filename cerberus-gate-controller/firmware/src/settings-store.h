// ----------------------------------------------------------------------------
//  settings-store.h — Persists user-toggleable app settings to NVS (via
//  Preferences), mirroring the pattern already used by
//  net/wifi-credentials.h and display/touch-calibration.h. Currently the
//  three Settings-screen switches (race/race-serial-telemetry.h's
//  g_watchdog_tx_enabled, net/wifi-manager.h's g_wifi_rssi_report_enabled,
//  debug-log.h's g_debug_verbose_enabled).
// ----------------------------------------------------------------------------
#pragma once

#include <Arduino.h>
#include <Preferences.h>

const char* const SETTINGS_NAMESPACE = "settings";

/// @brief Loads persisted settings into the given flags. Falls back to each
/// flag's current (compiled-in default) value if nothing's been saved yet.
inline void settings_load(bool &watchdog_enabled, bool &wifi_stats_enabled, bool &debug_verbose_enabled) {
  Preferences prefs;
  prefs.begin(SETTINGS_NAMESPACE, true);
  watchdog_enabled = prefs.getBool("watchdog", watchdog_enabled);
  wifi_stats_enabled = prefs.getBool("wifi_stats", wifi_stats_enabled);
  debug_verbose_enabled = prefs.getBool("debug_verbose", debug_verbose_enabled);
  prefs.end();
}

inline void settings_save_watchdog(bool enabled) {
  Preferences prefs;
  prefs.begin(SETTINGS_NAMESPACE, false);
  prefs.putBool("watchdog", enabled);
  prefs.end();
}

inline void settings_save_wifi_stats(bool enabled) {
  Preferences prefs;
  prefs.begin(SETTINGS_NAMESPACE, false);
  prefs.putBool("wifi_stats", enabled);
  prefs.end();
}

inline void settings_save_debug_verbose(bool enabled) {
  Preferences prefs;
  prefs.begin(SETTINGS_NAMESPACE, false);
  prefs.putBool("debug_verbose", enabled);
  prefs.end();
}
