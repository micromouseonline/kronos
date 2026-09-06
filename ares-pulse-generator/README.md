# ares-pulse-generator

ESP32-S3/C3 bench tool that simulates photodiode/beam-break triggers into
a real `hesperus-timing-gate` board under test, by driving three
active-low GPIO outputs (`TRG_ARM`/`TRG_START`/`TRG_GOAL`). Trials are
runtime-selectable over serial (`list`/`run`/`arm`/`status`) or fired by a
physical button against whatever trial is currently armed.

See [CLAUDE.md](CLAUDE.md) for firmware design details and
[../PLATFORMIO.md](../PLATFORMIO.md) for the workspace build guide.

**Note:** since hesperus's `base_s3_zero`/`base_s3_super_mini` boards moved
to the analogue dual-EMA beam-break path (`beam-sensor.h`), ARES's digital
pulse output can no longer trigger those boards — see `TODO.md`'s
`ares-pulse-generator` section.

## Targets

| PlatformIO env               | Board                             |
|-------------------------------|-------------------------------------|
| ares-pulser-s3-zero           | Waveshare/generic ESP32-S3-Zero     |
| ares-pulser-s3-super-mini     | ESP32-S3 Super Mini                 |
| ares-pulser-c3-super-mini     | ESP32-C3 Super Mini                 |
| ares-pulser-c3-xiao            | Seeed XIAO ESP32-C3                 |

## Build

```
pio run -e ares-pulser-s3-zero
pio run -e ares-pulser-s3-zero -t upload
```
from `firmware/`.

## Flashing and serial access (ESP32-S3 boards)

The S3 boards (`ares-pulser-s3-zero`, `ares-pulser-s3-super-mini`) use
native USB CDC rather than a USB-serial bridge chip, so the port can be
fiddly to get back after a reflash. `boards.ini` already sets:

```ini
build_flags =
  -D ARDUINO_USB_CDC_ON_BOOT=1    ; Enable USB CDC (virtual COM port) at boot
  -D ARDUINO_USB_MODE=1           ; Select device mode for USB (value 1 = device)
  -D ARDUINO_TINYUSB=1            ; Use TinyUSB stack for native USB functionality
```

If the port doesn't come back after flashing, a manual reset or power
cycle gets it to a known state. The C3 envs don't set `ARDUINO_TINYUSB`
and aren't affected by this quirk.

**Also note:** resetting the ARES board while wired directly to a pair of
hesperus boards can produce a spurious trigger glitch on both gates — make
sure it has fully reset before starting a trial or a new log (bench-test
tooling only, no bearing on production use; see `TODO.md`).
