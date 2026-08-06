// ----------------------------------------------------------------------------
//  network-health-stats.h — Persistent stall/drop/disconnect counters
// ----------------------------------------------------------------------------
//  Lifetime (survives reboot) counters for the failure modes characterized in
//  NETWORK-TIMING-ISSUE.md's "wsClient.loop() blocking under congestion" and
//  "Wi-Fi power-save vs. battery budget" issues. A real deployment has no
//  serial cable attached for hours to catch these live -- this is what lets
//  an operator check after the fact (GET /status) whether any of the
//  adversarial-smoke-test-only failure modes ever actually fired in the
//  field, without needing to have been watching when it happened.
//
//  Kept deliberately dumb: plain volatile counters (matching
//  networkq_overflow_count's existing style in main.cpp), no mutex -- a torn
//  increment on a rarely-incrementing diagnostic counter is an acceptable
//  cost next to adding lock contention to wsPumpTask's latency-critical path.
//  NVS writes are deferred to loop()'s own dirty-flag check
//  (network_health_flush_if_dirty()), never done inline at an increment site
//  -- flash writes take milliseconds, which would add directly back into
//  wsPumpTask's stall duration if done inside its ws_client_mutex-held
//  region. No throttle on the flush itself: these events are rare by design
//  (that's the whole premise being instrumented), so flushing promptly
//  whenever dirty costs nothing in practice and avoids losing the most
//  recent count to a crash/reboot before a delayed flush would have fired.
// ----------------------------------------------------------------------------
#pragma once

#include <Arduino.h>
#include <Preferences.h>

constexpr const char *NETWORK_HEALTH_NAMESPACE = "net_health";

struct NetworkHealthStats {
  volatile uint32_t stall_count = 0;      // wsClient.loop() blocked > 50ms (wsPumpTask's existing canary threshold)
  volatile uint32_t max_stall_ms = 0;
  volatile uint32_t disconnect_count = 0;  // WStype_DISCONNECTED events
  volatile uint32_t drop_ack_deadline = 0;  // "Event dropped after ack deadline."
  volatile uint32_t drop_max_retries = 0;   // "Event dropped after max retries."
  volatile uint32_t drop_link_down = 0;     // "Link down. Event dropped."
};

inline NetworkHealthStats g_network_health;
inline volatile bool g_network_health_dirty = false;

/// @brief Loads persisted counters from NVS into g_network_health. Call once
/// from setup(), before any task that might increment them starts.
inline void network_health_load() {
  Preferences prefs;
  prefs.begin(NETWORK_HEALTH_NAMESPACE, true);
  g_network_health.stall_count = prefs.getUInt("stall_count", 0);
  g_network_health.max_stall_ms = prefs.getUInt("max_stall_ms", 0);
  g_network_health.disconnect_count = prefs.getUInt("disconnects", 0);
  g_network_health.drop_ack_deadline = prefs.getUInt("drop_ackdl", 0);
  g_network_health.drop_max_retries = prefs.getUInt("drop_maxrt", 0);
  g_network_health.drop_link_down = prefs.getUInt("drop_linkdn", 0);
  prefs.end();
}

/// @brief Persists the current counters to NVS. Only called from
/// network_health_flush_if_dirty() (loop()'s own schedule) -- never call this
/// directly from wsPumpTask or uploadWorkerTask; flash writes take
/// milliseconds and would defeat the point of not blocking those paths.
inline void network_health_save() {
  Preferences prefs;
  prefs.begin(NETWORK_HEALTH_NAMESPACE, false);
  prefs.putUInt("stall_count", g_network_health.stall_count);
  prefs.putUInt("max_stall_ms", g_network_health.max_stall_ms);
  prefs.putUInt("disconnects", g_network_health.disconnect_count);
  prefs.putUInt("drop_ackdl", g_network_health.drop_ack_deadline);
  prefs.putUInt("drop_maxrt", g_network_health.drop_max_retries);
  prefs.putUInt("drop_linkdn", g_network_health.drop_link_down);
  prefs.end();
  g_network_health_dirty = false;
}

/// @brief Call once per loop() iteration. No-ops unless a counter has
/// changed since the last flush.
inline void network_health_flush_if_dirty() {
  if (g_network_health_dirty) {
    network_health_save();
  }
}

/// @brief Zeroes every counter and persists immediately. An explicit,
/// operator-triggered action (serial `netstats clear` / HTTP
/// `POST /status/reset`) -- rare and deliberate, unlike the increment
/// sites, so a synchronous NVS write here is fine.
inline void network_health_clear() {
  g_network_health = NetworkHealthStats();
  network_health_save();
}
