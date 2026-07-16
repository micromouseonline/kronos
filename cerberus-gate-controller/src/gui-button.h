// ----------------------------------------------------------------------------
//  gui-button.h — ButtonID, shared by every input producer (gpio-buttons.h,
//  neokey-buttons.h, eez-actions.cpp) and the input queue (input-events.h)
//  as the one common vocabulary for "which logical button".
//
//  Used to also hold the raw-LGFX on-screen button bar (BUTTON_MENU,
//  CustomButton) that fed the old Supervisor UI -- removed now that the
//  EEZ Studio/LVGL screens (lib/ui/) own on-screen buttons directly.
// ----------------------------------------------------------------------------
#pragma once

enum ButtonID { BTN_ARM, BTN_START, BTN_GOAL, BTN_RESET, NUM_BUTTONS };
