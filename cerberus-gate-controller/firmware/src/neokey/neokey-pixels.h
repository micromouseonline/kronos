// ----------------------------------------------------------------------------
//  neokey-pixels.h — NeoKey 1x4 LED-control facade. Independent of the
//  button/input-event side (neokey-buttons.h): this header never touches
//  ButtonID/InputSource/input_queue_post, purely arbitrary key-index colour
//  control, so status-colour feedback doesn't need to know anything about
//  button debounce internals and vice versa. Both facades read/write the
//  same shared `neokey_device` instance (neokey-driver.h) -- there's only
//  one physical seesaw chip on one I2C address, so there's exactly one
//  owner of its init (neokey-buttons.h's neokey_buttons_init(), which hands
//  it to a background task rather than running it synchronously -- see
//  neokey-driver.h). Calling the setters below before that background init
//  has finished is a normal, expected runtime state, not a caller error --
//  they check Neokey::isAvailable() the same way poll_neokey_buttons() does
//  before touching the bus, so they no-op cleanly (return false) rather
//  than blocking on neokey_bus_mutex for however long init is still
//  running.
// ----------------------------------------------------------------------------
#pragma once

#include "boards/board-select.h"

struct KeyColours {
  uint32_t colour_0;
  uint32_t colour_1;
  uint32_t colour_2;
  uint32_t colour_3;
};

#if HAS_NEOKEY_BUTTONS

#include "neokey-driver.h"

inline bool neokey_set_colour(uint8_t key, uint32_t colour) {
  if (!neokey_device.isAvailable()) {
    return false;
  }
  neokey_bus_lock();
  bool ok = neokey_device.setColour(key, colour);
  neokey_bus_unlock();
  return ok;
}

inline bool neokey_set_colours(const KeyColours &colours) {
  if (!neokey_device.isAvailable()) {
    return false;
  }
  neokey_bus_lock();
  bool ok = neokey_device.setColour(0, colours.colour_0);
  ok = neokey_device.setColour(1, colours.colour_1) && ok;
  ok = neokey_device.setColour(2, colours.colour_2) && ok;
  ok = neokey_device.setColour(3, colours.colour_3) && ok;
  neokey_bus_unlock();
  return ok;
}

inline bool neokey_set_all(uint32_t colour) {
  if (!neokey_device.isAvailable()) {
    return false;
  }
  neokey_bus_lock();
  bool ok = neokey_device.setAllColour(colour);
  neokey_bus_unlock();
  return ok;
}

#else

inline bool neokey_set_colour(uint8_t, uint32_t) {
  return false;
}
inline bool neokey_set_colours(const KeyColours &) {
  return false;
}
inline bool neokey_set_all(uint32_t) {
  return false;
}

#endif
