#pragma once

#include <Arduino.h>
#include <memory>

class IStatusLED {
 public:
  virtual void begin() = 0;
  virtual void setRGB(uint8_t r, uint8_t g, uint8_t b) = 0;
  virtual void turnOff() { setRGB(0, 0, 0); }
  virtual void turnOn() { setRGB(25, 0, 0); }  //
  virtual void toggle() {
    m_state = 1 - m_state;
    m_state ? turnOn() : turnOff();
  }
  virtual ~IStatusLED() {}

 private:
  uint8_t m_state = 0;
};

/////////////////////////////////////////////////////////////////////////////
class NoLED : public IStatusLED {
 public:
  NoLED() {}

  void begin() override {}

  void setRGB(uint8_t r, uint8_t g, uint8_t b) override {}
};

/////////////////////////////////////////////////////////////////////////////
class SimpleLED : public IStatusLED {
 private:
  uint8_t m_pin;
  bool m_activeLow;

 public:
  SimpleLED(uint8_t pin, bool activeLow = false) : m_pin(pin), m_activeLow(activeLow) {}

  void begin() override {
    pinMode(m_pin, OUTPUT);
    turnOff();
  }

  void turnOff() override { digitalWrite(m_pin, m_activeLow ? 1 : 0); }

  void turnOn() override { digitalWrite(m_pin, m_activeLow ? 0 : 1); }

  void setRGB(uint8_t r, uint8_t g, uint8_t b) override {
    bool state = (r > 0 || g > 0 || b > 0);
    digitalWrite(m_pin, m_activeLow ? !state : state);
  }
};

/////////////////////////////////////////////////////////////////////////////
#ifdef HAS_RGB

class StandardRGBLED : public IStatusLED {
 private:
  uint8_t m_rPin, m_gPin, m_bPin;

 public:
  StandardRGBLED(uint8_t r, uint8_t g, uint8_t b) : m_rPin(r), m_gPin(g), m_bPin(b) {}

  void begin() override {
    pinMode(m_rPin, OUTPUT);
    pinMode(m_gPin, OUTPUT);
    pinMode(m_bPin, OUTPUT);
    turnOff();
  }

  void setRGB(uint8_t r, uint8_t g, uint8_t b) override {
    analogWrite(m_rPin, r);
    analogWrite(m_gPin, g);
    analogWrite(m_bPin, b);
  }
};

#endif  // HAS_RGB

/////////////////////////////////////////////////////////////////////////////
#ifdef HAS_NEOPIXEL

#include <Adafruit_NeoPixel.h>

#ifndef NEOPIXEL_COLOR_ORDER
#define NEOPIXEL_COLOR_ORDER NEO_GRB
#endif

class NeoPixelStatusLED : public IStatusLED {
 private:
  Adafruit_NeoPixel m_strip;

 public:
  NeoPixelStatusLED(uint8_t pin, neoPixelType type) : m_strip(1, pin, type) {}

  void begin() override {
    m_strip.begin();
    turnOff();
  }

  void setRGB(uint8_t r, uint8_t g, uint8_t b) override {
    m_strip.setPixelColor(0, m_strip.Color(r, g, b));
    m_strip.show();
  }
};

#endif  // HAS_NEOPIXEL

/////////////////////////////////////////////////////////////////////////////

#ifndef LED_ACTIVE_LOW
#define LED_ACTIVE_LOW false
#endif

/// @brief Returns the correct LED driver for the current board.
inline std::unique_ptr<IStatusLED> createStatusLED() {
#if defined(HAS_NEOPIXEL)
  return std::unique_ptr<IStatusLED>(new NeoPixelStatusLED(STATUS_LED, NEOPIXEL_COLOR_ORDER + NEO_KHZ800));
#elif defined(HAS_RGB)
#error "HAS_RGB: define MY_RGB_R_PIN, MY_RGB_G_PIN, MY_RGB_B_PIN and implement this branch"
#elif defined(HAS_LED)
  return std::unique_ptr<IStatusLED>(new SimpleLED(STATUS_LED, LED_ACTIVE_LOW));
#else
  return std::unique_ptr<IStatusLED>(new NoLED());
#endif
}

/**
 * @brief Value-type handle for the board status LED.
 *
 * Declare a global instance and call begin() in setup().
 */
class StatusLED {
  std::unique_ptr<IStatusLED> m_impl;

 public:
  StatusLED() : m_impl(createStatusLED()) {}
  void begin() { m_impl->begin(); }
  void turnOn() { m_impl->turnOn(); }
  void turnOff() { m_impl->turnOff(); }
  void toggle() { m_impl->toggle(); }
  void setRGB(uint8_t r, uint8_t g, uint8_t b) { m_impl->setRGB(r, g, b); }
};
