// ----------------------------------------------------------------------------
//  lv_conf.h — LVGL 8.3.x configuration (LV_CONF_INCLUDE_SIMPLE, see
//  boards.ini's feature_lvgl block). Deliberately minimal: everything not
//  defined here falls back to LVGL's own default in lv_conf_internal.h.
//
//  The FEATURE CONFIGURATION section's font/widget enables below are
//  reconciled against the actual generated lib/ui/screens.c (grep for
//  lv_obj_set_style_text_font/lv_font_montserrat_*/lv_*_create calls) --
//  if a future EEZ Studio re-export starts using a font size or widget
//  not already enabled here, the build will fail with an undeclared
//  identifier and this list needs a new line added.
// ----------------------------------------------------------------------------
#ifndef LV_CONF_H
#define LV_CONF_H

#include <stdint.h>

/*====================
   COLOR SETTINGS
 *====================*/
#define LV_COLOR_DEPTH 16
// If the first boot shows byte-swapped/wrong-hued colors, flip this --
// depends on how LovyanGFX's pushImageDMA() (lib/display/lvgl-bridge.cpp)
// hands pixels to the panel; not knowable without real hardware.
#define LV_COLOR_16_SWAP 0

/*=========================
   MEMORY SETTINGS
 *=========================*/
// LVGL's internal heap (widget objects, styles, animations) -- separate
// from the display draw buffer, which lvgl-bridge.cpp allocates directly
// and sizes per-board (PSRAM vs internal-SRAM boards). 48KB fits all boards
// without board-specific conditionals here.
#define LV_MEM_CUSTOM 0
#define LV_MEM_SIZE (48U * 1024U)

/*====================
   HAL SETTINGS
 *====================*/
#define LV_DISP_DEF_REFR_PERIOD 30
#define LV_INDEV_DEF_READ_PERIOD 30
// Manual tick source -- lvgl-bridge.cpp drives lv_tick_inc() from an
// esp_timer periodic callback, so no LV_TICK_CUSTOM_* wiring is needed.
#define LV_TICK_CUSTOM 0
#define LV_DPI_DEF 130

/*=======================
 * FEATURE CONFIGURATION
 *=======================*/
#define LV_USE_LOG 0

// Sizes actually referenced by lib/ui/screens.c's
// lv_obj_set_style_text_font() calls (montserrat_8/12/... etc. also appear
// in screens.c but only inside their own #if LV_FONT_MONTSERRAT_N guards,
// as part of EEZ's font-picker lookup table -- those don't need enabling).
#define LV_FONT_MONTSERRAT_10 1
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_28 1
#define LV_FONT_DEFAULT &lv_font_montserrat_14

// Layout systems and extra widgets screens.c uses.
#define LV_USE_FLEX 1
#define LV_USE_GRID 1
#define LV_USE_LIST 1
#define LV_USE_QRCODE 1

#define LV_USE_PERF_MONITOR 0
#define LV_USE_MEM_MONITOR 0

#define LV_BUILD_EXAMPLES 0

#endif  // LV_CONF_H
