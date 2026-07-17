// ----------------------------------------------------------------------------
//  boards/jc2432w328c.h — Guition/JC "JC2432W328C" hardware profile
//  (a fourth, distinct CYD-family board).
//  Add new boards as sibling files here, not inside config.h.
//
//  VERIFIED working end to end on real hardware: panel (boots, displays,
//  correct colors/orientation) and CST820 capacitive touch (registers
//  presses). Started as a copy of cyd2usb-diymalls-st7789.h; panel pins,
//  backlight pin, SPI write frequency and rotation were cross-checked
//  against a "display only" LovyanGFX demo for this exact board, and the
//  touch section was replaced entirely (the DIYMALLS boards' resistive
//  XPT2046 assumption was wrong for this board -- see Touch Configuration
//  below).
// ----------------------------------------------------------------------------
#pragma once

#define DISPLAY_PANEL_ST7789
#define DISPLAY_TOUCH_CST820  // capacitive touch over I2C -- confirmed, NOT the resistive XPT2046 assumed at first

// ----- Panel Pinout (SPI) -----
// Cross-confirmed against a working "display only" LovyanGFX demo for this
// exact board. Note PIN_LCD_MISO=-1 (not wired -- panel is write-only,
// matches readable=false in display.h) and the backlight pin (27, driven by
// plain digitalWrite in the reference, not a PWM channel).
constexpr int PIN_LCD_MOSI = 13;
constexpr int PIN_LCD_MISO = -1;
constexpr int PIN_LCD_SCLK = 14;
constexpr int PIN_LCD_CS = 15;
constexpr int PIN_LCD_DC = 2;
constexpr int PIN_LCD_RST = -1;  // tied to system reset
constexpr int PIN_LCD_BL = 27;   // Backlight pin -- NOT 21, differs from the DIYMALLS boards
constexpr int SPI_WRITE_HZ = 27000000;
constexpr bool BL_ACTIVE_HIGH = true;

// ----- Panel Orientation & Dimensions -----
constexpr int PANEL_NATIVE_WIDTH = 240;
constexpr int PANEL_NATIVE_HEIGHT = 320;
constexpr int DISPLAY_WIDTH = 320;
constexpr int DISPLAY_HEIGHT = 240;
constexpr int DISPLAY_ROTATION = 1;
// INVERT_COLORS=false and PANEL_OFFSET_ROTATION=0 match the working
// reference demo's config (invert=false, offset_rotation=0). If a different
// unit shows inverted colors or mirrored text/graphics, flip INVERT_COLORS
// or try PANEL_OFFSET_ROTATION values 2/4/6 (LovyanGFX MADCTL mirror
// offset).
constexpr bool INVERT_COLORS = false;
constexpr bool BGR_ORDER = true;
constexpr int PANEL_X_OFFSET = 0;
constexpr int PANEL_Y_OFFSET = 0;
constexpr int PANEL_OFFSET_ROTATION = 0;

// ----- Touch Configuration (CST820, I2C) -----
// This board's touch is capacitive (CST820), NOT the resistive XPT2046
// copied from the DIYMALLS boards originally -- that whole SPI-based touch
// section was wrong and has been replaced. Confirmed via a working Arduino
// driver for this exact board: CST820 touch(sda=33, scl=32, rst=25, int=21),
// I2C address 0x15.
//
// LovyanGFX has no dedicated Touch_CST820 class, but its Touch_CST816S
// class defaults to the same I2C address (0x15) -- CST816S/CST816T/CST820
// are register-compatible Hynitron siblings, and Touch_CST816S is the
// standard way to drive a CST820 in LovyanGFX. See display.h.
constexpr int PIN_TOUCH_SDA = 33;
constexpr int PIN_TOUCH_SCL = 32;
constexpr int PIN_TOUCH_RST = 25;
// PIN_TOUCH_INT was GPIO21, matching the reference driver's int=21. Freed to
// -1 (no INT pin, matches display.h's existing -1-means-unwired convention,
// e.g. PIN_LCD_RST above) to hand GPIO21 to the NeoKey I2C connector below --
// LovyanGFX's Touch_CST816S doesn't require a wired INT pin, touch is
// already read by polling (LVGL's touch indev read callback,
// lib/display/lvgl-bridge.cpp, calls lcd.getTouch() every cycle, not
// interrupt-driven).
constexpr int PIN_TOUCH_INT = -1;
constexpr int TOUCH_I2C_ADDR = 0x15;
constexpr int TOUCH_I2C_HZ = 400000;

// ----- NeoKey 1x4 Configuration (optional attachment, I2C port 1) -----
// Board's external I2C connector: GPIO21/GPIO22, freed up above from the
// touch controller's unused INT pin. CONFIRMED on real hardware: key
// presses work, including simultaneously with touch.
constexpr int PIN_NEOKEY_SDA = 21;
constexpr int PIN_NEOKEY_SCL = 22;

// ----- Input Capability Flags -----
#define HAS_TOUCH_INPUT 1
#define HAS_GPIO_BUTTONS 0
#define HAS_NEOKEY_BUTTONS 1  // optional attachment -- runtime-detected, see neokey-driver.h's `available` flag
#define TOUCH_SHARES_DISPLAY_SPI_BUS 0  // touch is I2C, not on the panel's SPI bus at all
// CST820 reports pixel coordinates, but in its own native orientation --
// touch was reported working correctly on this board's hardware already
// (offset_rotation=0 below happens to match LCD_ROTATION=3), but Freenove
// hit exactly this class of bug (FT6336U, offset_rotation=0 did NOT match
// its LCD_ROTATION=1), so calibration is enabled here too rather than
// relying on a coincidental match holding across every physical unit.
#define TOUCH_NEEDS_CALIBRATION 1

constexpr int LCD_ROTATION = 3;  // confirmed working for the panel; touch offset_rotation may still need tuning separately
