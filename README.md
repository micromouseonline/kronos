# KRONOS: Hardware-Synchronized Robotic Timing Gates

**KRONOS** is a distributed, microsecond-accurate wireless robotic timing gate ecosystem built on the ESP32 platform. It is designed for high-performance track automation and precision race scoring without the need for physical connection cables.

---

## Technical Highlights

* **Hardware-Level Precision:** Achieves an average tracking accuracy gap of $2.36\,\mu\text{s}$ (with a worst-case variance of $55\,\mu\text{s}$) by anchoring remote nodes to the 1 MHz Timing Synchronization Function (TSF) clock embedded directly within Wi-Fi beacon frames.
* **Self-Healing Clock Recovery:** Eradicates tracking errors from network drops or stack resets. If a node misses AP beacons, an autonomous audit engine seamlessly reconstructs missing timestamps using a disciplined internal monotonic processor clock (`esp_timer_get_time()`).
* **Asynchronous FreeRTOS Queues:** Decouples physical event capturing from network transmission. Pins use high-speed hardware interrupts to instantly cache telemetry into RAM queues, safely buffering rapid-fire bursts down to $35\text{ ms}$ intervals.
* **Resilient Network Architecture:** Uses HTTP over TCP (`HERMES` protocol) to guarantee delivery. Variable transport latencies or retransmission lags become irrelevant because the hardware event timestamp is baked directly into the packet payload.

---

## Naming Conventions & System Layers

* **KRONOS:** The overall distributed wireless network topology and codebase.
* **HADES:** The central PC software hosting the database and sliding-window event pairing algorithms.
* **ATLAS:** The standalone Wi-Fi Access Point acting as the authoritative field master clock.
* **CERBERUS:** The multi-threaded gate edge firmware handling interrupts and clock audits.
* **HESPERUS:** The physical optical sensor modules (lasers or infrared beams).
* **HERMES:** The concise HTTP application-layer packet payload protocol.

---

## Quick Start & Hardware Setup

### 1. Prerequisites

* **Access Point (`ATLAS`):** Any standard domestic or portable router configured as a standalone network subnet.
* **Gate Nodes:** Dual-core ESP32 or ESP32-S3 boards (standard single-core variants like the C3 require Level 2+ high-priority interrupt configurations to minimize jitter).
* **Sensors (`HESPERUS`):** Photodiode or laser break-beam circuits wired to the designated edge input pins.

### 2. Network Provisioning

The central `HADES` web server must be mapped to a fixed, unmoving network coordinate via a DHCP reservation or static IP mapping. Alternatively, the system natively supports multicast DNS (`mDNS`), allowing gates to dynamically connect via a hostname (e.g., `http://timing.local`).

### 3. Visual Diagnostics on Track

An onboard LED provides real-time state confirmation without an attached serial monitor:

* **LED Off:** System operating normally; network clearing traffic flawlessly.
* **LED Solid On:** Wi-Fi link down or server stalling; data packets are currently backing up safely inside the internal FreeRTOS memory queue.

---

## For More Information

For detailed clock calibration formulas, circuit diagrams, power management deep-dives (Wi-Fi Modem Sleep configurations), and field-trial telemetry logs, please refer to our full [**System Architecture Documentation Page**](docs/kronos-synchronized-timing-gates-architecture.md).