// ----------------------------------------------------------------------------
//  board-id.h — Identifies which physical controller this is from its
//  factory eFuse MAC, and names it. get_board_name() is used as both the
//  mDNS hostname (net/mdns.h, so the board is http://<name>.local/) and the
//  Wi-Fi/DHCP hostname (set in main.cpp's setup()). Hesperus gates look up
//  "cerberus" -- mDNS names are case-insensitive, so only the board named
//  CERBERUS below is reachable by the gates without changing their firmware.
// ----------------------------------------------------------------------------
#pragma once

#include <esp_mac.h>

#include <cstdint>
#include <cstdio>

struct BoardInfo {
  uint32_t id32;
  const char *name;
};

// Board IDs derived from the last 4 bytes of the MAC address
inline constexpr BoardInfo boards[] = {
    {0xD8E7240C, "CERBERUS"},      // B0:CB:D8:E7:24:0C
    {0x5C0AAF9C, "CERBERUS-GREY"}, // 14:33:5C:0A:AF:9C
    {0x3C2C6100, "CERBERUS-RED"},  // 78:1C:3C:2C:61:00
};

inline uint32_t get_chip_id() {
  uint8_t mac[6];
  esp_efuse_mac_get_default(mac);

  // Cast to uint32_t before shifting to prevent signed int promotion issues
  return ((uint32_t)mac[2] << 24) | //
         ((uint32_t)mac[3] << 16) | //
         ((uint32_t)mac[4] << 8) |  //
         (uint32_t)mac[5];          //
}

inline const char *get_board_name() {
  uint32_t id32 = get_chip_id();

  // 1. Check if the board is already known
  for (auto &b : boards) {
    if (id32 == b.id32) {
      return b.name;
    }
  }

  // 2. Fallback: CERBERUS-XXXX from the last two MAC octets.
  // 'static' keeps this buffer alive in memory after the function returns.
  static char fallback_name[16];
  snprintf(fallback_name, sizeof(fallback_name), "CERBERUS-%04X", (unsigned)(id32 & 0xFFFF));

  return fallback_name;
}
