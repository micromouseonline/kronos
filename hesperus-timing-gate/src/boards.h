#pragma once

#include <WiFi.h>

struct BoardInfo {
  uint32_t id32;
  const char *name;
};

// Fill these with board IDs derived from MAC address
constexpr BoardInfo boards[] = {
    {0x0E209D7C, "GATE_02"},     // 18:8B:0E:20:9D:7C -> 192.168.0.81
    {0x0E22B6A4, "GATE_01"},     // 18:8B:0E:22:B6:A4 -> 192.168.0.80
    {0x45B077E0, "GATE_04"},     //
    {0x6ECC52D4, "GREEN-05"},    // AC:27:6E:CC:52:D4
    {0x6ECC5904, "GREEN-01"},    // AC:27:6E:CC:59:04
    {0x6ECC9A5C, "GREEN-06"},    // AC:27:6E:CC:9A:5C
    {0x85677860, "GREEN-08"},    // 28:84:85:67:78:60
    {0x8567826C, "GREEN-04"},    // 28:84:85:67:82:6C
    {0x8567D29C, "GREEN-17"},    // 28:84:85:67:D2:9C
    {0x8567EA68, "GREEN-03"},    // 28:84:85:67:EA:68
    {0x9037CC98, "GATE_03"},     //
    {0xAB6E5000, "M5STACK-01"},  // 98:F4:AB:6E:50:00
    {0xE6FFFE1E, "GATE_05"},     // AC:EB:E6:FF:FE:1E
};

inline uint32_t getChipID32() {
  uint64_t mac = ESP.getEfuseMac();  // 48‑bit unique ID
  return (uint32_t)(mac >> 16);      // take upper 32 bits
}

inline const char *identifyBoard() {
  uint32_t id32 = getChipID32();

  for (auto &b : boards) {
    if (id32 == b.id32) {
      return b.name;
    }
  }

  return "UNKNOWN";
}
