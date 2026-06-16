# event-pulse-generator-arduino-nano

PlatformIO firmware for the Arduino Nano reference pulse generator.

## Platform

- Board: Arduino Nano (ATmega328, new bootloader)
- Framework: Arduino
- Build tool: PlatformIO (`pio run`, `pio run -t upload`)
- Upload port: COM18

## Source layout

```
src/
  main.cpp   - generates one pulse per second on LED_BUILTIN
```

## Purpose

Generates a 1 Hz reference pulse (100 ms high, 900 ms low) for timing calibration. The period constant (`one_second = 998572 us`) compensates for measured crystal error on this specific board.
