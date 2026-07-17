// ----------------------------------------------------------------------------
//  display.h — Per-board LovyanGFX (LGFX) display/touch driver config.
//
//  Display:  ILI9341 (240x320 IPS) over SPI, HSPI/SPI3_HOST.
//  Touch:    FT6336U (FocalTech FT6x36 family) over I²C, 0x38.
//  Backlight pin 45, active-HIGH.
//
//  Reference: Freenove's official TFT_eSPI_Setups/FNK0104B_2.8_240x320_ILI9341.h
//  https://github.com/Freenove/Freenove_ESP32_S3_Display
// ----------------------------------------------------------------------------
#pragma once

#define LGFX_USE_V1
#include "boards/board-select.h"

#if defined(BOARD_M5_CORE)
// Use LovyanGFX's runtime auto-detection to configure the M5Stack Core panel/backlight.
// This board has no touchscreen (3 hardware buttons instead); autodetect simply
// won't find a touch chip, which is correct.
#define LGFX_AUTODETECT
#include <LovyanGFX.hpp>

#elif defined(BOARD_CYD2USB_DIYMALLS_ILI9341)  // Sunton CYD2USB, DIYMALLS variant with ILI9341 panel driver (ESP32-2432S028R family) Manual Configuration

#include <LovyanGFX.hpp>
class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ILI9341 _panel;
  lgfx::Bus_SPI _bus;
  lgfx::Light_PWM _light;
  lgfx::Touch_XPT2046 _touch;  // resistive touch on its OWN dedicated SPI pins (SPI2/HSPI)

 public:
  LGFX() {
    {  // SPI bus — panel only. Touch below is a fully independent bus/pins.
      auto cfg = _bus.config();
      cfg.spi_host = SPI3_HOST;
      cfg.spi_mode = 0;
      cfg.freq_write = SPI_WRITE_HZ;
      cfg.freq_read = SPI_WRITE_HZ / 2;
      cfg.spi_3wire = false;
      cfg.use_lock = true;
      cfg.dma_channel = SPI_DMA_CH_AUTO;
      cfg.pin_sclk = PIN_LCD_SCLK;
      cfg.pin_mosi = PIN_LCD_MOSI;
      cfg.pin_miso = PIN_LCD_MISO;
      cfg.pin_dc = PIN_LCD_DC;
      _bus.config(cfg);
      _panel.setBus(&_bus);
    }
    {  // Panel — dimensions MUST be the native portrait size of the
      // physical display (240×320). LovyanGFX swaps width/height
      // itself when you call setRotation() with a 90/270° rotation.
      auto cfg = _panel.config();
      cfg.pin_cs = PIN_LCD_CS;
      cfg.pin_rst = PIN_LCD_RST;
      cfg.pin_busy = -1;
      cfg.panel_width = PANEL_NATIVE_WIDTH;
      cfg.panel_height = PANEL_NATIVE_HEIGHT;
      cfg.offset_x = PANEL_X_OFFSET;
      cfg.offset_y = PANEL_Y_OFFSET;
      cfg.offset_rotation = PANEL_OFFSET_ROTATION;  // tune this if the image is mirrored
      cfg.dummy_read_pixel = 8;
      cfg.dummy_read_bits = 1;
      cfg.readable = false;
      cfg.invert = INVERT_COLORS;  // tune this if colors are inverted
      cfg.rgb_order = !BGR_ORDER;  // LovyanGFX: false = BGR
      cfg.dlen_16bit = false;
      cfg.bus_shared = false;  // touch is on a fully separate bus, nothing to share
      _panel.config(cfg);
    }
    {  // Backlight
      auto cfg = _light.config();
      cfg.pin_bl = PIN_LCD_BL;
      cfg.invert = !BL_ACTIVE_HIGH;  // Active-HIGH on this board -> invert = false
      cfg.freq = 12000;
      cfg.pwm_channel = 7;
      _light.config(cfg);
      _panel.setLight(&_light);
    }
    {  // Resistive touch — XPT2046 on its own dedicated SPI2/HSPI pins,
      // physically independent of the panel's SPI3/VSPI bus above.
      auto cfg = _touch.config();
      cfg.x_min = 0;
      cfg.x_max = PANEL_NATIVE_WIDTH - 1;
      cfg.y_min = 0;
      cfg.y_max = PANEL_NATIVE_HEIGHT - 1;
      cfg.pin_int = PIN_TOUCH_IRQ;
      cfg.bus_shared = false;
      cfg.offset_rotation = 0;
      cfg.spi_host = SPI2_HOST;
      cfg.freq = TOUCH_SPI_HZ;
      cfg.pin_sclk = PIN_TOUCH_CLK;
      cfg.pin_mosi = PIN_TOUCH_MOSI;
      cfg.pin_miso = PIN_TOUCH_MISO;
      cfg.pin_cs = PIN_TOUCH_CS;
      _touch.config(cfg);
      _panel.setTouch(&_touch);
    }
    setPanel(&_panel);
  }
};

#elif defined(BOARD_CYD2USB_DIYMALLS_ST7789)  // Sunton CYD2USB, DIYMALLS variant with ST7789 panel driver (ESP32-2432S028R family) -- confirmed against a
                                              // working TFT_eSPI reference config

#include <LovyanGFX.hpp>
class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ST7789 _panel;  // NOT ILI9341 -- this variant's panel chip differs from the ILI9341 variant's
  lgfx::Bus_SPI _bus;
  lgfx::Light_PWM _light;
  lgfx::Touch_XPT2046 _touch;  // resistive touch on its OWN dedicated SPI pins (SPI2/HSPI)

 public:
  LGFX() {
    {  // SPI bus — panel only. Touch below is a fully independent bus/pins.
      auto cfg = _bus.config();
      cfg.spi_host = SPI3_HOST;
      cfg.spi_mode = 0;
      cfg.freq_write = SPI_WRITE_HZ;
      cfg.freq_read = SPI_WRITE_HZ / 2;
      cfg.spi_3wire = false;
      cfg.use_lock = true;
      cfg.dma_channel = SPI_DMA_CH_AUTO;
      cfg.pin_sclk = PIN_LCD_SCLK;
      cfg.pin_mosi = PIN_LCD_MOSI;
      cfg.pin_miso = PIN_LCD_MISO;
      cfg.pin_dc = PIN_LCD_DC;
      _bus.config(cfg);
      _panel.setBus(&_bus);
    }
    {  // Panel — dimensions MUST be the native portrait size of the
      // physical display (240×320). LovyanGFX swaps width/height
      // itself when you call setRotation() with a 90/270° rotation.
      auto cfg = _panel.config();
      cfg.pin_cs = PIN_LCD_CS;
      cfg.pin_rst = PIN_LCD_RST;
      cfg.pin_busy = -1;
      cfg.panel_width = PANEL_NATIVE_WIDTH;
      cfg.panel_height = PANEL_NATIVE_HEIGHT;
      cfg.offset_x = PANEL_X_OFFSET;
      cfg.offset_y = PANEL_Y_OFFSET;
      cfg.offset_rotation = PANEL_OFFSET_ROTATION;  // tune this if the image is mirrored
      cfg.dummy_read_pixel = 8;
      cfg.dummy_read_bits = 1;
      cfg.readable = false;
      cfg.invert = INVERT_COLORS;  // tune this if colors are inverted
      cfg.rgb_order = !BGR_ORDER;  // LovyanGFX: false = BGR
      cfg.dlen_16bit = false;
      cfg.bus_shared = false;  // touch is on a fully separate bus, nothing to share
      _panel.config(cfg);
    }
    {  // Backlight
      auto cfg = _light.config();
      cfg.pin_bl = PIN_LCD_BL;
      cfg.invert = !BL_ACTIVE_HIGH;  // Active-HIGH on this board -> invert = false
      cfg.freq = 12000;
      cfg.pwm_channel = 7;
      _light.config(cfg);
      _panel.setLight(&_light);
    }
    {  // Resistive touch — XPT2046 on its own dedicated SPI2/HSPI pins,
      // physically independent of the panel's SPI3/VSPI bus above.
      auto cfg = _touch.config();
      cfg.x_min = 0;
      cfg.x_max = PANEL_NATIVE_WIDTH - 1;
      cfg.y_min = 0;
      cfg.y_max = PANEL_NATIVE_HEIGHT - 1;
      cfg.pin_int = PIN_TOUCH_IRQ;
      cfg.bus_shared = false;
      cfg.offset_rotation = 0;
      cfg.spi_host = SPI2_HOST;
      cfg.freq = TOUCH_SPI_HZ;
      cfg.pin_sclk = PIN_TOUCH_CLK;
      cfg.pin_mosi = PIN_TOUCH_MOSI;
      cfg.pin_miso = PIN_TOUCH_MISO;
      cfg.pin_cs = PIN_TOUCH_CS;
      _touch.config(cfg);
      _panel.setTouch(&_touch);
    }
    setPanel(&_panel);
  }
};

#elif defined(BOARD_JC2432W328C)  // Guition/JC JC2432W328C -- ST7789 panel (SPI) + CST820 touch (I2C), both cross-confirmed against working reference code for
                                  // this exact board

#include <LovyanGFX.hpp>
class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ST7789 _panel;
  lgfx::Bus_SPI _bus;
  lgfx::Light_PWM _light;
  lgfx::Touch_CST816S _touch;  // CST816S driver covers CST820 -- register-compatible Hynitron siblings, same default I2C address (0x15)

 public:
  LGFX() {
    {  // SPI bus — panel only. Touch is I2C, on a completely separate bus.
      auto cfg = _bus.config();
      cfg.spi_host = SPI3_HOST;
      cfg.spi_mode = 0;
      cfg.freq_write = SPI_WRITE_HZ;
      cfg.freq_read = SPI_WRITE_HZ / 2;
      cfg.spi_3wire = false;
      cfg.use_lock = true;
      cfg.dma_channel = 1;  // matches the working reference demo (not SPI_DMA_CH_AUTO)
      cfg.pin_sclk = PIN_LCD_SCLK;
      cfg.pin_mosi = PIN_LCD_MOSI;
      cfg.pin_miso = PIN_LCD_MISO;
      cfg.pin_dc = PIN_LCD_DC;
      _bus.config(cfg);
      _panel.setBus(&_bus);
    }
    {  // Panel — dimensions MUST be the native portrait size of the
      // physical display (240×320). LovyanGFX swaps width/height
      // itself when you call setRotation() with a 90/270° rotation.
      auto cfg = _panel.config();
      cfg.pin_cs = PIN_LCD_CS;
      cfg.pin_rst = PIN_LCD_RST;
      cfg.pin_busy = -1;
      cfg.panel_width = PANEL_NATIVE_WIDTH;
      cfg.panel_height = PANEL_NATIVE_HEIGHT;
      cfg.offset_x = PANEL_X_OFFSET;
      cfg.offset_y = PANEL_Y_OFFSET;
      cfg.offset_rotation = PANEL_OFFSET_ROTATION;  // tune this if the image is mirrored
      cfg.dummy_read_pixel = 8;
      cfg.dummy_read_bits = 1;
      cfg.readable = false;
      cfg.invert = INVERT_COLORS;  // tune this if colors are inverted
      cfg.rgb_order = !BGR_ORDER;  // LovyanGFX: false = BGR
      cfg.dlen_16bit = false;
      cfg.bus_shared = false;  // touch is I2C, on a separate bus -- nothing to share
      _panel.config(cfg);
    }
    {  // Backlight
      auto cfg = _light.config();
      cfg.pin_bl = PIN_LCD_BL;
      cfg.invert = !BL_ACTIVE_HIGH;  // Active-HIGH on this board -> invert = false
      cfg.freq = 12000;
      cfg.pwm_channel = 7;
      _light.config(cfg);
      _panel.setLight(&_light);
    }
    {  // Capacitive touch — CST820 on I2C, fully independent of the panel's SPI bus.
      auto cfg = _touch.config();
      cfg.x_min = 0;
      cfg.x_max = PANEL_NATIVE_WIDTH - 1;
      cfg.y_min = 0;
      cfg.y_max = PANEL_NATIVE_HEIGHT - 1;
      cfg.pin_int = PIN_TOUCH_INT;
      cfg.pin_rst = PIN_TOUCH_RST;
      cfg.bus_shared = false;
      cfg.offset_rotation = 0;
      cfg.i2c_port = 0;
      cfg.i2c_addr = TOUCH_I2C_ADDR;
      cfg.pin_sda = PIN_TOUCH_SDA;
      cfg.pin_scl = PIN_TOUCH_SCL;
      cfg.freq = TOUCH_I2C_HZ;
      _touch.config(cfg);
      _panel.setTouch(&_touch);
    }
    setPanel(&_panel);
  }
};

#elif defined(BOARD_S3_CYD_TOUCH_FREENOVE)  // Freenove FNK0104B Board Manual Configuration

#include <LovyanGFX.hpp>
class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ILI9341 _panel;
  lgfx::Bus_SPI _bus;
  lgfx::Light_PWM _light;
  lgfx::Touch_FT5x06 _touch;  // FT5x06 driver also handles FT6x36 family

 public:
  LGFX() {
    {  // SPI bus — HSPI / SPI3_HOST per Freenove's USE_HSPI_PORT
      auto cfg = _bus.config();
      cfg.spi_host = SPI3_HOST;
      cfg.spi_mode = 0;
      cfg.freq_write = SPI_WRITE_HZ;
      cfg.freq_read = SPI_WRITE_HZ / 2;
      cfg.spi_3wire = false;
      cfg.use_lock = true;
      cfg.dma_channel = SPI_DMA_CH_AUTO;
      cfg.pin_sclk = PIN_LCD_SCLK;
      cfg.pin_mosi = PIN_LCD_MOSI;
      cfg.pin_miso = PIN_LCD_MISO;
      cfg.pin_dc = PIN_LCD_DC;
      _bus.config(cfg);
      _panel.setBus(&_bus);
    }
    {  // Panel — dimensions MUST be the native portrait size of the
      // physical display (240×320). LovyanGFX swaps width/height
      // itself when you call setRotation() with a 90/270° rotation.
      auto cfg = _panel.config();
      cfg.pin_cs = PIN_LCD_CS;
      cfg.pin_rst = PIN_LCD_RST;
      cfg.pin_busy = -1;
      cfg.panel_width = PANEL_NATIVE_WIDTH;
      cfg.panel_height = PANEL_NATIVE_HEIGHT;
      cfg.offset_x = PANEL_X_OFFSET;
      cfg.offset_y = PANEL_Y_OFFSET;
      cfg.offset_rotation = 0;
      cfg.dummy_read_pixel = 8;
      cfg.dummy_read_bits = 1;
      cfg.readable = false;
      cfg.invert = INVERT_COLORS;
      cfg.rgb_order = !BGR_ORDER;  // LovyanGFX: false = BGR
      cfg.dlen_16bit = false;
      cfg.bus_shared = false;
      _panel.config(cfg);
    }
    {  // Backlight
      auto cfg = _light.config();
      cfg.pin_bl = PIN_LCD_BL;
      cfg.invert = !BL_ACTIVE_HIGH;  // Active-HIGH on this board -> invert = false
      cfg.freq = 12000;
      cfg.pwm_channel = 7;
      _light.config(cfg);
      _panel.setLight(&_light);
    }
    {  // Capacitive touch — FT6336U on I²C bus 0.
      auto cfg = _touch.config();
      cfg.x_min = 0;
      cfg.x_max = PANEL_NATIVE_WIDTH - 1;
      cfg.y_min = 0;
      cfg.y_max = PANEL_NATIVE_HEIGHT - 1;
      cfg.pin_int = PIN_TOUCH_INT;
      cfg.pin_rst = PIN_TOUCH_RST;
      cfg.bus_shared = false;
      cfg.offset_rotation = 0;
      cfg.i2c_port = 0;
      cfg.i2c_addr = TOUCH_I2C_ADDR;
      cfg.pin_sda = PIN_TOUCH_SDA;
      cfg.pin_scl = PIN_TOUCH_SCL;
      cfg.freq = TOUCH_I2C_HZ;
      _touch.config(cfg);
      _panel.setTouch(&_touch);
    }
    setPanel(&_panel);
  }
};

#else
#error "display.h: no known display configuration for this board"
#endif