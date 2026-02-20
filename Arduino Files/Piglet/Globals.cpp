#include "Globals.h"
#include <Wire.h>

// ---- Global objects ----
PinMap pins = PINS_C6; // default until detected/configured
Config cfg;
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
TinyGPSPlus gps;
HardwareSerial GPSSerial(1);
WebServer server(80);

// ---- State flags ----
bool sdOk = false;
bool scanningEnabled = true;
bool gpsHasFix = false;
bool allowScanForOled = false;
bool userScanOverride = false;
bool autoPaused = false;

// ---- OLED page system ----
uint8_t currentPage = 0;
bool statusPagePaused = false;

// ---- AP state ----
uint32_t apStartMs = 0;
bool apClientSeen = false;
bool apWindowActive = false;
const uint32_t AP_WINDOW_MS = 60000UL;

// ---- Counters ----
uint32_t networksFound2G = 0;
uint32_t networksFound5G = 0;

// ---- Log state ----
File logFile;
String currentCsvPath;
wl_status_t lastStaStatus = WL_IDLE_STATUS;

// ---- Upload state ----
bool     uploading = false;
bool     uploadPausedScanWasEnabled = false;
uint32_t uploadTotalFiles = 0;
uint32_t uploadDoneFiles  = 0;
String   uploadCurrentFile = "";
String   uploadLastResult  = "";
int      wigleTokenStatus  = 0;
int      wigleLastHttpCode = 0;

// ---- WiGLE constants ----
const char* WIGLE_HOST = "api.wigle.net";
const uint16_t WIGLE_PORT = 443;
