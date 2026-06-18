#pragma once

#include <WiFi.h>

struct BoardInfo {
    uint32_t id32;
    const char *name;
};

// Fill these with board IDs derived from MAC address
BoardInfo boards[] = {
        {0x98CC3790, "GATE_01"},      // 94:a9:90:37:cc:98
        {0xE077B045, "GATE_02"},      // eFuse MAC raw: 0xE077B0453AB4
        {0xE4F8220E, "GATE_C3_03"}};  // C3 board - 18:8B:0E:22:F8:E4


inline uint32_t getChipID32() {
    uint64_t mac = ESP.getEfuseMac();  // 48‑bit unique ID
    return (uint32_t) (mac >> 16);     // take upper 32 bits
}


inline const char *identifyBoard() {
    uint32_t id32 = getChipID32();

    for (auto &b: boards) {
        if (id32 == b.id32) {
            return b.name;
        }
    }

    return "UNKNOWN";
}
