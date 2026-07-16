#ifndef EEZ_LVGL_UI_FONTS_H
#define EEZ_LVGL_UI_FONTS_H

#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

extern const lv_font_t ui_font_dseg_40;
extern const lv_font_t ui_font_freesans48;
extern const lv_font_t ui_font_mono24_numbers;
extern const lv_font_t ui_font_mono54_numbers;
extern const lv_font_t ui_font_mono18;
extern const lv_font_t ui_font_mono16;

#ifndef EXT_FONT_DESC_T
#define EXT_FONT_DESC_T
typedef struct _ext_font_desc_t {
    const char *name;
    const void *font_ptr;
} ext_font_desc_t;
#endif

extern ext_font_desc_t fonts[];

#ifdef __cplusplus
}
#endif

#endif /*EEZ_LVGL_UI_FONTS_H*/