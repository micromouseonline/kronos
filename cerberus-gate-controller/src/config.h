// ----------------------------------------------------------------------------
//  config.h — Unified hardware profile & application tunables.
// ----------------------------------------------------------------------------
#pragma once

#include <stdint.h>

// ============================================================================
//  1. HARDWARE CONFIGURATION (DISPLAY & TOUCH)
//  Board wiring/pin facts live in lib/boards/ -- add new boards as sibling
//  files there, not as another branch here.
// ============================================================================

#include "boards/board-select.h"

// ============================================================================
//  2. APPLICATION SETTINGS & BEHAVIOR
// ============================================================================

// Local Input Polling Task period (Core 1) -- DESIGN-REQUIREMENT.md specifies
// GPIO/NeoKey/touch are all polled sequentially every 15ms from one task.
constexpr int INPUT_POLL_PERIOD_MS = 15;

// Hold threshold for NeoKey long-press gestures (TOUCH -> menu, ARM -> new
// mouse). See neokey-buttons.h / neokey/neokey-driver.h.
constexpr uint32_t NEOKEY_LONG_PRESS_MS = 2000;