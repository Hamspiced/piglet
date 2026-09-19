#pragma once
#include <Arduino.h>

struct PinMap {
  int sda, scl;  // OLED
  int sd_cs, sd_sck, sd_miso, sd_mosi; // SD
  int gps_rx, gps_tx; // GPS
  int btn; // Button
  const char* name;  // "S3" / "C6" / etc
};

// --- S3 default pins (your original) ---
static const PinMap PINS_S3 = {
  5, 6,
  4, 7, 8, 9,
  44, 43,
  2,
  "S3"
};

// --- C6 default pins ---
static const PinMap PINS_C6 = {
  22, 23,
  21, 19, 20, 18,
  17, 16,
  1,
  "C6"
};

// --- C5 default pins ---
static const PinMap PINS_C5 = {
  23, 24,
  7, 8, 9, 10,
  12, 11,
  0,
  "C5"
};

// --- S3 + Expansion Base ---
static const PinMap PINS_S3_EXP_BASE = {
  5, 6,
  3, 7, 8, 10,
  12, 11,
  2,
  "EXP_BASE"
};

// --- XIAO ESP32-C3 ---
// Headless (no built-in OLED), 2.4 GHz only.
// SDA/SCL (D4/D5) are exposed and can drive an external OLED if attached.
// SD on SPI2 (D8/D9/D10); GPS on UART1 (D7=RX, D6=TX).
// btn=-1: GPIO9 (D9) conflicts with SPI MISO so no dedicated button pin.
static const PinMap PINS_XIAO_C3 = {
  6,  7,           // SDA (D4/GPIO6), SCL (D5/GPIO7) — OLED if attached
  2,  8, 9,  10,  // SD:  cs(D0), sck(D8), miso(D9), mosi(D10)
  20, 21,          // GPS: rx(D7/GPIO20 from GPS TX), tx(D6/GPIO21 to GPS RX)
  -1,              // BTN: none (GPIO9=SPI MISO conflict; wire external if needed)
  "C3"
};
