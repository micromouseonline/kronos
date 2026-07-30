// ----------------------------------------------------------------------------
//  neokey-driver.h — Low-level Adafruit NeoKey 1x4 (seesaw, I2C) hardware
//  driver. Owns the I2C bus and the one physical device instance; everything
//  else touching the NeoKey (neokey-buttons.h, neokey-pixels.h) reads/writes
//  through the single `neokey_device` object defined here rather than
//  re-initializing the seesaw chip themselves.
//
//  Lives in lib/neokey/ as its own PlatformIO library -- no other project in
//  this repo uses a NeoKey yet, but this is already reusable as-is if that
//  changes.
// ----------------------------------------------------------------------------
#pragma once

#include "boards/board-select.h"

#if HAS_NEOKEY_BUTTONS

#include <Arduino.h>
#include <Wire.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "Adafruit_NeoKey_1x4.h"
#include "debug-log.h"
#include "seesaw_neopixel.h"

// Guards the shared I2C bus (port 1) between poll_neokey_buttons()'s reads
// (neokey_device.update(), Core-1 input polling task) and any pixel-colour
// writes (neokey_device.setColour()/setAllColour(), called from the main
// task -- e.g. main.cpp's NeoKey colour cycle). This is unconditionally
// needed whenever HAS_NEOKEY_BUTTONS=1: reads and writes to the same
// TwoWire(1) instance from two different FreeRTOS tasks on two different
// cores are a real race, not a hypothetical one, the moment anything calls
// neokey_set_colour()/neokey_set_all() while input_poll_task is running.
inline SemaphoreHandle_t neokey_bus_mutex = nullptr;
inline void neokey_bus_lock() {
  if (neokey_bus_mutex != nullptr) {
    xSemaphoreTake(neokey_bus_mutex, portMAX_DELAY);
  }
}
inline void neokey_bus_unlock() {
  if (neokey_bus_mutex != nullptr) {
    xSemaphoreGive(neokey_bus_mutex);
  }
}

// NeoPixel colour helpers (0xRRGGBB), used by neokey-pixels.h.
constexpr uint32_t NP_RED = 0xFF0000;
constexpr uint32_t NP_GREEN = 0x00FF00;
constexpr uint32_t NP_BLUE = 0x0000FF;  // pretty dark
constexpr uint32_t NP_YELLOW = 0xFF7F00;
constexpr uint32_t NP_MAGENTA = 0xFF00FF;
constexpr uint32_t NP_CYAN = 0x00FFFF;
constexpr uint32_t NP_WHITE = 0xFFFFFF;
constexpr uint32_t NP_OFF = 0x000000;

// NeoKey's fixed hardware default I2C address (not board-specific).
constexpr uint8_t NEOKEY_I2C_ADDR = 0x30;

class Neokey {
 public:
  // I2C port 1, not 0 -- LovyanGFX's I2C touch driver (display.h,
  // cfg.i2c_port = 0) owns port 0 directly through ESP-IDF, not through
  // Arduino's Wire object. A future touch+NeoKey board needs the NeoKey on
  // its own bus to avoid contention. PIN_NEOKEY_SDA/PIN_NEOKEY_SCL are owned
  // by whichever board profile in lib/boards/*.h sets
  // HAS_NEOKEY_BUTTONS=1 (not defined here), same pattern as PIN_TOUCH_SDA.
  Neokey() : myWire(1), neokey(NEOKEY_I2C_ADDR, &myWire) {}

  void setup() {
    myWire.begin(PIN_NEOKEY_SDA, PIN_NEOKEY_SCL);
    // Doesn't bound a single stalled transaction below ~1s (Wire's software
    // timeout default is already 50ms, Wire.cpp, but ESP-IDF's
    // i2c_master_cmd_begin() has a hardcoded 1000ms-minimum retry
    // granularity no software timeout can go below) -- kept anyway as the
    // correct/intended call for anything that *does* ack quickly.
    myWire.setTimeOut(50);

    // Quick presence probe -- one raw transaction (~1s worst case, per the
    // 1s floor above) instead of handing straight to Adafruit_seesaw::begin(),
    // which retries up to 30 times across its internal stages when nothing
    // acks (~10-30s worst case, confirmed on Freenove/ESP32-S3 hardware).
    // This runs inside init_neokey_device()'s lock (see below), which
    // poll_neokey_buttons() and the pixel setters now skip entirely via
    // isAvailable() rather than blocking on -- but other code could still
    // stall waiting for this lock during the probe, so bounding it to ~1s
    // instead of ~10-30s matters even with that fix in place.
    myWire.beginTransmission(NEOKEY_I2C_ADDR);
    if (myWire.endTransmission() != 0) {
      available = false;
      debug_log_enqueue("[NEOKEY] not found (quick probe), check wiring");
      return;
    }

    available = neokey.begin(NEOKEY_I2C_ADDR);
    if (!available) {
      // Reaching here means the quick probe above got an ACK but the full
      // seesaw handshake still failed -- a real device is present at this
      // address but isn't behaving like a NeoKey (unlikely, but possible
      // with address collisions). Deliberately not a while(1) hang: this
      // runs from a background task (neokey_buttons_init(),
      // neokey-buttons.h), but must still not wedge that task forever.
      debug_log_enqueue("[NEOKEY] not found, check wiring");
      return;
    }
    setAllColour(NP_OFF);  // Turn off all pixels initially
    raw_buttons = neokey.read();
  }

  bool isAvailable() const { return available; }

  uint8_t update() {
    if (!available) {
      return raw_buttons;
    }
    raw_buttons = neokey.read();
    uint32_t now = millis();
    last_debounced = debounced_buttons;  // snapshot before any state changes

    for (uint8_t i = 0; i < NUM_KEYS; i++) {
      bool raw = (raw_buttons & (1 << i)) != 0;
      bool debounced = (debounced_buttons & (1 << i)) != 0;

      if (raw != debounced) {
        if (now - last_change_time[i] >= DEBOUNCE_INTERVAL) {
          if (raw) {
            press_start_time[i] = now;
          } else {
            release_time[i] = now;
          }

          // Commit the new stable state
          if (raw) {
            debounced_buttons |= (1 << i);
          } else {
            debounced_buttons &= ~(1 << i);
          }

          last_change_time[i] = now;
        }
      } else {
        last_change_time[i] = now;  // Still stable, reset change timer
      }
    }

    return raw_buttons;
  }

  uint8_t getButtons() { return debounced_buttons; }

  bool isPressed(uint8_t i) { return debounced_buttons & (1 << i); }

  bool wasPressed(uint8_t i) { return !(last_debounced & (1 << i)) && (debounced_buttons & (1 << i)); }

  bool wasReleased(uint8_t i) { return (last_debounced & (1 << i)) && !(debounced_buttons & (1 << i)); }

  bool wasReleasedAfter(uint8_t i, uint32_t hold_time_ms) { return wasReleased(i) && (millis() - press_start_time[i] >= hold_time_ms); }

  bool isLongPress(uint8_t i, uint32_t durationMs) {
    return isPressed(i) && getHoldTime(i) >= durationMs;  //
  }

  // Edge-triggered: true exactly once per hold, the poll tick where the hold
  // first reaches durationMs (not every tick for as long as it's held).
  // Resets when the key is released, so the next press/hold cycle can fire
  // again.
  bool wasLongPressed(uint8_t i, uint32_t durationMs) {
    if (!isPressed(i)) {
      long_reported[i] = false;
      return false;
    }
    if (!long_reported[i] && getHoldTime(i) >= durationMs) {
      long_reported[i] = true;
      return true;
    }
    return false;
  }

  bool wasLongReleased(uint8_t i, uint32_t durationMs) { return wasReleased(i) && (release_time[i] - press_start_time[i] >= durationMs); }

  bool isDoublePress(uint8_t i, uint32_t windowMs = 300) {
    static uint32_t last_release[NUM_KEYS] = {0};
    if (wasReleased(i)) {
      uint32_t now = millis();
      bool doubleTap = (now - last_release[i]) <= windowMs;
      last_release[i] = now;
      return doubleTap;
    }
    return false;
  }

  bool isCombo(uint8_t mask) { return (getButtons() == mask); }

  void waitUntilReleased(uint8_t button) {
    while (isPressed(button)) {
      update();   // read fresh state
      delay(10);  // debounce-friendly sleep
    }
  }

  int waitForButtons(uint8_t mask) {
    while ((getButtons() & mask) == 0) {
      update();   // read fresh state
      delay(10);  // debounce-friendly sleep
    }
    uint8_t buttons = getButtons();
    waitUntilAllReleased();
    return buttons;
  }

  void waitUntilAllReleased() {
    while (getButtons() != 0x00) {
      update();
      delay(10);
    }
  }

  uint32_t getHoldTime(uint8_t button) {
    if (button >= NEOKEY_1X4_KEYS || !isPressed(button))
      return 0;
    return millis() - press_start_time[button];
  }

  bool setColour(uint8_t button, uint32_t colour) {
    if (!available || button >= NEOKEY_1X4_KEYS) {
      return false;  // not present, or invalid button index
    }
    neokey.pixels.setPixelColor(button, colour);
    neokey.pixels.show();
    return true;
  }

  bool setAllColour(uint32_t colour) {
    if (!available) {
      return false;
    }
    for (int i = 0; i < NEOKEY_1X4_KEYS; i++) {
      neokey.pixels.setPixelColor(i, colour);
    }
    neokey.pixels.show();
    return true;
  }

 private:
  TwoWire myWire;
  Adafruit_NeoKey_1x4 neokey;
  bool available = false;  // true once neokey.begin() has succeeded in setup()

  static const uint8_t NUM_KEYS = NEOKEY_1X4_KEYS;

  uint8_t raw_buttons = 0;        // From neokey.read()
  uint8_t debounced_buttons = 0;  // Stable debounced state
  uint8_t last_debounced = 0;

  uint32_t last_change_time[NUM_KEYS] = {0};
  uint32_t press_start_time[NUM_KEYS] = {0};
  uint32_t release_time[NUM_KEYS] = {0};
  bool long_reported[NUM_KEYS] = {false};  // wasLongPressed() edge-fire latch per key

  const uint32_t DEBOUNCE_INTERVAL = 20;  // ms
};

inline Neokey neokey_device;

inline void init_neokey_device() {
  neokey_bus_mutex = xSemaphoreCreateMutex();
  // Locked (unlike the old synchronous-boot version of this code) --
  // neokey_buttons_init() (neokey-buttons.h) now runs this from a one-shot
  // background task (neokey_init_task below) instead of blocking
  // app_setup(), specifically so an absent module's worst-case ~10s
  // detection stall (ESP32-S3, see Neokey::setup()'s comment) never delays
  // the Supervisor screen or GPIO/touch polling. That means this can now
  // genuinely run concurrently with poll_neokey_buttons() on Core 1 (the
  // old "safe because the task hasn't started yet" reasoning no longer
  // holds), so it needs the same bus lock every other I2C-touching call
  // already takes.
  neokey_bus_lock();
  neokey_device.setup();
  neokey_bus_unlock();
}

// One-shot background task: runs init_neokey_device() (which can stall for
// several seconds with no module attached) off the main task, so
// app_setup() never blocks on NeoKey detection. Self-deletes when done --
// poll_neokey_buttons() picks up the result via Neokey::isAvailable(),
// whichever task sets it.
inline void neokey_init_task(void*) {
  init_neokey_device();
  vTaskDelete(nullptr);
}

#endif  // HAS_NEOKEY_BUTTONS
