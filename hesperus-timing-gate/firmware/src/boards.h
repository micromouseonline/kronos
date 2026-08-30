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

/// @brief Returns the last 4 MAC octets (o2,o3,o4,o5 of the human-readable
/// o0:o1:o2:o3:o4:o5 form -- o0/o1 are the manufacturer OUI prefix, shared
/// by every board from the same batch, so not useful for telling boards
/// apart) packed as one uint32 in normal left-to-right octet order, i.e.
/// numerically equal to reading "o2o3o4o5" as a hex string. This is the
/// order boards[] below is populated in (matches a MAC as you'd see it in
/// ARP tables, network monitoring, AP client lists, etc.).
///
/// ESP.getEfuseMac() itself does NOT return the MAC in that order --
/// confirmed against real hardware 2026-08-07: it packs the 48-bit value
/// with o0 as the *least* significant byte and o5 towards the most
/// significant end, the reverse of human-reading order. Un-reversing that
/// here (rather than downstream at every call site) keeps `boards[]` and
/// identifyBoard() below written in the natural, ARP-matching order.
inline uint32_t getChipID32() {
  uint64_t mac = ESP.getEfuseMac();
  uint8_t o2 = (uint8_t)(mac >> 16);
  uint8_t o3 = (uint8_t)(mac >> 24);
  uint8_t o4 = (uint8_t)(mac >> 32);
  uint8_t o5 = (uint8_t)(mac >> 40);
  return ((uint32_t)o2 << 24) | ((uint32_t)o3 << 16) | ((uint32_t)o4 << 8) | o5;
}

/// @brief Falls back to the board's own last-three-MAC-octets (o3,o4,o5 --
/// id32's low 24 bits, already in proper left-to-right order per
/// getChipID32() above) as 6 uppercase hex characters, e.g. "CC9A5C" for
/// AC:27:6E:CC:9A:5C, when the board isn't in the `boards[]` catalogue
/// above. Was a bare "UNKNOWN" literal; that placeholder gave no way to
/// tell two uncatalogued boards apart on cerberus's side (log lines, the
/// gate-liveness UI) until someone catalogues them here.
inline const char *identifyBoard() {
  uint32_t id32 = getChipID32();

  for (auto &b : boards) {
    if (id32 == b.id32) {
      return b.name;
    }
  }

  static char fallback_name[7];  // 6 hex digits + NUL
  snprintf(fallback_name, sizeof(fallback_name), "%06X", (unsigned int)(id32 & 0x00FFFFFF));
  return fallback_name;
}
