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

constexpr CliCommand PROVISIONING_COMMANDS[] = {
    {"wifi", cmd_wifi},
    {"role", cmd_role},
};
constexpr size_t PROVISIONING_COMMAND_COUNT = sizeof(PROVISIONING_COMMANDS) / sizeof(PROVISIONING_COMMANDS[0]);
