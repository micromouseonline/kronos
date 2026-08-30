// ----------------------------------------------------------------------------
//  boards/cyd-touch-freenove.h — Freenove FNK0104B hardware profile.
//  Display: ILI9341 (240x320 IPS) over SPI. Touch: FT6336U (FocalTech
//  FT6x36 family) over I2C, 0x38. Backlight pin 45, active-HIGH.
//  Add new boards as sibling files here, not inside config.h.
// ----------------------------------------------------------------------------
#pragma once

// ----- Panel Driver & Touch Controller Selection -----
// Uncomment exactly one of each to match the hardware on your board.
#define DISPLAY_PANEL_ILI9341
// #define DISPLAY_PANEL_ST7789
// #define DISPLAY_PANEL_ILI9488
// #define DISPLAY_PANEL_GC9A01    // round 1.28" CYDs

#define DISPLAY_TOUCH_FT5X06  // FT5x06 driver handles FT6x36 family (FT6236, etc.)
// #define DISPLAY_TOUCH_XPT2046   // resistive boards
// #define DISPLAY_TOUCH_GT911

// ----- Panel Pinout (SPI) -----
constexpr int PIN_LCD_MOSI = 11;
constexpr int PIN_LCD_MISO = 13;
constexpr int PIN_LCD_SCLK = 12;
constexpr int PIN_LCD_CS = 10;
constexpr int PIN_LCD_DC = 46;
constexpr int PIN_LCD_RST = -1;  // -1 if tied to system reset
constexpr int PIN_LCD_BL = 45;   // Backlight pin
constexpr int SPI_WRITE_HZ = 40000000;
constexpr bool BL_ACTIVE_HIGH = true;

// ----- Panel Orientation & Dimensions -----
// Native panel is portrait 240x320. Display rotation swaps width/height.
constexpr int PANEL_NATIVE_WIDTH = 240;
constexpr int PANEL_NATIVE_HEIGHT = 320;
constexpr int DISPLAY_WIDTH = 320;   // Width after rotation
constexpr int DISPLAY_HEIGHT = 240;  // Height after rotation
constexpr int DISPLAY_ROTATION = 1;
constexpr bool INVERT_COLORS = true;  // Common for ILI9341 IPS modules
constexpr bool BGR_ORDER = true;      // true = BGR, false = RGB
constexpr int PANEL_X_OFFSET = 0;
constexpr int PANEL_Y_OFFSET = 0;

// ----- Touch Configuration -----
constexpr int PIN_TOUCH_SDA = 16;
constexpr int PIN_TOUCH_SCL = 15;
constexpr int PIN_TOUCH_INT = 17;
constexpr int PIN_TOUCH_RST = 18;
constexpr int TOUCH_I2C_ADDR = 0x38;  // FT6336U default
constexpr int TOUCH_I2C_HZ = 400000;

// ----- NeoKey 1x4 Configuration (optional attachment, I2C port 1) -----
// Confirmed against the physical FNK0104B board's dedicated external I2C
// connector: GPIO5=SCL, GPIO6=SDA.
constexpr int PIN_NEOKEY_SDA = 6;
constexpr int PIN_NEOKEY_SCL = 5;

// ----- Input Capability Flags -----
#define HAS_TOUCH_INPUT 1
#define HAS_GPIO_BUTTONS 0
#define HAS_NEOKEY_BUTTONS 1  // optional attachment -- runtime-detected, see neokey-driver.h's `available` flag
#define TOUCH_SHARES_DISPLAY_SPI_BUS 0  // FT6336U is on I2C, not the display's SPI bus
// FT6336U reports pixel coordinates, but in its own native orientation --
// with LCD_ROTATION=1 they don't line up with the displayed rotation
// (confirmed on hardware: touches registered, but at the wrong position).
// Run the same 4-corner wizard resistive boards use rather than hand-tuning
// display.h's touch offset_rotation by trial and error.
#define TOUCH_NEEDS_CALIBRATION 1

constexpr int LCD_ROTATION = 1;
