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
    SCREEN_ID_WIFI_SETUP = 4,
    SCREEN_ID_SETTINGS = 5,
    _SCREEN_ID_LAST = 5
};

typedef struct _objects_t {
    lv_obj_t *menu;
    lv_obj_t *main;
    lv_obj_t *splash;
    lv_obj_t *wifi_setup;
    lv_obj_t *settings;
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
    lv_obj_t *pnl_mouse_name;
    lv_obj_t *lbl_mouse_name;
    lv_obj_t *pnl_run_times;
    lv_obj_t *obj16;
    lv_obj_t *lbl_run_time_list;
    lv_obj_t *obj17;
    lv_obj_t *lbl_leaderboard_list;
    lv_obj_t *panel_current_run_time;
    lv_obj_t *lbl_current_run_time;
    lv_obj_t *pnl_run_number;
    lv_obj_t *lbl_run_number;
    lv_obj_t *pnl_time_remaining;
    lv_obj_t *lbl_time_remaining;
    lv_obj_t *lbl_version;
    lv_obj_t *lbl_wifi_ssid;
    lv_obj_t *obj18;
    lv_obj_t *obj19;
    lv_obj_t *obj20;
    lv_obj_t *sw_watchdog;
    lv_obj_t *obj21;
    lv_obj_t *sw_wifi_stats;
    lv_obj_t *obj22;
} objects_t;

extern objects_t objects;

void create_screen_menu();
void tick_screen_menu();

void create_screen_main();
void tick_screen_main();

void create_screen_splash();
void tick_screen_splash();

void create_screen_wifi_setup();
void tick_screen_wifi_setup();

void create_screen_settings();
void tick_screen_settings();

void tick_screen_by_id(enum ScreensEnum screenId);
void tick_screen(int screen_index);

void create_screens();

#ifdef __cplusplus
}
#endif

#endif /*EEZ_LVGL_UI_SCREENS_H*/