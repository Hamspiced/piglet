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