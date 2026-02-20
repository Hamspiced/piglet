#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WebServer.h>
#include <SD.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <TinyGPSPlus.h>
#include "PinMapDefs.h"
#include "Config.h"

// OLED display dimensions
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64

// ---- Global objects ----
extern PinMap pins;
extern Config cfg;
extern Adafruit_SSD1306 display;
extern TinyGPSPlus gps;
extern HardwareSerial GPSSerial;
extern WebServer server;

// ---- State flags ----
extern bool sdOk;
extern bool scanningEnabled;
extern bool gpsHasFix;
extern bool allowScanForOled;
extern bool userScanOverride;
extern bool autoPaused;

// ---- OLED page system ----
// 0=Status, 1=Networks, 2=Navigation, 3=Pause, 4=Pig
static const uint8_t PAGE_COUNT = 5;
extern uint8_t currentPage;
extern bool statusPagePaused;   // true when user double-pressed to pause on status page

// ---- AP state ----
extern uint32_t apStartMs;
extern bool apClientSeen;
extern bool apWindowActive;
extern const uint32_t AP_WINDOW_MS;

// ---- Counters ----
extern uint32_t networksFound2G;
extern uint32_t networksFound5G;

// ---- Log state ----
extern File logFile;
extern String currentCsvPath;
extern wl_status_t lastStaStatus;

// ---- Upload state ----
extern bool     uploading;
extern bool     uploadPausedScanWasEnabled;
extern uint32_t uploadTotalFiles;
extern uint32_t uploadDoneFiles;
extern String   uploadCurrentFile;
extern String   uploadLastResult;
extern int      wigleTokenStatus;
extern int      wigleLastHttpCode;

// ---- WiGLE constants ----
extern const char* WIGLE_HOST;
extern const uint16_t WIGLE_PORT;

// ---- Optional TLS buffer sizing (no-op for broad core compatibility) ----
inline void tlsMaybeSetBufferSizes(WiFiClientSecure&, int, int) {}
