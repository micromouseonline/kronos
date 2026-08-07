// ----------------------------------------------------------------------------
//  gate-liveness.h — Per-role (start/goal) WS connection-state tracking, for
//  a UI liveness indicator (NETWORK-TIMING-ISSUE.md-adjacent: hesperus's own
//  application-level HEARTBEAT event is deliberately never sent over the
//  wire, see hesperus-timing-gate/src/main.cpp's uploadWorkerTask, so
//  liveness here is keyed on WS connection lifecycle
//  (WS_EVT_CONNECT/DISCONNECT/ERROR), not on staleness-since-last-event —
//  gates sit idle for open-ended periods between races, so a staleness
//  timeout would falsely read "disconnected" during any normal idle gap.
//
//  Role (start vs goal) is never transmitted explicitly by a gate -- it's
//  inferred from which event arrives (ARM/START only ever come from the
//  start-role board, GOAL only from the goal-role board, mirroring
//  hesperus's own board-role.h::board_event_name() in reverse). A
//  WS_EVT_CONNECT alone doesn't carry a payload, so a reconnecting client's
//  role isn't known until its next event -- which could be minutes away
//  given how sparse real triggers are. To avoid a stale "disconnected"
//  reading through that window, remote_ip is remembered per role across
//  disconnects and used to optimistically re-associate a reconnecting
//  client immediately, on WS_EVT_CONNECT, ahead of its next real event.
//
//  g_gate_link[] is plain RAM, deliberately not persisted (NVS/Preferences)
//  across a cerberus reboot -- confirmed 2026-08-07: after a cerberus
//  restart both indicators correctly show red until each gate's next real
//  event, even though the gates themselves never lost power and reconnect
//  within seconds. Left as-is on purpose: an operator restarting cerberus
//  would want to re-test the gates anyway, so that first post-reboot event
//  is a feature (a natural liveness check) rather than a gap to paper over
//  with added flash-write persistence.
// ----------------------------------------------------------------------------
#pragma once

#include <Arduino.h>
#include <cstring>

enum class GateRole { START = 0, GOAL = 1 };
constexpr size_t NUM_GATE_ROLES = 2;

struct GateLinkState {
  bool connected = false;
  char gate_id[32] = "";
  uint32_t client_id = 0;
  IPAddress remote_ip;
  uint32_t last_change_ms = 0;
};

inline GateLinkState g_gate_link[NUM_GATE_ROLES];

/// @brief Maps an `event` string to the role that can only ever produce it.
/// @return 0 (START) for "ARM"/"START", 1 (GOAL) for "GOAL", -1 otherwise
/// (RESTART/NEW_MOUSE are real HTTP_EVENT_COMMAND_MAP entries but never
/// produced by gate hardware, so intentionally not role-bearing).
inline int gate_role_from_event(const char *event) {
  if (strcmp(event, "ARM") == 0 || strcmp(event, "START") == 0) {
    return static_cast<int>(GateRole::START);
  }
  if (strcmp(event, "GOAL") == 0) {
    return static_cast<int>(GateRole::GOAL);
  }
  return -1;
}

/// @brief Called from WS_EVT_DATA once a role-bearing event's role is known
/// -- a duplicate/retried event still proves the link is alive right now,
/// so this should run unconditionally, not just on a fresh dispatch.
inline void gate_liveness_mark_role_connected(GateRole role, uint32_t client_id, const char *gate_id,
                                               const IPAddress &ip, uint32_t now_ms) {
  GateLinkState &slot = g_gate_link[static_cast<int>(role)];
  slot.connected = true;
  slot.client_id = client_id;
  strncpy(slot.gate_id, gate_id, sizeof(slot.gate_id) - 1);
  slot.gate_id[sizeof(slot.gate_id) - 1] = '\0';
  slot.remote_ip = ip;
  slot.last_change_ms = now_ms;
}

/// @brief Called from WS_EVT_CONNECT. Optimistically re-associates a
/// reconnecting client with its role by matching remote_ip against the
/// last-known IP for each role slot, ahead of the client's next real event.
/// No-op if the IP doesn't match a previously-seen role (e.g. this really
/// is a brand-new/never-before-seen client).
inline void gate_liveness_note_client_connect(uint32_t client_id, const IPAddress &ip, uint32_t now_ms) {
  for (size_t i = 0; i < NUM_GATE_ROLES; i++) {
    if (g_gate_link[i].remote_ip == ip) {
      g_gate_link[i].connected = true;
      g_gate_link[i].client_id = client_id;
      g_gate_link[i].last_change_ms = now_ms;
      return;
    }
  }
}

/// @brief Called from WS_EVT_DISCONNECT/WS_EVT_ERROR (client->id() only --
/// no payload available at this point). Clears whichever role slot (if any)
/// this client_id currently holds; gate_id/remote_ip are kept so a
/// subsequent reconnect from the same IP can be optimistically
/// re-associated by gate_liveness_note_client_connect() above.
inline void gate_liveness_mark_client_disconnected(uint32_t client_id, uint32_t now_ms) {
  for (size_t i = 0; i < NUM_GATE_ROLES; i++) {
    if (g_gate_link[i].connected && g_gate_link[i].client_id == client_id) {
      g_gate_link[i].connected = false;
      g_gate_link[i].last_change_ms = now_ms;
      return;
    }
  }
}
