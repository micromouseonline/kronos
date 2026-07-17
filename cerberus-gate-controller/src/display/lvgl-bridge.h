// ----------------------------------------------------------------------------
//  lvgl-bridge.h — Wires LVGL's display/touch drivers to the board's LGFX
//  instance (display.h). Depends only on board-select.h (via lvgl-bridge.cpp),
//  not config.h -- see lib/'s dependency-direction rule (lib/ never depends
//  on src/).
//
//  <lvgl.h> is deliberately NOT included here, only inside lvgl-bridge.cpp --
//  every env extends feature_lvgl (see boards.ini) so the dependency is
//  always available, but declarations here only reference LGFX, never an
//  lv_* type, so callers never need an #if either way.
// ----------------------------------------------------------------------------
#pragma once

#include "display.h"  // LGFX

// Initializes LVGL core, registers the display driver (flush callback +
// draw buffer, sized per-board -- a PSRAM double buffer where BOARD_HAS_PSRAM
// is set, a small internal-SRAM partial buffer otherwise), and starts the
// periodic tick source (esp_timer -> lv_tick_inc()). Call once from
// setup(), after lcd.init()/lcd.setRotation(). Always real -- every board
// gets the LVGL display, touch is the only per-board optional part.
void lvgl_display_init(LGFX &lcd);

// Registers the touch input device (lcd.getTouch() wrapped as an
// LV_INDEV_TYPE_POINTER indev). No-op when HAS_TOUCH_INPUT is 0 (e.g. M5
// Core, which gets the display half of this bridge but has no touchscreen).
void lvgl_touch_init(LGFX &lcd);

// Drives LVGL's timer/animation/rendering handler (lv_timer_handler()). Call
// every loop() iteration. Always real, same as lvgl_display_init().
void lvgl_task_handler();

// starts a lockout period to make sure the user's finger is clear of the screen
// before a second touch event can register.
void trigger_touch_lockout();
