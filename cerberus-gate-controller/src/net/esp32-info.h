#pragma once

#include <Arduino.h>

/**
 * @brief Print the factory-programmed eFuse MAC address to Serial.
 *
 * Reads the 48-bit (or 64-bit on IEEE 802.15.4 variants) MAC address
 * from eFuse and prints it as colon-separated hex octets.
 */
inline void print_factory_mac(void) {
  uint64_t factmac = ESP.getEfuseMac();

  // A standard MAC address is always 6 bytes long,
  // even on chips with 15.4 thread/zigbee support.
  uint8_t mac[6];

  // Extract bytes in correct network order (Big Endian)
  mac[0] = (uint8_t)(factmac >> 0);
  mac[1] = (uint8_t)(factmac >> 8);
  mac[2] = (uint8_t)(factmac >> 16);
  mac[3] = (uint8_t)(factmac >> 24);
  mac[4] = (uint8_t)(factmac >> 32);
  mac[5] = (uint8_t)(factmac >> 40);

  Serial.printf("[SYSTEM] Factory MAC: %02X:%02X:%02X:%02X:%02X:%02X\n", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

/**
 * @brief Print a human-readable label for a flash chip mode to Serial.
 *
 * @param mode  Flash mode value returned by ESP.getFlashChipMode().
 */
void printFlashChipMode(FlashMode_t mode);

/**
 * @brief Print a full ESP32 system information report to Serial.
 *
 * Reports chip model, revision, core count, CPU frequency, SDK version,
 * flash size/speed/mode, PSRAM size, heap statistics, and sketch size.
 */
void getInfo(void);

inline void print_info(void) {
  Serial.println("\n\nESP32 Chip Information");
  Serial.printf("  Chip model: %s, Revision: %d\n", ESP.getChipModel(), ESP.getChipRevision());
  Serial.printf("  Core count: %d \n", ESP.getChipCores());
  Serial.printf("  CPU frequency: %lu MHz\n", ESP.getCpuFreqMHz());
  Serial.printf("  Cycle count: %lu\n", ESP.getCycleCount());
  Serial.printf("  SDK version: %s\n", ESP.getSdkVersion());

  Serial.println("\nFlash Memory");
  Serial.printf("  Flash size: %lu\n", ESP.getFlashChipSize());
  Serial.printf("  Flash speed: %lu\n", ESP.getFlashChipSpeed());
  Serial.print("  Flash mode: ");
  printFlashChipMode(ESP.getFlashChipMode());
  Serial.printf(" (%d)\n", ESP.getFlashChipMode());

  Serial.println("\nPseudo random access memory (PSRAM aka SPI RAM)");
  uint32_t psize = ESP.getPsramSize();
  Serial.print("  PSRAM size: ");
  if (psize) {
    Serial.printf("%lu\n", psize);
    Serial.printf("  Free PSRAM: %lu\n", ESP.getFreePsram());
    Serial.printf("  Min free PSRAM: %lu\n", ESP.getMinFreePsram());
    Serial.printf("  Max PSRAM alloc size: %lu\n", ESP.getMaxAllocPsram());
  } else {
    Serial.println("none");
  }

  Serial.println("\nSketch");
  Serial.printf("  Size: %lu\n", ESP.getSketchSize());
  Serial.printf("  Free space: %lu\n", ESP.getFreeSketchSpace());

  Serial.println("\nHeap");
  Serial.printf("  Size: %lu\n", ESP.getHeapSize());
  Serial.printf("  Free: %lu\n", ESP.getFreeHeap());
  Serial.printf("  Minimum free since boot: %lu\n", ESP.getMinFreeHeap());
  Serial.printf("  Maximum allocation size: %lu\n", ESP.getMaxAllocHeap());
}
