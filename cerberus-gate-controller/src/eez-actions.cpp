// ----------------------------------------------------------------------------
//  eez-actions.cpp — Implements the action_* callbacks EEZ Studio's
//  generated lib/ui/actions.h declares (button clicks etc. from the
//  EEZ-designed screens). Hand-written and never touched by an EEZ Studio
//  re-export, unlike everything under lib/ui/.
//
//  Unconditional, even on boards with no touch panel (e.g. M5 Core):
//  lib/ui/screens.c registers these as event callbacks at screen-creation
//  time regardless of whether touch input ever actually fires them, so the
//  linker needs them defined on every board once lib/ui/ is referenced.
// ----------------------------------------------------------------------------
#include "ui/actions.h"
#include "ui/screens.h"
#include "ui/ui.h"

#include "debug-log.h"
#include "display/display.h"  // LGFX
#include "input-events.h"     // input_queue_post, InputSource, ButtonID
#include "net/wifi-credentials.h"
#include "net/wifi-manager.h"  // wifi_request_provisioning
#include "race/race-timer.h"   // race_timer_active

// Define callback type for confirmation result
typedef void (*confirm_cb_t)(bool confirmed, void *user_data);

typedef struct {
  confirm_cb_t cb;
  void *user_data;
} confirm_ctx_t;

static void msgbox_event_cb(lv_event_t *e) {
  lv_obj_t *mbox = lv_event_get_current_target(e);
  confirm_ctx_t *ctx = (confirm_ctx_t *)lv_event_get_user_data(e);

  // Get clicked button index (0 = OK, 1 = CANCEL)
  uint16_t btn_id = lv_msgbox_get_active_btn(mbox);
  bool confirmed = (btn_id == 0);

  if (ctx && ctx->cb) {
    ctx->cb(confirmed, ctx->user_data);
  }

  // Clean up dynamically allocated context memory
  if (ctx)
    lv_mem_free(ctx);

  // Close and destroy dialog
  lv_msgbox_close(mbox);
}

void show_confirm_dialog(const char *title, const char *msg, confirm_cb_t callback, void *user_data) {
  static const char *btns[] = {"OK", "Cancel", ""};

  // Allocate memory for context to pass callback through event user_data
  confirm_ctx_t *ctx = (confirm_ctx_t *)lv_mem_alloc(sizeof(confirm_ctx_t));
  if (!ctx)
    return;
  ctx->cb = callback;
  ctx->user_data = user_data;

  // Create modal message box (parent = NULL targets layer_top)
  lv_obj_t *mbox = lv_msgbox_create(NULL, title, msg, btns, true);
  lv_obj_center(mbox);

  // Attach handler to VALUE_CHANGED event emitted by button matrix
  lv_obj_add_event_cb(mbox, msgbox_event_cb, LV_EVENT_VALUE_CHANGED, ctx);
}

// EventType defaults to InputEventType::PRESSED if not provided
void action_on_timer_arm(lv_event_t *e) {
  input_queue_post(BTN_ARM, InputSource::TOUCH);
}

void action_on_timer_start(lv_event_t *e) {
  input_queue_post(BTN_START, InputSource::TOUCH);
}

void action_on_timer_goal(lv_event_t *e) {
  input_queue_post(BTN_GOAL, InputSource::TOUCH);
}

void action_on_timer_touch(lv_event_t *e) {
  input_queue_post(BTN_TOUCH, InputSource::TOUCH);
}

// Long-press callbacks (registered on btn_arm/btn_start/btn_goal's
// LV_EVENT_LONG_PRESSED in screens.c): post into the shared queue like
// every other producer's hold gesture. BUTTON_COMMAND_MAP
// (race/race-command-source.h) decides what each HELD event means
void action_on_timer_arm_long(lv_event_t *e) {
  input_queue_post(BTN_ARM, InputSource::TOUCH, InputEventType::HELD);
}

void action_on_timer_start_long(lv_event_t *e) {
  input_queue_post(BTN_START, InputSource::TOUCH, InputEventType::HELD);
}

void action_on_timer_goal_long(lv_event_t *e) {
  input_queue_post(BTN_GOAL, InputSource::TOUCH, InputEventType::HELD);
}

void action_on_timer_touch_long(lv_event_t *e) {
  input_queue_post(BTN_TOUCH, InputSource::TOUCH, InputEventType::HELD);
}

// Menu -> maze timer screen navigation. UI navigation only, not a
// ButtonID/input_queue_post event.
void action_on_menu_maze_timer(lv_event_t *e) {
  loadScreen(SCREEN_ID_MAIN);
}

void action_on_menu_calibrate(lv_event_t *e) {
  debug_println("CALIBRATE!");
}

static void on_reset_confirmed(bool confirmed, void *user_data) {
  if (confirmed) {
    ESP.restart();
  }
}

void action_on_menu_reset(lv_event_t *e) {
  show_confirm_dialog("System Restart", "Are you sure?", on_reset_confirmed, NULL);
}
}

// Just navigation -- the actual decision is the wifi_setup screen's two
// buttons below, so a stray tap here is a total no-op.
void action_on_menu_setup(lv_event_t *e) {
  String ssid_text = "SSID: ";
  ssid_text += (WiFi.status() == WL_CONNECTED) ? WiFi.SSID() : "Not connected";
  lv_label_set_text(objects.lbl_wifi_ssid, ssid_text.c_str());
  // LV_SIZE_CONTENT means the label's width just changed to match the new
  // text above -- re-centre now that it's known, since SSIDs vary in length
  // (up to 32 chars) and a fixed x position would run long ones off-screen.
  lv_obj_align(objects.lbl_wifi_ssid, LV_ALIGN_TOP_MID, 0, 37);
  lv_scr_load(objects.wifi_setup);
}

// "Return" button: leave Wi-Fi untouched, back to the menu.
void action_on_wifi_setup_return(lv_event_t *e) {
  lv_scr_load(objects.menu);
}

// "New network" button: wipes saved credentials and forces the config
// portal open on wifi_connect_task's next poll tick (net/wifi-manager.h's
// wifi_request_provisioning()), regardless of whether the current network
// is still connected. wifi_provisioning_start() (net/wifi-provisioning.h)
// takes over the LCD directly once that fires, so no further screen
// navigation is needed here.
void action_on_wifi_setup_confirm(lv_event_t *e) {
  debug_println("[SYSTEM] Wi-Fi reconfigure requested: clearing saved credentials");
  wifi_credentials_clear();
  wifi_request_provisioning();
}
