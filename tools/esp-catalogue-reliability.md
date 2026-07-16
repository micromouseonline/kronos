# ESP Catalogue Reliability Analysis

## Why esptool is more reliable

esptool's connection logic (`loader.py`, `reset.py`) has several layers the cataloging
script was missing.

### 1. Multiple reset strategies with delay variation

esptool cycles through four strategies per connection attempt:

| Strategy | Delay | Notes |
|---|---|---|
| `UnixTightReset` | 0.1s | Atomic POSIX DTR/RTS, POSIX only |
| `UnixTightReset` | 0.6s | Same, longer delay |
| `ClassicReset` | 0.1s | Sequential DTR/RTS writes |
| `ClassicReset` | 0.6s | Same, longer delay |

It makes up to 7 total connection attempts (`DEFAULT_CONNECT_ATTEMPTS`) cycling these
strategies. The original script did one pre-reset then delegated to esptool, but the
pre-reset was actually fighting esptool's own reset sequence.

### 2. USB device type detection

S3 and C6 boards can use one of three USB connection types, each needing a different
reset strategy:

| VID | PID | Connection type | Required strategy |
|---|---|---|---|
| 0x303A | 0x1001 | Built-in USB-JTAG/Serial | `usb_reset` |
| 0x303A | chip-specific | Built-in USB-OTG | `default_reset` |
| other | other | External UART bridge (CP210x, CH340) | `default_reset` |

For USB-JTAG/Serial (the built-in peripheral on S3 and C6), DTR and RTS are virtual
signals in the USB driver. They do not connect to the EN or BOOT pins. Toggling them
does nothing useful for resetting the chip, but opening the serial port can trigger a
USB disconnect/reconnect, leaving the port in an unstable state before esptool connects.

### 3. Two-phase chip detection

esptool uses two fallback methods:

1. `GET_SECURITY_INFO` command (0x14) - returns a chip ID; used on ESP32-C3 and later
2. Magic value read from `0x40001000` - used on ESP8266, ESP32 classic, ESP32-S2

This means it identifies the chip through the protocol rather than parsing output text.

### 4. Boot log detection

After reset, the ROM bootloader prints a line like:
```
boot:(0x13)(waiting for download)
```
esptool reads this and can distinguish:
- Chip not in download mode (wrong boot strapping)
- TX path down (boot log received but no sync response)

The cataloging script had no visibility into this; it just reported "failed to sync."

### 5. Tight sync timeout

`SYNC_TIMEOUT = 0.1s` per sync attempt, with 5 attempts per reset strategy. This means
esptool fails fast on each attempt and moves to the next strategy rather than blocking.
The original 5-second subprocess timeout covered only one esptool run but was not enough
for esptool to exhaust its 7-attempt cycle.

---

## Changes made to esp-catalogue.py

### Removed `reset_native_usb()`

The function opened the serial port, cleared DTR/RTS, then closed it before calling
esptool. For native USB chips this was counterproductive (see section 2 above). esptool
handles all reset sequencing internally.

### Added `get_before_mode(port)`

Detects VID/PID of the connected device and returns the appropriate `--before` flag:
- USB-JTAG/Serial (S3/C6 built-in): `"usb_reset"`
- Everything else: `"default_reset"`

### `--after no_reset` on `chip_id`, `--before no_reset` on `flash_id`

The original script made two separate esptool subprocess calls. After `chip_id`
completes, esptool hard-resets the chip (sends it back to normal boot). The `flash_id`
call then had to fight to re-enter download mode - a second full reset/sync cycle.

With `--after no_reset`, the chip stays in download mode after `chip_id`. The `flash_id`
call then uses `--before no_reset` to skip the reset and sync directly - one connection
cycle instead of two.

### Increased timeout and retries

- Timeout: 5s -> 20s (covers esptool's 7-attempt full cycle)
- Retries: 3 -> 5

### Extended RAM table

Added C6 (320 KB) and H2 (320 KB) which were missing.
