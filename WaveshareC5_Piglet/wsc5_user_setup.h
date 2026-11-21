// SPDX-License-Identifier: MIT
// TFT_eSPI user setup for the Waveshare ESP32-C5-Touch-LCD-3.5
// (ST7796 320x480). Force-included by this example's platformio.ini.
//
// The display SPI bus (SCLK 6 / MOSI 7 / MISO 2) is shared with the microSD
// card. Touch is an FT6336 on I2C (SDA 27 / SCL 26), driven by the BSP, NOT by
// TFT_eSPI's resistive-touch path -- so there is no TOUCH_CS here.
//
// The panel's reset and backlight are NOT GPIOs: they live on the 0x24 I/O
// expander, brought up by WsC5_35::begin() before tft.init(). Hence
// TFT_RST -1 and no TFT_BL. Pin values mirror src/board/ws_c5_35/ws_c5_35_pins.h.
#pragma once

#define USER_SETUP_INFO "ESP32-C5-BSP / Waveshare C5 3.5 / ST7796 / FT6336"
#define ST7796_DRIVER

// Portrait native (320 wide x 480 tall); rotate at runtime via setRotation().
#define TFT_WIDTH  320
#define TFT_HEIGHT 480

#define TFT_MISO 2
#define TFT_MOSI 7
#define TFT_SCLK 6
#define TFT_CS   8
#define TFT_DC   5
#define TFT_RST  -1   // reset is on the expander (EXIO1)
// No TFT_BL: the backlight is the expander's PWM (EXIO5), not a GPIO.

#define LOAD_GLCD
#define LOAD_FONT2

// ST7796 on this board wants inversion on (Waveshare's init sends 0x21). The
// sketch is the live control point via tft.invertDisplay(); this define is kept
// because TFT_eSPI expects one of the two inversion defines present.
#define TFT_INVERSION_ON
#define TFT_RGB_ORDER TFT_BGR

// TFT_eSPI's init-time write clock. 60 MHz is this panel's validated ceiling.
#ifndef SPI_FREQUENCY
#define SPI_FREQUENCY      60000000
#endif
#define SPI_READ_FREQUENCY 20000000
