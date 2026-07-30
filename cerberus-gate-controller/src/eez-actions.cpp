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
#include "net/wifi-credentials.h"
#include "net/wifi-manager.h"            // wifi_request_provisioning, g_wifi_rssi_report_enabled
#include "race/race-serial-telemetry.h"  // g_watchdog_tx_enabled
#include "race/race-timer.h"             // race_timer_active
#include "settings-store.h"              // settings_save_watchdog, settings_save_wifi_stats

// Variable to hold the caller screen object/ID
static lv_obj_t *g_previous_screen = NULL;

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

// Menu -> maze timer screen navigation. Also the one-shot activation
// trigger for the race state machine (race_timer_active, race-timer.h):
// button/serial/HTTP traffic is ignored until this fires, so the timer
// can't start advancing while the operator is still sitting on the main
// menu. race_timer_active is never cleared again, so later trips back to
// the menu screen (BTN_TOUCH HELD) leave an in-progress race running.
void action_on_menu_maze_timer(lv_event_t *e) {
  race_timer_active = true;
  loadScreen(SCREEN_ID_MAIN);
}

void action_on_menu_calibrate(lv_event_t *e) {
  debug_log_enqueue("CALIBRATE!");
}

static void on_reset_confirmed(bool confirmed, void *user_data) {
  if (confirmed) {
    ESP.restart();
  }
}

void action_on_menu_reset(lv_event_t *e) {
  show_confirm_dialog("System Restart", "Are you sure?", on_reset_confirmed, NULL);
}

// Defined in main.cpp -- refreshes the NeoKey LEDs from race_state
// immediately, rather than waiting for the next physical button press to
// trigger neokey_reflect_race_state() via input_event_handler.
extern void neokey_reflect_race_state();

// Menu -> re-enter CALIBRATE from any race state, abandoning whatever
// mouse was in progress (race_timer_handle_command's ENTER_CALIBRATION
// applies regardless of current race_state). Sets race_timer_active like
// action_on_menu_maze_timer above -- GATE TEST is an equally explicit
// entry into the timer subsystem, so it must also unlock command
// processing if it's the first screen visited after boot.
void action_on_menu_gate_test(lv_event_t *e) {
  race_timer_active = true;
  debug_log_enqueue("GATE TEST!");
  race_timer_handle_command(RaceCommand::ENTER_CALIBRATION);
  neokey_reflect_race_state();
  loadScreen(SCREEN_ID_MAIN);
}

// Menu -> Settings. Syncs all three switches to the current in-memory flag
// state every time the screen is opened, rather than relying on screens.c's
// hardcoded initial CHECKED state (which predates persistence and doesn't
// match any flag's real default).
void action_on_menu_settings(lv_event_t *e) {
  if (g_watchdog_tx_enabled) {
    lv_obj_add_state(objects.sw_watchdog, LV_STATE_CHECKED);
  } else {
    lv_obj_clear_state(objects.sw_watchdog, LV_STATE_CHECKED);
  }
  if (g_wifi_rssi_report_enabled) {
    lv_obj_add_state(objects.sw_wifi_stats, LV_STATE_CHECKED);
  } else {
    lv_obj_clear_state(objects.sw_wifi_stats, LV_STATE_CHECKED);
  }
  if (g_debug_verbose_enabled) {
    lv_obj_add_state(objects.sw_debug_verbose, LV_STATE_CHECKED);
  } else {
    lv_obj_clear_state(objects.sw_debug_verbose, LV_STATE_CHECKED);
  }
  loadScreen(SCREEN_ID_SETTINGS);
}

// Shared VALUE_CHANGED handler for all three switches -- identifies which
// one fired via the event target, updates the matching global, and
// persists it immediately (settings-store.h), same "write straight
// through" convention as wifi_credentials_save() below.
void action_on_settings_change(lv_event_t *e) {
  lv_obj_t *target = lv_event_get_target(e);
  bool checked = lv_obj_has_state(target, LV_STATE_CHECKED);
  if (target == objects.sw_watchdog) {
    g_watchdog_tx_enabled = checked;
    settings_save_watchdog(checked);
  } else if (target == objects.sw_wifi_stats) {
    g_wifi_rssi_report_enabled = checked;
    settings_save_wifi_stats(checked);
  } else if (target == objects.sw_debug_verbose) {
    g_debug_verbose_enabled = checked;
    settings_save_debug_verbose(checked);
  }
}

void action_on_settings_return(lv_event_t *e) {
  loadScreen(SCREEN_ID_MENU);
}

// Just navigation -- the actual decision is the wifi_setup screen's two
// buttons below, so a stray tap here is a total no-op.
void action_on_menu_setup(lv_event_t *e) {
  bool connected = (WiFi.status() == WL_CONNECTED);
  String info_text = "SSID: ";
  info_text += connected ? WiFi.SSID() : "Not connected";
  info_text += "\nIP: ";
  info_text += connected ? WiFi.localIP().toString() : "-";
  lv_label_set_text(objects.lbl_wifi_ssid, info_text.c_str());
  // LV_SIZE_CONTENT auto-fits the label's width to its widest line (the
  // SSID and IP lines are rarely the same length), so text_align also has
  // to be CENTER -- otherwise the shorter line just sits flush-left inside
  // that box instead of centering under the other one.
  lv_obj_set_style_text_align(objects.lbl_wifi_ssid, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
  // Re-centre the box itself now that its width is known, since SSIDs vary
  // in length (up to 32 chars) and a fixed x position would run long ones
  // off-screen.
  lv_obj_align(objects.lbl_wifi_ssid, LV_ALIGN_TOP_MID, 0, 37);
  // Get currently active screen object directly from LVGL engine
  g_previous_screen = lv_scr_act();
  lv_scr_load(objects.wifi_setup);
}

// "Return" button: leave Wi-Fi untouched, back to the menu.
void action_on_wifi_setup_return(lv_event_t *e) {
  if (g_previous_screen != NULL) {
    // lv_scr_load_anim(g_previous_screen, LV_SCR_LOAD_ANIM_FADE_ON, 200, 0, false);
    // lv_scr_load(objects.menu);
    lv_scr_load(g_previous_screen);
  }
}

// "New network" button: wipes saved credentials and forces the config
// portal open on wifi_connect_task's next poll tick (net/wifi-manager.h's
// wifi_request_provisioning()), regardless of whether the current network
// is still connected. wifi_provisioning_start() (net/wifi-provisioning.h)
// takes over the LCD directly once that fires, so no further screen
// navigation is needed here.
void action_on_wifi_setup_confirm(lv_event_t *e) {
  debug_log_enqueue("[SYSTEM] Wi-Fi reconfigure requested: clearing saved credentials");
  wifi_credentials_clear();
  wifi_request_provisioning();
}
