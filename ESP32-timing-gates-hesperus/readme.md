# ESP32 S3 Zero

## Flashing and Serial Access

I understand that the ESP32-S3 may be fiddly tro to have reset and get the serial port going after reflashing. Check the PLATFORMIO.INI file for the correct settings and have the setup() function wait until the port is ready.

```ini
# Base configuration for ESP32-S3 series.
# These typically have different USB and other peripheral setups compared to original ESP32.
build_flags =
  -D ARDUINO_USB_CDC_ON_BOOT=1    ; Enable USB CDC (virtual COM port) at boot
  -D ARDUINO_USB_MODE=1           ; Select device mode for USB (value 1 = device)
  -D ARDUINO_TINYUSB=1            ; Use TinyUSB stack for native USB functionality

```
You may have to manually reset after flashing. A power cycle will get thgings to a known state though.

