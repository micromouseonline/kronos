# Technical Review: KRONOS/CERBERUS Gate Firmware

`ESP32-timing-gates-hesperus/src/main.cpp`

Reviewed by Claude

---

## Outstanding Features

### Disciplined dual-clock holdover architecture
The most technically sophisticated element of the firmware. By simultaneously capturing
both the Wi-Fi TSF counter and `esp_timer_get_time()` in the ISR at the moment of
the physical trigger, the design mirrors the holdover architecture of a professional
GPS receiver: a primary reference (TSF, disciplined by the AP's hardware beacon) and a
fallback oscillator (CPU crystal, disciplined by the EMA-filtered `clock_alpha`).
This is exactly the right approach for a wireless timing system where the primary
reference can momentarily disappear.

### EMA-filtered clock_alpha calibration
The 10%/90% EMA on `clock_alpha` is well-chosen. It suppresses per-beacon jitter while
still tracking slow crystal drift over minutes and hours. The >4 s inter-sample gate
ensures the ratio is statistically stable before being fed into the filter, avoiding
contamination from back-to-back events.

### MIN_PLAUSIBLE_TSF plausibility guard (300 000 000 µs)
Prevents accepting a TSF captured in the narrow window after a Wi-Fi stack reset before
beacon resynchronisation has occurred. 300 seconds is a well-judged threshold: long
enough to be certain the stack is settled, short enough to not delay legitimate startup.

### Escape hatch (PATCH 1) — proactive SYN recovery
After a successful `200 OK` response during SYN mode, the firmware immediately queries
the raw TSF and, if it looks healthy, forces re-entry by resetting `has_initial_baseline`.
This is a clean, proactive mechanism that does not rely on a new trigger event to
initiate recovery — important in a low-traffic test scenario where gate events may be
sparse.

### Explicit BSSID binding
`WiFi.begin(ssid, password, 0, bssid)` locks association to a known AP. Without this,
the ESP32 may roam to a neighbour access point whose TSF epoch is completely unrelated,
silently corrupting the timing baseline in a way that is very hard to diagnose in the
field.

### Prime heartbeat interval (5147 ms)
A non-round heartbeat interval desynchronises the transmission from 802.11 DTIM beacon
periods and from periodic tasks on other stations, reducing collision probability at the
AP — a detail that most implementors overlook.

### Correct FreeRTOS ISR practice
`xQueueSendFromISR` with `xHigherPriorityTaskWoken` followed by `portYIELD_FROM_ISR()`
is textbook correct. The ISR does the minimum possible work (capture timestamps, enqueue)
and yields immediately.

### Hard Wi-Fi watchdog
`WiFi.disconnect(true, true)` + delay + `WiFi.begin()` completely tears down and
rebuilds the radio interface. A soft reconnect alone cannot recover certain ESP-IDF
lwIP crash states. This is the correct treatment for a 15-second silence.

---

## Summary Assessment

The dual-clock holdover design is genuinely impressive for a consumer-grade Wi-Fi
platform and represents a level of engineering depth that goes well beyond typical
ESP32 hobby projects. The empirical results (mean gap 2.36 µs, max 55 µs, 100%
pairing over 60,000 events) validate the architecture.

---

## Future Development Path

| Phase | Feature | Notes |
|-------|---------|-------|
| Near | mDNS server discovery | Replace hardcoded IP with `timing.local` |
| Near | Wi-Fi Modem Sleep | `WIFI_PS_MIN_MODEM` between events; TSF active during sleep |
| Near | NVS config store | gate_id override, debounce, DRIFT_MARGIN_US, server URL |
| Near | Configurable debounce | Per-pin, loaded from NVS |
| Medium | OTA firmware update | ArduinoOTA or ESP-IDF OTA; critical for field deployment |
| Medium | SSD1306 display task | Gate ID, clock mode, last gap, queue depth (lib already in platformio.ini) |
| Medium | Stack telemetry in heartbeat | Append `&stack=NNN` to HB URL for remote diagnostics |
| Long | HTTP keep-alive / WebSocket | Persistent connection; halves TCP handshake overhead per event |
| Long | NVS event buffering | Survive power cycle with unsent events preserved in flash |
| Long | Multi-AP BSSID fallback | Secondary AP list with explicit TSF-resync on AP switch |
| Long | Local standalone scoring | Compute lap/split locally if server unreachable; display on SSD1306 |
