#pragma once
#include <Arduino.h>

// OLED layout constants
static const int OLED_YELLOW_H = 16; // typical 2-color modules
static const int OLED_W = 128;
static const int OLED_H = 64;

static inline bool isTwoColor() { return true; } // set false if you swap back

void oledProgressBar(int x, int y, int w, int h, float pct);
void updateOLED(float speedValue);
void showSplashScreen();
