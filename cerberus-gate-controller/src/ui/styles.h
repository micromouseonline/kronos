#ifndef EEZ_LVGL_UI_STYLES_H
#define EEZ_LVGL_UI_STYLES_H

#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

// Style: timer_panel_blue
lv_style_t *get_style_timer_panel_blue_MAIN_DEFAULT();
void add_style_timer_panel_blue(lv_obj_t *obj);
void remove_style_timer_panel_blue(lv_obj_t *obj);

// Style: menu_option_panel
lv_style_t *get_style_menu_option_panel_MAIN_DEFAULT();
void add_style_menu_option_panel(lv_obj_t *obj);
void remove_style_menu_option_panel(lv_obj_t *obj);

// Style: custom_switch
lv_style_t *get_style_custom_switch_MAIN_DEFAULT();
lv_style_t *get_style_custom_switch_KNOB_DEFAULT();
lv_style_t *get_style_custom_switch_INDICATOR_CHECKED();
void add_style_custom_switch(lv_obj_t *obj);
void remove_style_custom_switch(lv_obj_t *obj);

#ifdef __cplusplus
}
#endif

#endif /*EEZ_LVGL_UI_STYLES_H*/