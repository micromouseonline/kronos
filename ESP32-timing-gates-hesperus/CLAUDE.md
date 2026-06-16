# ESP32-timing-gates-hesperus

PlatformIO firmware for the ESP32-S3 timing gates (project codename: Hesperus).

## Platform

- Board: ESP32-S3 (esp32-s3-devkitc-1)
- Framework: Arduino + FreeRTOS
- Build tool: PlatformIO (`pio run`, `pio run -t upload`)
- Serial monitor: 115200 baud

## Off-limits directories

- `.pio/`: generated build artefacts and vendored libraries - do not read or modify

## Source layout

```
src/
  main.cpp      - application entry point, FreeRTOS tasks
  boards.h      - MAC-to-gate-ID lookup table
  secrets.h     - WiFi credentials (not committed)
```

## Key design points

- Two FreeRTOS queues: `networkQueue` (HTTP dispatch) and `ledQueue` (NeoPixel feedback)
- Timing uses dual timestamps: `tsf_observed` (Wi-Fi TSF) and `processor_clock` (esp_timer)
- Dynamic clock disciplining: EMA-weighted `clock_alpha` corrects processor drift against TSF
- Events: `TRIGGER_A`, `TRIGGER_B`, `HEARTBEAT`
- Gate identity resolved at boot from MAC address via `boards.h`

## Libraries (managed by PlatformIO)

- Adafruit NeoPixel
- Adafruit SSD1306 / SH1106 / GFX
- JC_Button
