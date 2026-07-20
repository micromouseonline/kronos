#pragma once

#include <Arduino.h>

class DebouncedButton {
 private:
  int _pin;
  unsigned long _debounceDelay;
  unsigned long _lastDebounceTime;
  bool _lastState;
  bool _activeLow;
  bool _longReported = false;

 public:
  // Constructor: activeLow defaults to true if not specified
  DebouncedButton(int pin, bool activeLow = true, unsigned long debounceDelay = 200) {
    _pin = pin;
    _activeLow = activeLow;
    _debounceDelay = debounceDelay;
    _lastDebounceTime = 0;

    // Set the initial expected idle state based on active polarity
    _lastState = _activeLow ? HIGH : LOW;
  }

  // Call this in setup()
  void begin() {
    if (_activeLow) {
      // Active Low uses internal pull-up resistor
      pinMode(_pin, INPUT_PULLUP);
    } else {
      // Active High requires an external pull-down resistor on the hardware side
      pinMode(_pin, INPUT);
    }
  }

  // Call this inside app_loop(). Returns true ONLY on a valid, debounced trigger.
  bool wasPressed() {
    bool currentState = digitalRead(_pin);
    bool pressedTriggered = false;
    unsigned long currentMillis = millis();

    // Did the state change at all? (Press or Release)
    if (currentState != _lastState) {
      if ((currentMillis - _lastDebounceTime) > _debounceDelay) {
        // Trigger condition depends on configuration:
        // Active Low triggers when pin goes LOW. Active High triggers when pin goes HIGH.
        if (_activeLow && currentState == LOW) {
          pressedTriggered = true;
        } else if (!_activeLow && currentState == HIGH) {
          pressedTriggered = true;
        }

        // Update the timer and state on a stable change
        _lastDebounceTime = currentMillis;
        _lastState = currentState;
      }
    }
    return pressedTriggered;
  }

  // Current debounced state (not edge-triggered).
  bool isPressed() const {
    return _activeLow ? (_lastState == LOW) : (_lastState == HIGH);
  }

  // Edge-triggered: true exactly once per hold, on the poll where the hold
  // first reaches durationMs (not every poll for as long as it's held).
  // Resets on release so the next press/hold cycle can fire again. Relies
  // on wasPressed() having been called this same poll to refresh
  // _lastDebounceTime/_lastState -- mirrors Neokey::wasLongPressed()'s
  // contract so every input producer posts InputEventType::HELD the same
  // way (input-events.h).
  bool wasLongPressed(unsigned long durationMs) {
    if (!isPressed()) {
      _longReported = false;
      return false;
    }
    if (!_longReported && (millis() - _lastDebounceTime) >= durationMs) {
      _longReported = true;
      return true;
    }
    return false;
  }
};
