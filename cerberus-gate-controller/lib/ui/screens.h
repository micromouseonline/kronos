#ifndef EEZ_LVGL_UI_SCREENS_H
#define EEZ_LVGL_UI_SCREENS_H

#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

// Screens

enum ScreensEnum {
    _SCREEN_ID_FIRST = 1,
    SCREEN_ID_MENU = 1,
    SCREEN_ID_MAIN = 2,
    SCREEN_ID_SPLASH = 3,
    _SCREEN_ID_LAST = 3
};

typedef struct _objects_t {
    lv_obj_t *menu;
    lv_obj_t *main;
    lv_obj_t *splash;
    lv_obj_t *obj0;
    lv_obj_t *obj1;
    lv_obj_t *obj2;
    lv_obj_t *obj3;
    lv_obj_t *obj4;
    lv_obj_t *obj5;
    lv_obj_t *obj6;
    lv_obj_t *obj7;
    lv_obj_t *obj8;
    lv_obj_t *obj9;
    lv_obj_t *lbl_settings;
    lv_obj_t *obj10;
    lv_obj_t *obj11;
    lv_obj_t *btn_arm;
    lv_obj_t *obj12;
    lv_obj_t *btn_start;
    lv_obj_t *obj13;
    lv_obj_t *btn_goal;
    lv_obj_t *obj14;
    lv_obj_t *btn_touch;
    lv_obj_t *obj15;
    lv_obj_t *obj16;
    lv_obj_t *obj17;
    lv_obj_t *run_times;
    lv_obj_t *obj18;
    lv_obj_t *obj19;
    lv_obj_t *main_title;
    lv_obj_t *obj20;
    lv_obj_t *main_title_1;
} objects_t;

extern objects_t objects;

void create_screen_menu();
void tick_screen_menu();

void create_screen_main();
void tick_screen_main();

void create_screen_splash();
void tick_screen_splash();

void tick_screen_by_id(enum ScreensEnum screenId);
void tick_screen(int screen_index);

void create_screens();

#ifdef __cplusplus
}
#endif

#endif /*EEZ_LVGL_UI_SCREENS_H*/