// ----------------------------------------------------------------------------
//  provisioning-commands.h — `wifi` and `role` serial commands, registered
//  with cli.h's Cli class. Bench-provisioning only: connect over USB serial,
//  type `wifi <ssid> <passphrase>` (saves to NVS, reboots) and
//  `role <start|goal>` (saves to NVS, takes effect immediately -- no reboot
//  needed since the role table is read fresh on every event).
// ----------------------------------------------------------------------------
#pragma once

#include <Arduino.h>

#include "board-role.h"
#include "cli.h"
#include "network-health-stats.h"
#include "wifi-credentials.h"

inline void cmd_wifi(int argc, char **argv) {
  if (argc != 3) {
    Serial.println("usage: wifi <ssid> <passphrase>");
    return;
  }
  wifi_credentials_save(argv[1], argv[2]);
  Serial.printf("Wi-Fi credentials saved for \"%s\", rebooting...\n", argv[1]);
  delay(200);
  ESP.restart();
}

inline void cmd_role(int argc, char **argv) {
  if (argc != 2) {
    Serial.println("usage: role <start|goal>");
    return;
  }
  BoardRole role;
  if (strcmp(argv[1], "start") == 0) {
    role = BoardRole::START;
  } else if (strcmp(argv[1], "goal") == 0) {
    role = BoardRole::GOAL;
  } else {
    Serial.println("usage: role <start|goal>");
    return;
  }
  board_role_save(role);
  Serial.printf("Role saved: %s\n", board_role_name(role));
}

inline void cmd_netstats(int argc, char **argv) {
  if (argc == 2 && strcmp(argv[1], "clear") == 0) {
    network_health_clear();
    Serial.println("netstats cleared");
    return;
  }
  if (argc != 1) {
    Serial.println("usage: netstats [clear]");
    return;
  }
  Serial.printf("ws_stall_count=%u ws_max_stall_ms=%u ws_disconnect_count=%u\n", g_network_health.stall_count,
                g_network_health.max_stall_ms, g_network_health.disconnect_count);
  Serial.printf("event_drop_ack_deadline=%u event_drop_max_retries=%u event_drop_link_down=%u\n",
                g_network_health.drop_ack_deadline, g_network_health.drop_max_retries,
                g_network_health.drop_link_down);
}

constexpr CliCommand PROVISIONING_COMMANDS[] = {
    {"wifi", cmd_wifi},
    {"role", cmd_role},
    {"netstats", cmd_netstats},
};
constexpr size_t PROVISIONING_COMMAND_COUNT = sizeof(PROVISIONING_COMMANDS) / sizeof(PROVISIONING_COMMANDS[0]);
