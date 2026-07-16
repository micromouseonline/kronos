#include "lvgl-bridge.h"

#include <lvgl.h>

#include <esp_heap_caps.h>
#include <esp_timer.h>

#include "board-select.h"

#ifdef BOARD_HAS_PSRAM
static constexpr uint32_t BUF_LINES = DISPLAY_HEIGHT;  // full-frame double buffer, PSRAM
#else
static constexpr uint32_t BUF_LINES = 20;  // small partial buffer, internal SRAM is scarce
#endif

static lv_disp_draw_buf_t draw_buf;
static lv_disp_drv_t disp_drv;
static LGFX *lcd_ptr = nullptr;

static void disp_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_p) {
  const int32_t w = area->x2 - area->x1 + 1;
  const int32_t h = area->y2 - area->y1 + 1;
  // pushImageDMA<uint16_t> has no byte-swap control at all (checked
  // LovyanGFX's LGFXBase.hpp directly). pushPixelsDMA's (data, len, swap)
  // overload does, and swap=true is what a known-working reference
  // implementation (timer-demo/main.cpp, same LV_COLOR_DEPTH=16 /
  // LV_COLOR_16_SWAP=0 lv_conf.h) uses -- pushImageDMA was silently wrong
  // on every board, just more visible on some panels than others.
  lcd_ptr->startWrite();
  lcd_ptr->setAddrWindow(area->x1, area->y1, w, h);
  lcd_ptr->pushPixelsDMA(reinterpret_cast<const uint16_t *>(color_p), w * h, true);
  lcd_ptr->endWrite();
  lv_disp_flush_ready(drv);
}

static void lv_tick_timer_cb(void *) {
  lv_tick_inc(1);
}

void lvgl_display_init(LGFX &lcd) {
  lcd_ptr = &lcd;
  lv_init();

#ifdef BOARD_HAS_PSRAM
  static lv_color_t *buf1 = static_cast<lv_color_t *>(
      heap_caps_malloc(DISPLAY_WIDTH * BUF_LINES * sizeof(lv_color_t), MALLOC_CAP_SPIRAM));
  static lv_color_t *buf2 = static_cast<lv_color_t *>(
      heap_caps_malloc(DISPLAY_WIDTH * BUF_LINES * sizeof(lv_color_t), MALLOC_CAP_SPIRAM));
  lv_disp_draw_buf_init(&draw_buf, buf1, buf2, DISPLAY_WIDTH * BUF_LINES);
#else
  static lv_color_t buf1[DISPLAY_WIDTH * BUF_LINES];
  lv_disp_draw_buf_init(&draw_buf, buf1, nullptr, DISPLAY_WIDTH * BUF_LINES);
#endif

  lv_disp_drv_init(&disp_drv);
  disp_drv.hor_res = DISPLAY_WIDTH;
  disp_drv.ver_res = DISPLAY_HEIGHT;
  disp_drv.flush_cb = disp_flush_cb;
  disp_drv.draw_buf = &draw_buf;
  lv_disp_drv_register(&disp_drv);

  esp_timer_create_args_t timer_args = {};
  timer_args.callback = lv_tick_timer_cb;
  timer_args.name = "lv_tick";
  esp_timer_handle_t timer = nullptr;
  esp_timer_create(&timer_args, &timer);
  esp_timer_start_periodic(timer, 1000);  // 1ms period -> lv_tick_inc(1)
}

void lvgl_task_handler() {
  lv_timer_handler();
}

// Touch indev: real on boards with a touch panel, no-op otherwise (e.g. M5
// Core, which gets the display half of this bridge but has no touchscreen).
#if HAS_TOUCH_INPUT

static lv_indev_drv_t indev_drv;

static void touch_read_cb(lv_indev_drv_t *drv, lv_indev_data_t *data) {
  int32_t x = 0;
  int32_t y = 0;
  if (lcd_ptr->getTouch(&x, &y)) {
    data->state = LV_INDEV_STATE_PRESSED;
    data->point.x = x;
    data->point.y = y;
  } else {
    data->state = LV_INDEV_STATE_RELEASED;
  }
}

void lvgl_touch_init(LGFX &lcd) {
  lcd_ptr = &lcd;
  lv_indev_drv_init(&indev_drv);
  indev_drv.type = LV_INDEV_TYPE_POINTER;
  indev_drv.read_cb = touch_read_cb;
  lv_indev_drv_register(&indev_drv);
}

#else  // !HAS_TOUCH_INPUT

void lvgl_touch_init(LGFX &) {}

#endif  // HAS_TOUCH_INPUT
