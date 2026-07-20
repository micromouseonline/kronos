// ----------------------------------------------------------------------------
//  boards/m5-core.h — M5Stack Core hardware profile.
//  Add new boards as sibling files here, not inside config.h.
//
//  Display/backlight are LovyanGFX-autodetected (see display.h's
//  LGFX_AUTODETECT branch), so only the non-autodetected specifics (button
//  pins, capability flags) live here.
// ----------------------------------------------------------------------------
#pragma once

// ----- M5Stack Core Native Specifications -----
#define DISPLAY_PANEL_M5STACK_CORE
constexpr int PANEL_NATIVE_WIDTH = 320;  // Core native is landscape (320x240)
constexpr int PANEL_NATIVE_HEIGHT = 240;
constexpr int DISPLAY_WIDTH = 320;
constexpr int DISPLAY_HEIGHT = 240;
constexpr int DISPLAY_ROTATION = 1;
constexpr bool INVERT_COLORS = true;
constexpr bool BGR_ORDER = false;
constexpr int PANEL_X_OFFSET = 0;
constexpr int PANEL_Y_OFFSET = 0;

// ----- Physical Buttons (A/B/C, active-LOW with onboard pull-ups) -----
constexpr int PIN_BUTTON_A = 39;
constexpr int PIN_BUTTON_B = 38;
constexpr int PIN_BUTTON_C = 37;
constexpr unsigned long GPIO_BUTTON_LONG_PRESS_MS = 1600;  // hold threshold, shared by all 3 buttons

// ----- NeoKey 1x4 Configuration (optional attachment, I2C port 1) -----
// Confirmed against the physical board's external I2C connector:
// GPIO21=SDA, GPIO22=SCL. NOTE: LovyanGFX's autodetect briefly touches
// I2C port 1 on these exact pins while probing for AXP192 (used by
// M5Station/Core2/Tough to identify those variants) -- but the plain
// "M5Stack Core" board this profile targets (board_M5Stack /
// m5stack-core-esp32-16M) drives its backlight via plain PWM on GPIO32
// instead, so that probe releases the port before app_setup() reaches
// neokey_buttons_init(). Expected to be safe based on reading LovyanGFX's
// autodetect source, but unlike the other 4 boards this one hasn't been
// confirmed on real hardware yet -- watch for "[NEOKEY] not found" (fine,
// just means the module isn't attached this session) vs. any regression in
// display/backlight behavior (would mean the port-1 sharing assumption was
// wrong).
constexpr int PIN_NEOKEY_SDA = 21;
constexpr int PIN_NEOKEY_SCL = 22;

// ----- Input Capability Flags -----
#define HAS_TOUCH_INPUT 0
#define HAS_GPIO_BUTTONS 1
#define HAS_NEOKEY_BUTTONS 1  // optional attachment -- runtime-detected, see neokey-driver.h's `available` flag
#define TOUCH_SHARES_DISPLAY_SPI_BUS 0
#define TOUCH_NEEDS_CALIBRATION 0  // no touch controller on this board

constexpr int LCD_ROTATION = 1;
