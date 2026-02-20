#pragma once
#include <Arduino.h>
#include <Adafruit_SSD1306.h>

// OLED layout constants
static const int OLED_YELLOW_H = 16; // typical 2-color modules
static const int OLED_W = 128;
static const int OLED_H = 64;

static inline bool isTwoColor() { return true; } // set false if you swap back

// ---- Pig walk animation ----
struct PigAnim {
  int16_t x = 0;
  int16_t y = 36;
  int8_t  dx = 1;
  uint8_t phase = 0;
  uint32_t lastMs = 0;
  uint16_t frameMs = 90;
};
extern PigAnim pig;

void oledProgressBar(int x, int y, int w, int h, float pct);
void updateOLED(float speedValue);
void pigAnimTick();  // call from loop() when on pig page (faster than 500ms)
void showSplashScreen();
