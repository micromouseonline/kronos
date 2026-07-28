// ----------------------------------------------------------------------------
//  board-select.h — Picks the active board's pin/capability profile based on
//  the BOARD_* macro set via -D build flags (see boards.ini). Add new
//  boards as sibling files in lib/boards/, not as another branch here.
// ----------------------------------------------------------------------------
#pragma once

#if defined(BOARD_CYD2USB_DIYMALLS_ILI9341)
#include "cyd2usb-diymalls-ili9341.h"
#elif defined(BOARD_CYD2USB_DIYMALLS_ST7789)
#include "cyd2usb-diymalls-st7789.h"
#elif defined(BOARD_JC2432W328C)
#include "jc2432w328c.h"
#else  // Default: Freenove FNK0104B Configuration
#include "cyd-touch-freenove.h"
#endif

#ifndef STATUS_LED
#error "STATUS_LED not defined. Set board defines in boards.ini."
#endif