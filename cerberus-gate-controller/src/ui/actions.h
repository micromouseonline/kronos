#ifndef EEZ_LVGL_UI_EVENTS_H
#define EEZ_LVGL_UI_EVENTS_H

#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

extern void action_on_timer_arm(lv_event_t * e);
extern void action_on_timer_start(lv_event_t * e);
extern void action_on_timer_goal(lv_event_t * e);
extern void action_on_timer_touch(lv_event_t * e);
extern void action_on_timer_touch_long(lv_event_t * e);
extern void action_on_menu_maze_timer(lv_event_t * e);
extern void action_on_timer_arm_long(lv_event_t * e);
extern void action_on_menu_calibrate(lv_event_t * e);
extern void action_on_timer_start_long(lv_event_t * e);
extern void action_on_timer_goal_long(lv_event_t * e);
extern void action_on_menu_setup(lv_event_t * e);
extern void action_on_wifi_setup_return(lv_event_t * e);
extern void action_on_wifi_setup_confirm(lv_event_t * e);
extern void action_on_menu_reset(lv_event_t * e);
extern void action_on_menu_gate_test(lv_event_t * e);

#ifdef __cplusplus
}
#endif

#endif /*EEZ_LVGL_UI_EVENTS_H*/