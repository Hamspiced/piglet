// Piglet — Waveshare ESP32-C5-Touch-LCD-3.5 board adapter.
//
// Bridges the upstream T-Dongle sketch (Adafruit_ST7735 + ST77XX colours) onto
// this board's hardware without rewriting the application:
//
//   * Display: a TFT_eSPI (ST7796, C5-patched) subclass that answers to the few
//     ST7735-specific calls the sketch makes (initR / setColRowStartPublic) so
//     the ~400 tft.* draw calls compile and run unchanged. The real panel setup
//     (driver, pins, inversion) lives in wsc5_user_setup.h.
//   * Colours: the sketch's COLOR_*/BLACK/WHITE macros remapped to TFT_eSPI's
//     RGB565 names.
//   * Board power/reset/touch/GPS come from the BSP (WsC5_35::*), pinned to the
//     ESP32-C5-BSP Git dependency @ 920a619.
//
// Upstream Piglet is CC BY-NC-SA 4.0; the BSP is MIT. This adapter is part of
// the Piglet derivative port.
#pragma once

#include <TFT_eSPI.h>
#include <Wire.h>
#include <board/ws_c5_35/ws_c5_35_board.h>
#include <board/ws_c5_35/ws_c5_35_pins.h>
#include <board/ws_c5_35/ws_c5_35_expander.h>
#include <board/ws_c5_35/ws_c5_35_gps.h>
#include <chip/esp32c5_i2c_bus.h>

// LC76G GPS presence probe on the shared I2C bus (command addr 0x50). Call after
// WsC5_35::begin() has opened the bus. If the module is absent, the sketch skips
// the GPS transport entirely instead of churning the bus with failed reads.
inline bool wsc5GpsPresent() {
  Esp32C5::I2cBus::Guard g(50);   // serialise with touch while we probe
  Wire.beginTransmission((uint8_t)0x50);
  return Wire.endTransmission() == 0;
}

// FT6336 INT line on this board (gates touch reads to kill phantom points).
#ifndef WSC5_TP_INT_GPIO
#define WSC5_TP_INT_GPIO 3
#endif

// Drop-in for the sketch's Adafruit_ST7735Ex: a TFT_eSPI panel that also answers
// the two ST7735-only calls the sketch makes. The (cs,dc,rst) ctor args are
// accepted and ignored — TFT_eSPI takes its pins from wsc5_user_setup.h.
class DisplayC5 : public TFT_eSPI {
public:
  DisplayC5(int /*cs*/ = -1, int /*dc*/ = -1, int /*rst*/ = -1) : TFT_eSPI() {}
  // ST7735 panel-window offset: not applicable to the ST7796 — no-op.
  void setColRowStartPublic(int /*col*/, int /*row*/) {}
  // ST7735 initR(tabColour) maps to TFT_eSPI's init(); the sketch applies
  // inversion/rotation right after, same as before.
  void initR(uint8_t /*options*/) { init(); }
};

// ---- Colours: sketch's ST77XX names -> TFT_eSPI RGB565 -----------------------
#define COLOR_RED    TFT_RED
#define COLOR_GREEN  TFT_GREEN
#define COLOR_BLUE   TFT_BLUE
#define COLOR_CYAN   TFT_CYAN
#define COLOR_YELLOW TFT_YELLOW
#ifndef BLACK
  #define BLACK TFT_BLACK
#endif
#ifndef WHITE
  #define WHITE TFT_WHITE
#endif
