#pragma once

#include <WiFi.h>
#include <esp_mac.h>

#include "status-led.h"

struct BoardInfo {
  uint32_t id32;
  const char *name;
};

// Board IDs derived from the last 4 bytes of the MAC address
BoardInfo boards[] = {
    {0x0E22B6A4, "GATE_01"},  // 18:8B:0E:22:B6:A4 -> 192.168.0.80
    {0x0E209D7C, "GATE_02"},  // 18:8B:0E:20:9D:7C -> 192.168.0.81
    {0x9037CC98, "GATE_03"},  //
    {0x45B077E0, "GATE_04"},  //
    {0xE6FFFE1E, "GATE_05"},  // AC:EB:E6:FF:FE:1E
};

inline uint32_t get_chip_id() {
  uint8_t mac[6];
  esp_efuse_mac_get_default(mac);

  // Cast to uint32_t before shifting to prevent signed int promotion issues
  return ((uint32_t)mac[2] << 24) |  //
         ((uint32_t)mac[3] << 16) |  //
         ((uint32_t)mac[4] << 8) |   //
         (uint32_t)mac[5];           //
}

inline const char *get_board_name() {
  uint32_t id32 = get_chip_id();

  // 1. Check if the board is already known
  for (auto &b : boards) {
    if (id32 == b.id32) {
      return b.name;
    }
  }

  // 2. Fallback: If not found, format the required hex string.
  // 'static' keeps this buffer alive in memory after the function returns.
  static char fallback_name[12];
  snprintf(fallback_name, sizeof(fallback_name), "0x%08X", id32);

  return fallback_name;
}
