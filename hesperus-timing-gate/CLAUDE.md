# hesperus-timing-gate

PlatformIO firmware for the ESP32-S3 timing gates (project codename: Hesperus).

## Platform

- Board: ESP32-S3 (esp32-s3-devkitc-1)
- Framework: Arduino + FreeRTOS
- Build tool: PlatformIO (`pio run`, `pio run -t upload`)
- Serial monitor: 115200 baud

## Off-limits directories

- `firmware/.pio/`: generated build artefacts and vendored libraries - do not read or modify

## Hardware design files

- `hardware/pcb/`: KiCad project. `hardware/mechanical/`: enclosure/mounting
  CAD (native source + STEP/STL exports). Both are user-maintained via
  KiCad/CAD tools directly - do not hand-edit as text.

## Source layout

PlatformIO firmware lives under `firmware/` (`platformio.ini`, `boards.ini`,
`src/`, `include/`, `lib/`, `test/`, `dist/`) - run `pio run` from there.

```
firmware/src/
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
- The persistent WS connection to cerberus (`wsClient`, a `WebSocketsClient`) is
  pumped exclusively by a dedicated `wsPumpTask`, decoupled from
  `uploadWorkerTask`'s ack-wait/retry deadline logic via `ws_client_mutex`
  (guards every `wsClient` call) and `ws_ack_state_mutex` (guards the ack
  flag/tsf pair) — see `NETWORK-TIMING-LOG.md`'s "`wsClient.loop()`
  blocking under congestion" issue for why.
- `feature_http` (`AsyncTCP`/`ESPAsyncWebServer`, `#if HAS_HTTP`) backs a
  small on-demand diagnostics server only (`firmware/src/net/debug-http-server.h`,
  `GET /logs` / `GET /status`) — it does not carry event-reporting traffic,
  which stays on `feature_ws_client`'s `WebSocketsClient`.

## Libraries (managed by PlatformIO)

- Adafruit NeoPixel
- Adafruit SSD1306 / SH1106 / GFX
- JC_Button
- WebSockets (Links2004) — persistent WS client to cerberus
- AsyncTCP / ESPAsyncWebServer — diagnostics-only HTTP server (`firmware/src/net/debug-http-server.h`)
