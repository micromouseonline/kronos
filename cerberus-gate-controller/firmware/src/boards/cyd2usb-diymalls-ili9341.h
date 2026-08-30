// ----------------------------------------------------------------------------
//  boards/cyd2usb-diymalls-ili9341.h — Sunton "CYD2USB" hardware profile,
//  DIYMALLS variant with an ILI9341 panel driver (ESP32-2432S028R family,
//  micro-USB + USB-C).
//  Add new boards as sibling files here, not inside config.h.
//
//  Pin values below started as the commonly-documented values for this
//  board family and are now VERIFIED working (boots, displays, touch
//  responds) on the board purchased from
//  https://www.amazon.co.uk/dp/B0CG2WQGP9 -- see boards.ini's
//  base_cyd2usb_diymalls_ili9341 section. A visually-identical CYD2USB
//  variant with a different panel driver chip (ST7789) also exists --
//  see cyd2usb-diymalls-st7789.h -- selected by its own BOARD_* macro.
// ----------------------------------------------------------------------------
#pragma once

#define DISPLAY_PANEL_ILI9341
#define DISPLAY_TOUCH_XPT2046  // resistive touch, own dedicated SPI pins (NOT shared with the panel)

// ----- Panel Pinout (SPI) -----
constexpr int PIN_LCD_MOSI = 13;
constexpr int PIN_LCD_MISO = 12;
constexpr int PIN_LCD_SCLK = 14;
constexpr int PIN_LCD_CS = 15;
constexpr int PIN_LCD_DC = 2;
constexpr int PIN_LCD_RST = -1;  // tied to system reset
constexpr int PIN_LCD_BL = 21;   // Backlight pin
constexpr int SPI_WRITE_HZ = 40000000;
constexpr bool BL_ACTIVE_HIGH = true;

// ----- Panel Orientation & Dimensions -----
constexpr int PANEL_NATIVE_WIDTH = 240;
constexpr int PANEL_NATIVE_HEIGHT = 320;
constexpr int DISPLAY_WIDTH = 320;
constexpr int DISPLAY_HEIGHT = 240;
constexpr int DISPLAY_ROTATION = 1;
// INVERT_COLORS=false and PANEL_OFFSET_ROTATION=0 are the confirmed-working
// values on real hardware (see the file header). If a different unit shows
// inverted colors or mirrored text/graphics, flip INVERT_COLORS or try
// PANEL_OFFSET_ROTATION values 2/4/6 (LovyanGFX MADCTL mirror offset).
constexpr bool INVERT_COLORS = false;
constexpr bool BGR_ORDER = true;
constexpr int PANEL_X_OFFSET = 0;
constexpr int PANEL_Y_OFFSET = 0;
constexpr int PANEL_OFFSET_ROTATION = 0;

// ----- Touch Configuration (XPT2046) -----
// Dedicated SPI pins, physically separate from the panel's bus (unlike the
// capacitive FT6336U board, this touch chip is NOT on shared MOSI/MISO/SCLK).
// GPIO36/39 are input-only on classic ESP32, used here for the two
// touch-chip-to-ESP32 signals (DOUT, IRQ). Confirmed working (touch
// responds) on real hardware -- see the file header.
constexpr int PIN_TOUCH_CLK = 25;
constexpr int PIN_TOUCH_MOSI = 32;  // T_DIN
constexpr int PIN_TOUCH_MISO = 39;  // T_DOUT (input-only pin)
constexpr int PIN_TOUCH_CS = 33;
constexpr int PIN_TOUCH_IRQ = 36;  // input-only pin
constexpr int TOUCH_SPI_HZ = 1000000;

// ----- NeoKey 1x4 Configuration (optional attachment, I2C port 1) -----
// Confirmed against the physical board's external I2C connector:
// GPIO22=SCL, GPIO27=SDA. Neither pin is used by the panel/touch pins
// above -- this board's I2C bus is otherwise completely unused (touch is
// SPI XPT2046).
// CONFIRMED on real hardware: key presses work, including simultaneously
// with touch.
constexpr int PIN_NEOKEY_SDA = 27;
constexpr int PIN_NEOKEY_SCL = 22;

// ----- Input Capability Flags -----
#define HAS_TOUCH_INPUT 1
#define HAS_GPIO_BUTTONS 0
#define HAS_NEOKEY_BUTTONS 1            // optional attachment -- runtime-detected, see neokey-driver.h's `available` flag
#define TOUCH_SHARES_DISPLAY_SPI_BUS 0  // XPT2046 has its own dedicated SPI pins, separate from the panel
#define TOUCH_NEEDS_CALIBRATION 1       // resistive XPT2046 -- raw ADC readings need mapping to screen pixels

constexpr int LCD_ROTATION = 1;
