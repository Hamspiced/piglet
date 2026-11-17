#pragma once
#include <Arduino.h>
#include <Adafruit_SSD1306.h>

// OLED layout constants
static const int OLED_YELLOW_H = 16; // typical 2-color modules
static const int OLED_W = 128;
static const int OLED_H = 64;

static inline bool isTwoColor() { return true; } // set false if you swap back

void oledProgressBar(int x, int y, int w, int h, float pct);
void updateOLED(float speedValue);
void pigAnimTick();    // call from loop() when on pig page (PigWorld, 20 fps)
void sasquatchStart(); // triple-tap on pig page
void showSplashScreen();
