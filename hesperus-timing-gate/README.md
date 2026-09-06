# hesperus-timing-gate

ESP32 firmware for the two-gate infrared timing system's gate boards.
Gates synchronise timing via Wi-Fi TSF and report `TRIGGER_A`/`TRIGGER_B`
events to `cerberus-gate-controller` over a persistent WebSocket
connection, with dynamic EMA-weighted clock disciplining against drift.

See [CLAUDE.md](CLAUDE.md) for firmware design details and
[../PLATFORMIO.md](../PLATFORMIO.md) for the workspace build guide.

## Targets

| PlatformIO env                    | Board                                  |
|------------------------------------|-----------------------------------------|
| hesperus-gate-s3-zero              | Waveshare/generic ESP32-S3-Zero         |
| hesperus-gate-s3-super-mini        | ESP32-S3 Super Mini                     |
| hesperus-gate-c3-super-mini        | ESP32-C3 Super Mini                     |
| hesperus-gate-c3-xiao               | Seeed XIAO ESP32-C3                     |
| hesperus-gate-qtpy-esp32-pico       | Adafruit QT Py ESP32 Pico (alternate-silicon trial board) |

## Build

```
pio run -e hesperus-gate-s3-zero
pio run -e hesperus-gate-s3-zero -t upload
```
from `firmware/`.

## Flashing and serial access (ESP32-S3 boards)

The S3 boards (`hesperus-gate-s3-zero`, `hesperus-gate-s3-super-mini`) use
native USB CDC rather than a USB-serial bridge chip, so the port can be
fiddly to get back after a reflash. `boards.ini` already sets:

```ini
build_flags =
  -D ARDUINO_USB_CDC_ON_BOOT=1    ; Enable USB CDC (virtual COM port) at boot
  -D ARDUINO_USB_MODE=1           ; Select device mode for USB (value 1 = device)
  -D ARDUINO_TINYUSB=1            ; Use TinyUSB stack for native USB functionality
```

If the port doesn't come back after flashing, a manual reset or power
cycle gets it to a known state. The C3 and QT Py ESP32 Pico envs don't set
`ARDUINO_TINYUSB` and aren't affected by this native-USB-CDC quirk (the QT
Py Pico's USB-serial goes through an onboard CP2102N bridge chip instead —
see `boards.ini`'s `base_qtpy_esp32_pico` comments).
