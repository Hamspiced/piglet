/*
 * LilyGo T-Dongle C5 Piglet Wardriver
 * (WiGLE CSV + GPS + SD + TFT + Web UI + APA102 LED)
 *
 * STANDALONE PORT — single-file sketch for the T-Dongle C5 board.
 * ESP32-C5 with dual-band Wi-Fi 6 (2.4 + 5 GHz), ST7735 0.96" TFT, TF card, APA102 LED.
 *
 * Pin Map (T-Dongle C5):
 *   LCD (ST7735, SPI):
 *     MOSI = GPIO2, SCK = GPIO6, CS = GPIO10, DC = GPIO3, RST = GPIO1, BL = GPIO0
 *   SD / TF (shared SPI):
 *     CS = GPIO23, SCK = GPIO6, MISO = GPIO7, MOSI = GPIO2
 *   APA102 LED (shared SPI):
 *     DI = GPIO2, CI = GPIO7  (bit-bang when LCD+SD deselected)
 *   GPS (UART via Qwiic/JST connector):
 *     RX = GPIO12, TX = GPIO11
 *   Button:
 *     BOOT = GPIO9 (INPUT_PULLUP, strapping pin — safe to use as GPIO after boot)
 *
 * License: CC BY-NC-SA 4.0
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <SPI.h>
#include <SD.h>
#include <sys/time.h>
#include <ArduinoJson.h>
#include <TinyGPSPlus.h>

#include <stdint.h>
#include "esp_netif.h"
#include <vector>
#include <unordered_set>
#include <deque>
#include <time.h>
#include <math.h>
#include <esp_now.h>
#include "esp_wifi.h"
#include "driver/gpio.h"
#include "soc/gpio_sig_map.h"
#include "esp32-hal-matrix.h"

// Firmware version
#define FIRMWARE_VERSION "v2.53"

// ---------------- Pins (T-DONGLE C5) ----------------
struct PinMap {
  // TFT (SPI)
  int tft_sck, tft_mosi, tft_cs, tft_dc, tft_rst, tft_bl;
  // SD (SPI - shared bus)
  int sd_cs, sd_sck, sd_miso, sd_mosi;
  // APA102 LED (bit-bang)
  int led_di, led_ci;
  // GPS (UART via Qwiic)
  int gps_rx, gps_tx;
  // User button
  int btn;
};

static const PinMap PINS = {
  // TFT
  6,   // tft_sck
  2,   // tft_mosi
  10,  // tft_cs
  3,   // tft_dc
  1,   // tft_rst
  0,   // tft_bl
  // SD (shares SCK/MOSI with TFT)
  23,  // sd_cs
  6,   // sd_sck
  7,   // sd_miso
  2,   // sd_mosi
  // APA102 LED
  2,   // led_di  (shared with MOSI)
  7,   // led_ci  (shared with MISO)
  // GPS (Qwiic/JST connector)
  12,  // gps_rx
  11,  // gps_tx
  // BTN (user button on GPIO28 — confirmed by board hardware)
  28
};

// ---------------- TFT (Adafruit ST7735 — works on ESP32-C5) ----------------
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>

// Subclass to expose protected setColRowStart (became protected in library v1.10+)
class Adafruit_ST7735Ex : public Adafruit_ST7735 {
public:
  Adafruit_ST7735Ex(int8_t cs, int8_t dc, int8_t rst) : Adafruit_ST7735(cs, dc, rst) {}
  void setColRowStartPublic(int8_t col, int8_t row) { setColRowStart(col, row); }
};

// Use the hardware SPI constructor with CS, DC, RST
static Adafruit_ST7735Ex tft(PINS.tft_cs, PINS.tft_dc, PINS.tft_rst);

// RGB565 colors
#define COLOR_RED    ST77XX_RED
#define COLOR_GREEN  ST77XX_GREEN
#define COLOR_BLUE   ST77XX_BLUE
#define COLOR_CYAN   ST77XX_CYAN
#define COLOR_YELLOW ST77XX_YELLOW
#ifndef BLACK
  #define BLACK ST77XX_BLACK
#endif
#ifndef WHITE
  #define WHITE ST77XX_WHITE
#endif

// Adafruit GFX doesn't have textWidth() — calculate from text size
static int tftTextWidth(const char* txt, int textSize = 1) {
  if (!txt) return 0;
  return strlen(txt) * 6 * textSize;  // 6px per char at size 1
}

// ---------------- GPS ----------------
TinyGPSPlus gps;
HardwareSerial GPSSerial(1);

// ---------------- Web ----------------
WebServer server(80);

// ---------------- Config ----------------
struct Config {
  String wigleBasicToken;
  String homeSsid;
  String homePsk;
  String wardriverSsid = "Piglet-WARDRIVE";
  String wardriverPsk  = "wardrive1234";
  uint32_t gpsBaud     = 9600;
  String scanMode      = "aggressive";
  String speedUnits    = "kmh";
  // WDGoWars API key from https://wdgwars.pl/profile
  String wdgwarsApiKey;
  // Boot auto-upload limit: -1=all, 0=disabled, 1+=capped
  int maxBootUploads   = 25;
  // Optional device name shown in WiGLE header and filename
  String deviceName;
  // Auto-start mesh role on boot: "core" | "node" | "none"
  String meshModeOnBoot = "none";
  // Rotate TFT screen 180° (true = upside-down mount). Requires reboot.
  bool rotateScreen180 = false;
};

Config cfg;
static const char* bootSlogan = nullptr;

// ---------------- State ----------------
static bool sdOk = false;
static bool scanningEnabled = true;
static bool gpsHasFix = false;

// UI state
static int   uiGpsSats    = 0;
static float uiHeadingDeg = 0.0f;
static String uiHeadingTxt = "—";
static bool uiFirstDraw = true;

// Diff-repaint cache
static bool     prevAllowScan = false;
static bool     prevSdOk = false;
static bool     prevGpsFix = false;
static bool     prevSta = false;
static uint32_t prevFound2G = 0;
static uint32_t prevFound5G = 0;
static float    prevSpeed = -999;
static String   prevIp = "";
static String   prevUploadMsg = "";
static uint32_t prevUploadDone = 0;
static uint32_t prevUploadTotal = 0;

// Scan control
static bool userScanOverride = false;
static bool autoPaused = false;

// AP window
static uint32_t apStartMs = 0;
static bool apClientSeen = false;
static bool apWindowActive = false;
static const uint32_t AP_WINDOW_MS = 60000UL;

// Extended AP window — 5 min rolling timer, driven by the WebUI keep-alive modal
static bool     apExtended           = false;
static uint32_t apExtendedStartMs    = 0;
static bool     apForceClose         = false;
static const uint32_t AP_EXTENDED_WINDOW_MS    = 5UL * 60UL * 1000UL; // 5 minutes
static const uint32_t AP_EXTEND_PROMPT_LEAD_MS = 30000UL;              // prompt 30 s before expiry

// Counters
static uint32_t networksFound2G = 0;
static uint32_t networksFound5G = 0;

// Log state
static File logFile;
static String currentCsvPath;
static wl_status_t lastStaStatus = WL_IDLE_STATUS;

// ---------------- Upload State ----------------
static bool     uploading = false;
static bool     uploadPausedScanWasEnabled = false;
static uint32_t uploadTotalFiles = 0;
static uint32_t uploadDoneFiles  = 0;
static uint32_t uploadFailedFiles = 0;
static String   uploadCurrentFile = "";
static String   uploadLastResult  = "";
static String   uploadTargetName  = "";  // shown on TFT during upload
static int      wigleTokenStatus  = 0;
static int      wigleLastHttpCode = 0;

static const char* WDGWARS_HOST = "wdgwars.pl";
static const uint16_t WDGWARS_PORT = 443;

static const char* WIGLE_HOST = "api.wigle.net";
static const uint16_t WIGLE_PORT = 443;

// ---------------- Screen pages ----------------
// Pages: 0=status  1=networks  2=nav  3=pig  4=mesh-node
struct PigState_t {
  int16_t  x       = 0;
  int16_t  y       = 82;   // fixed vertical position in body area
  int8_t   dx      = 1;
  uint8_t  phase   = 0;
  uint32_t lastMs  = 0;
  uint16_t frameMs = 90;
};
static PigState_t    pig;
static uint8_t       currentPage = 0;
static const uint8_t PAGE_COUNT  = 5;
// Set true on page entry so the draw function does a full initial layout.
static bool pageNeedsInit[PAGE_COUNT] = { false, true, true, true, true };

// Navigation page — hold last good heading for HEADING_HOLD_MS after fix is lost
static double        lastGoodHeadingDeg = NAN;
static uint32_t      lastGoodHeadingMs  = 0;
static const uint32_t HEADING_HOLD_MS        = 5000;
static const float    HEADING_MIN_SPEED_KMPH = 3.0f;

// ---------------- APA102 LED (bit-bang) ----------------
// Drives the APA102 RGB LED by bit-banging when LCD+SD are deselected.
// APA102 protocol: 4 bytes start frame (0x00), then 0xE0|brightness, B, G, R, then end frame.

static void apa102Write(uint8_t r, uint8_t g, uint8_t b, uint8_t brightness = 8) {
  // Deselect LCD and SD first
  digitalWrite(PINS.tft_cs, HIGH);
  digitalWrite(PINS.sd_cs, HIGH);

  // Temporarily switch data/clock pins from SPI to GPIO output mode for
  // bit-banging. Using the IDF gpio_set_direction() instead of Arduino's
  // pinMode() avoids triggering the peripheral manager, which would
  // permanently detach these pins from the SPI bus (because SPI.begin()
  // is a no-op when the bus is already started, so pins never get
  // re-attached). The SPI peripheral and SD card remain intact.
  gpio_set_direction((gpio_num_t)PINS.led_di, GPIO_MODE_OUTPUT);
  gpio_set_direction((gpio_num_t)PINS.led_ci, GPIO_MODE_OUTPUT);

  auto sendByte = [](uint8_t val) {
    for (int i = 7; i >= 0; i--) {
      gpio_set_level((gpio_num_t)PINS.led_di, (val >> i) & 1);
      gpio_set_level((gpio_num_t)PINS.led_ci, 1);
      delayMicroseconds(1);
      gpio_set_level((gpio_num_t)PINS.led_ci, 0);
      delayMicroseconds(1);
    }
  };

  // Start frame: 32 bits of 0
  for (int i = 0; i < 4; i++) sendByte(0x00);

  // LED frame: 111xxxxx brightness, blue, green, red
  if (brightness > 31) brightness = 31;
  sendByte(0xE0 | brightness);
  sendByte(b);
  sendByte(g);
  sendByte(r);

  // End frame: 32 bits of 1
  for (int i = 0; i < 4; i++) sendByte(0xFF);

  // Restore SPI peripheral routing — reconnect the SPI MOSI and MISO
  // signals to the physical pins via the GPIO matrix. pinMatrixOutAttach
  // and pinMatrixInAttach are low-level wrappers around gpio_matrix_out/in
  // that bypass the peripheral manager.
  pinMatrixOutAttach(PINS.sd_mosi, FSPID_IN_IDX, false, false);
  gpio_set_direction((gpio_num_t)PINS.sd_miso, GPIO_MODE_INPUT);
  pinMatrixInAttach(PINS.sd_miso, FSPIQ_OUT_IDX, false);
}

static void ledOff()   { apa102Write(0, 0, 0, 0); }
static void ledGreen() { apa102Write(0, 10, 0, 2); }
static void ledRed()   { apa102Write(10, 0, 0, 2); }
static void ledBlue()  { apa102Write(0, 0, 10, 2); }

// Pulse state for non-blocking LED blink
static uint32_t ledPulseMs = 0;
static void ledPulseGreen() { ledGreen(); ledPulseMs = millis(); }

static void ledTick() {
  if (ledPulseMs && (millis() - ledPulseMs > 80)) {
    ledOff();
    ledPulseMs = 0;
  }
}

// ---------------- Helpers ----------------
static String pathBasename(const String& p) {
  int slash = p.lastIndexOf('/');
  if (slash < 0) return p;
  return p.substring(slash + 1);
}

static String authModeToString(wifi_auth_mode_t m) {
  switch (m) {
    case WIFI_AUTH_OPEN:            return "OPEN";
    case WIFI_AUTH_WEP:             return "WEP";
    case WIFI_AUTH_WPA_PSK:         return "WPA";
    case WIFI_AUTH_WPA2_PSK:        return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK:    return "WPAWPA2";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2EAP";
    case WIFI_AUTH_WPA3_PSK:        return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK:   return "WPA2WPA3";
    default: return "UNKNOWN";
  }
}

static time_t makeUtcEpochFromTm(struct tm* t) {
  const char* oldTz = getenv("TZ");
  setenv("TZ", "UTC0", 1); tzset();
  time_t epoch = mktime(t);
  if (oldTz) setenv("TZ", oldTz, 1); else unsetenv("TZ");
  tzset();
  return epoch;
}

static String iso8601NowUTC() {
  if (gps.date.isValid() && gps.time.isValid() &&
      gps.date.age() < 5000 && gps.time.age() < 5000) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02dZ",
             gps.date.year(), gps.date.month(), gps.date.day(),
             gps.time.hour(), gps.time.minute(), gps.time.second());
    return String(buf);
  }
  uint32_t s = millis() / 1000;
  char buf[32];
  snprintf(buf, sizeof(buf), "1970-01-01T00:%02lu:%02luZ",
           (unsigned long)((s/60)%60), (unsigned long)(s%60));
  return String(buf);
}

// ---------------- Config (plain text key=value) ----------------
static const char* CFG_PATH = "/wardriver.cfg";

static bool parseKeyValueLine(const String& lineIn, String& keyOut, String& valOut) {
  String line = lineIn; line.trim();
  if (line.length() == 0 || line[0] == '#' || line.startsWith("//")) return false;
  int eq = line.indexOf('=');
  if (eq < 0) return false;
  String k = line.substring(0, eq); k.trim();
  String v = line.substring(eq + 1); v.trim();
  if (v.length() >= 2 && ((v[0] == '"' && v[v.length()-1] == '"') ||
      (v[0] == '\'' && v[v.length()-1] == '\''))) {
    v = v.substring(1, v.length()-1); v.trim();
  }
  if (k.length() == 0) return false;
  keyOut = k; valOut = v;
  return true;
}

static void cfgAssignKV(const String& k, const String& v) {
  if (k == "wigleBasicToken") cfg.wigleBasicToken = v;
  else if (k == "homeSsid")      cfg.homeSsid = v;
  else if (k == "homePsk")       cfg.homePsk = v;
  else if (k == "wardriverSsid") cfg.wardriverSsid = v;
  else if (k == "wardriverPsk")  cfg.wardriverPsk = v;
  else if (k == "gpsBaud") { uint32_t b = (uint32_t)v.toInt(); if (b > 0) cfg.gpsBaud = b; }
  else if (k == "scanMode") { if (v == "aggressive" || v == "powersaving") cfg.scanMode = v; }
  else if (k == "speedUnits") { String vv = v; vv.toLowerCase(); if (vv == "kmh" || vv == "mph") cfg.speedUnits = vv; }
  else if (k == "wdgwarsApiKey")   cfg.wdgwarsApiKey = v;
  else if (k == "maxBootUploads") { int n = v.toInt(); if (n >= -1) cfg.maxBootUploads = n; }
  else if (k == "deviceName")      cfg.deviceName = v;
  else if (k == "meshModeOnBoot") { String vv = v; vv.toLowerCase(); if (vv == "core" || vv == "node" || vv == "none") cfg.meshModeOnBoot = vv; }
  else if (k == "rotateScreen180") { String vv = v; vv.toLowerCase(); cfg.rotateScreen180 = (vv == "true" || vv == "1"); }
}

static bool saveConfigToSD() {
  if (!sdOk) return false;
  digitalWrite(PINS.tft_cs, HIGH);
  if (SD.exists(CFG_PATH)) { SD.remove(CFG_PATH); }

  File f = SD.open(CFG_PATH, FILE_WRITE);
  if (!f) return false;

  f.println("# Piglet Wardriver config [T-Dongle C5]");
  f.print("wigleBasicToken="); f.println(cfg.wigleBasicToken);
  f.print("homeSsid=");        f.println(cfg.homeSsid);
  f.print("homePsk=");         f.println(cfg.homePsk);
  f.print("wardriverSsid=");   f.println(cfg.wardriverSsid);
  f.print("wardriverPsk=");    f.println(cfg.wardriverPsk);
  f.print("gpsBaud=");         f.println(cfg.gpsBaud);
  f.print("scanMode=");        f.println(cfg.scanMode);
  f.print("speedUnits=");      f.println(cfg.speedUnits);
  f.print("wdgwarsApiKey=");   f.println(cfg.wdgwarsApiKey);
  f.println("# Boot auto-upload limit: -1=all, 0=disabled, 1+=capped");
  f.print("maxBootUploads=");  f.println(cfg.maxBootUploads);
  f.println("# Device name: shown in WiGLE header and filename (optional)");
  f.print("deviceName=");      f.println(cfg.deviceName);
  f.println("# Mesh mode on boot: core | node | none");
  f.print("meshModeOnBoot=");  f.println(cfg.meshModeOnBoot);
  f.println("# Rotate screen 180 degrees (true = upside-down mount). Requires reboot.");
  f.print("rotateScreen180="); f.println(cfg.rotateScreen180 ? "true" : "false");

  f.flush(); f.close();
  Serial.println("[CFG] Saved OK");
  return true;
}

static bool loadConfigFromSD() {
  if (!sdOk) return false;
  digitalWrite(PINS.tft_cs, HIGH);
  if (!SD.exists(CFG_PATH)) { Serial.println("[CFG] No config file."); return false; }

  File f = SD.open(CFG_PATH, FILE_READ);
  if (!f) return false;

  while (f.available()) {
    String line = f.readStringUntil('\n');
    String k, v;
    if (parseKeyValueLine(line, k, v)) cfgAssignKV(k, v);
  }
  f.close();

  Serial.println("[CFG] Loaded config:");
  Serial.print("      homeSsid: ");      Serial.println(cfg.homeSsid);
  Serial.print("      gpsBaud: ");       Serial.println(cfg.gpsBaud);
  Serial.print("      scanMode: ");      Serial.println(cfg.scanMode);
  Serial.print("      wigle token: ");   Serial.println(cfg.wigleBasicToken.length() ? "(set)" : "(empty)");
  return true;
}

// ---------------- SD Logging ----------------
static String normalizeSdPath(const char* dir, const char* nameIn) {
  if (!dir || !nameIn) return "";
  String d(dir), n(nameIn);
  d.trim(); n.trim();
  if (d.length() == 0 || n.length() == 0) return "";
  if (d[0] != '/') d = "/" + d;
  while (d.endsWith("/")) d.remove(d.length() - 1);
  if (n[0] == '/') return n;
  String dNoSlash = d;
  if (dNoSlash.startsWith("/")) dNoSlash = dNoSlash.substring(1);
  if (n.startsWith(dNoSlash + "/")) return "/" + n;
  return d + "/" + n;
}

// Sanitise device name for filename/header use
static String tdongleSanitiseName(const String& raw) {
  String s = raw; s.replace(" ", "_");
  for (int i = (int)s.length()-1; i >= 0; i--) {
    char c = s[i];
    if (!isAlphaNumeric(c) && c != '_' && c != '-') s.remove(i, 1);
  }
  if (s.length() > 20) s = s.substring(0, 20);
  return s;
}

static String newCsvFilename() {
  digitalWrite(PINS.tft_cs, HIGH);
  if (!SD.exists("/logs")) SD.mkdir("/logs");
  String prefix = "";
  if (cfg.deviceName.length() > 0) {
    String safe = tdongleSanitiseName(cfg.deviceName);
    if (safe.length() > 0) prefix = safe + "_Piglet-TDongle_";
  }
  for (int tries = 0; tries < 25; tries++) {
    char buf[100];
    snprintf(buf, sizeof(buf), "/logs/%sWiGLE_%lu_%08lX.csv",
             prefix.c_str(), (unsigned long)millis(), (unsigned long)(uint32_t)esp_random());
    String p(buf);
    if (!SD.exists(p)) return p;
  }
  char buf2[100];
  snprintf(buf2, sizeof(buf2), "/logs/%sWiGLE_%lu.csv",
           prefix.c_str(), (unsigned long)millis());
  return String(buf2);
}

static void closeLogFile() {
  if (logFile) {
    digitalWrite(PINS.tft_cs, HIGH);
    logFile.flush();
    logFile.close();
  }
}

static bool openLogFile() {
  if (!sdOk) return false;
  closeLogFile();
  currentCsvPath = newCsvFilename();
  Serial.print("[SD] Opening log: "); Serial.println(currentCsvPath);

  digitalWrite(PINS.tft_cs, HIGH);
  logFile = SD.open(currentCsvPath, FILE_WRITE);
  if (!logFile) return false;

  String deviceField = "Piglet-Wardriver";
  if (cfg.deviceName.length() > 0) {
    String safe = tdongleSanitiseName(cfg.deviceName);
    if (safe.length() > 0) deviceField = "Piglet-TDongle-" + safe;
  }
  // WiGLE WiFi 1.6 header
  logFile.print("WigleWifi-1.6,appRelease=");
  logFile.print(FIRMWARE_VERSION);
  logFile.print(",model=LilyGo-T-Dongle-C5,release=1,device=");
  logFile.print(deviceField);
  logFile.println(",board=LilyGo-T-Dongle-C5,brand=Piglet");
  logFile.println("MAC,SSID,AuthMode,FirstSeen,Channel,Frequency,RSSI,CurrentLatitude,CurrentLongitude,AltitudeMeters,AccuracyMeters,RCOIs,MfgrId,Type");
  logFile.flush();
  return true;
}

// ---- Log-once dedupe (matches the upstream JCMK/Biscuit mac_history[200]) ----
// Self-contained for this single-file sketch (no shared header). Each BSSID is
// accepted once until it is evicted past the cap (FIFO ring), so a given network
// is written / forwarded only once per session.
struct WifiSeenRing {
  explicit WifiSeenRing(size_t cap = 200) : cap_(cap ? cap : 1) {}
  bool firstTime(const String& mac) {
    if (mac.length() < 17) return true;          // unparseable -> don't suppress
    uint64_t key = 0;
    for (int i = 0; i < 6; i++)
      key |= (uint64_t)strtoul(mac.c_str() + i * 3, nullptr, 16) << (i * 8);
    if (seen_.count(key)) return false;
    seen_.insert(key);
    order_.push_back(key);
    while (seen_.size() > cap_ && !order_.empty()) {
      seen_.erase(order_.front());
      order_.pop_front();
    }
    return true;
  }
 private:
  std::unordered_set<uint64_t> seen_;
  std::deque<uint64_t> order_;
  size_t cap_;
};

static void appendWigleRow(const String& mac, const String& ssid, const String& auth,
                           const String& firstSeen, int channel, int rssi,
                           double lat, double lon, double altM, double accM) {
  if (!sdOk || !logFile) return;

  // Log each BSSID once (covers solo + Core node-forwarded writes via one ring).
  static WifiSeenRing gWigleSeen;
  if (!gWigleSeen.firstTime(mac)) return;

  String safeSsid = ssid;
  safeSsid.replace("\"", "\"\"");

  String line;
  line.reserve(256);
  line += mac; line += ",";
  line += "\""; line += safeSsid; line += "\",";
  line += auth; line += ",";
  line += firstSeen; line += ",";
  line += String(channel); line += ",";
  // Frequency in MHz (WiGLE 1.6)
  uint32_t freq = 0;
  if      (channel >= 1  && channel <= 13) freq = 2407u + (uint32_t)channel * 5;
  else if (channel == 14)                  freq = 2484u;
  else if (channel >= 32)                  freq = 5000u + (uint32_t)channel * 5;
  line += String(freq); line += ",";
  line += String(rssi); line += ",";
  line += String(lat, 6); line += ",";
  line += String(lon, 6); line += ",";
  line += String(altM, 1); line += ",";
  line += String(accM, 1); line += ",";
  line += ",0,WIFI"; // RCOIs (empty), MfgrId (0), Type

  digitalWrite(PINS.tft_cs, HIGH);
  size_t written = logFile.println(line);

  // Detect silent write failure — if println() returns 0 for a non-empty line,
  // the SD card or file handle is broken. Attempt to reopen the log file once;
  // if that also fails, mark SD as unusable until next boot.
  if (written == 0 && line.length() > 0) {
    static uint8_t consecFails = 0;
    consecFails++;
    Serial.printf("[SD] Write failed (%u consecutive)\n", consecFails);
    if (consecFails >= 3) {
      Serial.println("[SD] Attempting log reopen...");
      closeLogFile();
      if (openLogFile()) {
        Serial.println("[SD] Reopen OK — retrying write");
        logFile.println(line);  // best-effort retry
        consecFails = 0;
      } else {
        Serial.println("[SD] Reopen FAILED — SD marked unusable");
        sdOk = false;
      }
    }
    return;
  }

  static uint32_t lastFlushMs = 0;
  static uint32_t linesSinceFlush = 0;
  linesSinceFlush++;

  if (linesSinceFlush >= 25 || (millis() - lastFlushMs) >= 2000) {
    digitalWrite(PINS.tft_cs, HIGH);
    logFile.flush();
    lastFlushMs = millis();
    linesSinceFlush = 0;
  }
}

// Forward declarations needed by WDGoWars and WiGLE batch functions
static void tftWigleUploadScreen(uint32_t done, uint32_t total, const String& filename);
static void forceStatusFullRedraw();

// ---- WDGoWars API key test — GET /api/me ----
static bool wdgwarsTestKey() {
  uploadLastResult = "";
  if (WiFi.status() != WL_CONNECTED) { uploadLastResult = "No STA WiFi"; return false; }
  if (cfg.wdgwarsApiKey.length() < 8) { uploadLastResult = "No API key set"; return false; }

  WiFiClientSecure client;
  client.setInsecure(); client.setTimeout(15000);
  if (!client.connect(WDGWARS_HOST, WDGWARS_PORT)) { uploadLastResult = "TLS connect fail"; return false; }

  client.print("GET /api/me HTTP/1.0\r\n");
  client.print(String("Host: ") + WDGWARS_HOST + "\r\n");
  client.print(String("X-API-Key: ") + cfg.wdgwarsApiKey + "\r\n");
  client.print("Connection: close\r\n\r\n");

  uint32_t ws = millis();
  while (!client.available() && client.connected() && (millis()-ws)<10000) { delay(10); yield(); }

  String status = client.readStringUntil('\n'); status.trim();
  int code = 0;
  if (status.startsWith("HTTP/")) {
    int s1 = status.indexOf(' ');
    if (s1>0) { int s2=status.indexOf(' ',s1+1); if(s2>s1) code=status.substring(s1+1,s2).toInt(); }
  }

  // Collect body to extract username
  String body = "";
  bool inBody = false;
  while (client.connected() || client.available()) {
    String line = client.readStringUntil('\n'); line.trim();
    if (!inBody) { if (line.length()==0) inBody=true; }
    else { body+=line; if(body.length()>512) break; }
  }
  client.stop();

  if (code == 200) {
    String user = "";
    int ui = body.indexOf("\"username\":");
    if (ui>=0) { int q1=body.indexOf('"',ui+11); int q2=(q1>=0)?body.indexOf('"',q1+1):-1; if(q1>=0&&q2>q1) user=body.substring(q1+1,q2); }
    uploadLastResult = user.length() ? "Key valid — "+user : "Key valid (200)";
    return true;
  }
  if (code==401||code==403) { uploadLastResult="Key invalid ("+String(code)+")"; }
  else { uploadLastResult="Unexpected response ("+String(code)+")"; }
  return false;
}

// ---- WDGoWars single-file upload — POST /api/upload-csv ----
static bool uploadFileToWdgwars(const String& path) {
  if (WiFi.status() != WL_CONNECTED) return false;
  if (cfg.wdgwarsApiKey.length() < 8) return false;
  digitalWrite(PINS.tft_cs, HIGH);
  if (!SD.exists(path)) return false;

  File f = SD.open(path, FILE_READ);
  if (!f) return false;

  String boundary = "----Piglet-WDGWARS-BOUNDARY";
  String filename = pathBasename(path);
  String pre = "--"+boundary+"\r\nContent-Disposition: form-data; name=\"file\"; filename=\""+filename+"\"\r\nContent-Type: text/csv\r\n\r\n";
  String post = "\r\n--"+boundary+"--\r\n";
  uint32_t contentLen = (uint32_t)pre.length()+(uint32_t)f.size()+(uint32_t)post.length();

  WiFiClientSecure client;
  client.setInsecure(); client.setTimeout(25000);
  bool connected = false;
  for (int att=1; att<=3; att++) {
    if (client.connect(WDGWARS_HOST, WDGWARS_PORT)) { connected=true; break; }
    client.stop(); delay(500); yield();
  }
  if (!connected) { uploadLastResult = "WDGW: TLS connect fail"; f.close(); return false; }

  client.print("POST /api/v2/upload-csv HTTP/1.0\r\n");
  client.print(String("Host: ")+WDGWARS_HOST+"\r\n");
  client.print(String("X-API-Key: ")+cfg.wdgwarsApiKey+"\r\n");
  client.print(String("Content-Type: multipart/form-data; boundary=")+boundary+"\r\n");
  client.print(String("Content-Length: ")+String(contentLen)+"\r\nConnection: close\r\n\r\n");
  client.print(pre);
  uint32_t fileSent = 0; bool wErr = false;
  uint8_t buf[512];
  while (!wErr) {
    int n = f.read(buf, sizeof(buf)); if (n <= 0) break;
    int off = 0;
    while (off < n) { int w = client.write(buf+off, n-off); if (w<=0){wErr=true;break;} off+=w; fileSent+=w; }
    yield();
  }
  f.close();
  client.print(post); client.flush();
  uint32_t totalSent = (uint32_t)pre.length() + fileSent + (uint32_t)post.length();
  Serial.printf("[WDGWars] ContentLen=%u sent=%u (%s)\n", contentLen, totalSent,
                (totalSent==contentLen)?"MATCH":"*** MISMATCH ***");
  if (wErr || totalSent != contentLen) {
    uploadLastResult = "WDGW: write error (body short)"; client.stop(); return false;
  }

  uint32_t ws = millis();
  while (!client.available() && client.connected() && (millis()-ws)<60000) { delay(100); yield(); }
  uint32_t elapsed = millis()-ws;
  if (!client.available()) {
    bool cc = !client.connected();
    uploadLastResult = cc ? "WDGW: conn closed ("+String(elapsed)+"ms)" : "WDGW: timeout (60s)";
    Serial.printf("[WDGWars] No data after %lums (%s)\n", (unsigned long)elapsed, cc?"conn closed":"timeout");
    client.stop(); return false;
  }

  String status = client.readStringUntil('\n'); status.trim();
  int code = 0;
  if (status.startsWith("HTTP/")) {
    int s1=status.indexOf(' '); if(s1>0){int s2=status.indexOf(' ',s1+1);if(s2>s1) code=status.substring(s1+1,s2).toInt();}
  }
  // Read body for server message
  String body=""; bool inBody=false;
  while (client.connected()||client.available()) {
    String line=client.readStringUntil('\n'); line.trim();
    if (!inBody){if(line.length()==0) inBody=true;} else{body+=line;if(body.length()>512) break;}
  }
  client.stop();
  Serial.printf("[WDGWars] HTTP %d  body: %s\n", code, body.substring(0,100).c_str());
  // V2 API: HTTP 202 Accepted — async job
  if (code == 202) {
    int jobId = 0;
    int ji = body.indexOf("\"job_id\":");
    if (ji >= 0) jobId = body.substring(ji + 9).toInt();
    if (jobId <= 0) { uploadLastResult = "WDGW: no job_id"; return false; }
    Serial.printf("[WDGWars] Job %d submitted, polling...\n", jobId);
    String jobPath = String("/api/v2/upload-job/") + String(jobId);
    for (int poll = 1; poll <= 15; poll++) {
      delay(3000); yield();
      WiFiClientSecure pc; pc.setInsecure(); pc.setTimeout(15000);
      if (!pc.connect(WDGWARS_HOST, WDGWARS_PORT)) { continue; }
      pc.print("GET " + jobPath + " HTTP/1.0\r\n");
      pc.print(String("Host: ")+WDGWARS_HOST+"\r\n");
      pc.print(String("X-API-Key: ")+cfg.wdgwarsApiKey+"\r\n");
      pc.print("Connection: close\r\n\r\n");
      uint32_t pw = millis();
      while (!pc.available() && pc.connected() && (millis()-pw)<10000) { delay(100); yield(); }
      if (!pc.available()) { pc.stop(); continue; }
      pc.readStringUntil('\n');
      String pb=""; bool pIn=false;
      while (pc.connected()||pc.available()) {
        String ln=pc.readStringUntil('\n'); ln.trim();
        if (!pIn){if(ln.length()==0) pIn=true;} else{pb+=ln;if(pb.length()>512) break;}
      }
      pc.stop();
      Serial.printf("[WDGWars] Poll %d: %s\n", poll, pb.substring(0,80).c_str());
      if (pb.indexOf("\"done\"") >= 0) {
        int imp=0; int ii=pb.indexOf("\"imported\":");
        if (ii>=0) imp=pb.substring(ii+11).toInt();
        uploadLastResult = "WDGW OK ("+String(imp)+" net)";
        return true;
      }
      if (pb.indexOf("\"failed\"") >= 0) { uploadLastResult="WDGW: job failed"; return false; }
    }
    uploadLastResult = "WDGW: poll timeout"; return false;
  }
  uploadLastResult = "WDGW fail (" + String(code) + ")";
  Serial.printf("[WDGWars] Upload FAILED HTTP %d: %s\n", code, body.c_str());
  return false;
}

// ---- Empty-file guard: true if file has any data beyond the 2 header lines ----
static bool csvHasDataRows(const String& path) {
  File f = SD.open(path, FILE_READ);
  if (!f) return false;
  for (int i=0; i<2; i++) { if(!f.available()){f.close();return false;} f.readStringUntil('\n'); }
  bool hasData = f.available()>0;
  f.close();
  return hasData;
}

// ---- WDGoWars-only batch upload (web UI button) ----
static uint32_t uploadAllCsvsToWdgwars() {
  if (!sdOk) { uploadLastResult="SD not OK"; return 0; }
  if (cfg.wdgwarsApiKey.length()<8) { uploadLastResult="No API key"; return 0; }

  uploadPausedScanWasEnabled=scanningEnabled; scanningEnabled=false;
  uploading=true; uploadTargetName="WDGW UL";
  uploadDoneFiles=0; uploadFailedFiles=0; uploadTotalFiles=0; uploadCurrentFile="";
  ledBlue();

  digitalWrite(PINS.tft_cs, HIGH);
  File root=SD.open("/logs");
  if (root){File f=root.openNextFile();while(f){String name=normalizeSdPath("/logs",f.name());
    if(name.endsWith(".csv")&&!(currentCsvPath.length()&&name==currentCsvPath)) uploadTotalFiles++;
    f.close();f=root.openNextFile();}root.close();}

  if (uploadTotalFiles==0){uploading=false;uploadTargetName="";scanningEnabled=uploadPausedScanWasEnabled;uploadLastResult="No CSVs to upload";ledOff();return 0;}

  std::vector<String> paths; paths.reserve(uploadTotalFiles+2);
  root=SD.open("/logs");
  if(root){File f=root.openNextFile();while(f){
    String path=normalizeSdPath("/logs",f.name()); f.close();
    if(path.endsWith(".csv")&&!(currentCsvPath.length()&&path==currentCsvPath)) paths.push_back(path);
    f=root.openNextFile();}root.close();}

  uint32_t okCount=0;
  for(size_t i=0;i<paths.size();i++){
    uploadCurrentFile=paths[i];
    tftWigleUploadScreen(uploadDoneFiles,uploadTotalFiles,pathBasename(uploadCurrentFile));
    if(!csvHasDataRows(paths[i])){Serial.printf("[UPLOAD] Empty CSV, deleting: %s\n",pathBasename(paths[i]).c_str());SD.remove(paths[i]);uploadDoneFiles++;continue;}
    bool ok=uploadFileToWdgwars(paths[i]);
    if(ok){okCount++;moveToUploaded(paths[i]);}else{uploadFailedFiles++;}
    uploadDoneFiles++;
    tftWigleUploadScreen(uploadDoneFiles,uploadTotalFiles,pathBasename(uploadCurrentFile));
    if(i<paths.size()-1) delay(1500);
  }
  uploading=false; uploadTargetName=""; scanningEnabled=uploadPausedScanWasEnabled;
  uploadCurrentFile=""; uploadLastResult="WDGWars: "+String(okCount)+"/"+String(uploadTotalFiles);
  ledOff(); forceStatusFullRedraw();
  return okCount;
}

// ---------------- WiGLE Upload ----------------
static bool moveToUploaded(const String& srcPath) {
  if (!sdOk) return false;
  digitalWrite(PINS.tft_cs, HIGH);
  if (!SD.exists(srcPath)) return false;
  if (!SD.exists("/uploaded")) SD.mkdir("/uploaded");

  String dstPath = String("/uploaded/") + pathBasename(srcPath);
  if (SD.exists(dstPath)) SD.remove(dstPath);

  bool ok = SD.rename(srcPath, dstPath);
  if (!ok) {
    // Copy fallback
    File in = SD.open(srcPath, FILE_READ);
    if (!in) return false;
    File out = SD.open(dstPath, FILE_WRITE);
    if (!out) { in.close(); return false; }
    uint8_t buf[1024];
    while (true) { int n = in.read(buf, sizeof(buf)); if (n <= 0) break; out.write(buf, n); delay(0); }
    out.flush(); out.close(); in.close();
    if (!SD.exists(dstPath)) return false;
    SD.remove(srcPath);
  }
  return true;
}

static bool wigleTestToken() {
  wigleLastHttpCode = 0;
  if (WiFi.status() != WL_CONNECTED) { uploadLastResult = "No STA WiFi"; wigleTokenStatus = -1; return false; }
  if (cfg.wigleBasicToken.length() < 8) { uploadLastResult = "No token set"; wigleTokenStatus = -1; return false; }

  const char* pathsToTry[] = { "/api/v2/profile", "/api/v2/stats/user", "/api/v2/user/profile" };
  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(15000);

  for (size_t i = 0; i < sizeof(pathsToTry)/sizeof(pathsToTry[0]); i++) {
    if (!client.connect(WIGLE_HOST, WIGLE_PORT)) { uploadLastResult = "TLS connect fail"; wigleTokenStatus = -1; return false; }
    String req = String("GET ") + pathsToTry[i] + " HTTP/1.1\r\nHost: " + WIGLE_HOST +
                 "\r\nAuthorization: Basic " + cfg.wigleBasicToken + "\r\nConnection: close\r\n\r\n";
    client.print(req);
    String status = client.readStringUntil('\n'); status.trim();
    while (client.connected()) { String line = client.readStringUntil('\n'); if (line == "\r" || line.length() == 0) break; }
    client.stop();

    int code = 0;
    if (status.startsWith("HTTP/")) {
      int sp1 = status.indexOf(' ');
      if (sp1 > 0) { int sp2 = status.indexOf(' ', sp1 + 1); if (sp2 > sp1) code = status.substring(sp1 + 1, sp2).toInt(); }
    }
    wigleLastHttpCode = code;
    if (code == 200) { wigleTokenStatus = 1; uploadLastResult = "Token valid (200)"; return true; }
    if (code == 401 || code == 403) { wigleTokenStatus = -1; uploadLastResult = "Token invalid (" + String(code) + ")"; return false; }
  }
  wigleTokenStatus = 0;
  uploadLastResult = "Token test inconclusive (" + String(wigleLastHttpCode) + ")";
  return false;
}

static bool uploadFileToWigle(const String& path) {
  wigleLastHttpCode = 0;
  if (WiFi.status() != WL_CONNECTED) { uploadLastResult = "No STA WiFi"; return false; }
  if (cfg.wigleBasicToken.length() < 8) { uploadLastResult = "No token set"; return false; }

  digitalWrite(PINS.tft_cs, HIGH);
  if (!SD.exists(path)) { uploadLastResult = "File missing"; return false; }

  File f = SD.open(path, FILE_READ);
  if (!f) { uploadLastResult = "Open fail"; return false; }

  String boundary = "----Piglet-WARDRIVE-BOUNDARY";
  String filename = pathBasename(path);
  String pre = "--" + boundary + "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"" + filename + "\"\r\nContent-Type: text/csv\r\n\r\n";
  String post = "\r\n--" + boundary + "--\r\n";
  uint32_t contentLen = (uint32_t)pre.length() + (uint32_t)f.size() + (uint32_t)post.length();

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(25000);
  if (!client.connect(WIGLE_HOST, WIGLE_PORT)) { uploadLastResult = "TLS connect fail"; f.close(); return false; }

  client.print(String("POST /api/v2/file/upload HTTP/1.0\r\nHost: ") + WIGLE_HOST +
               "\r\nAuthorization: Basic " + cfg.wigleBasicToken +
               "\r\nContent-Type: multipart/form-data; boundary=" + boundary +
               "\r\nContent-Length: " + String(contentLen) + "\r\nConnection: close\r\n\r\n");
  client.print(pre);

  uint8_t buf[1024];
  while (true) { int n = f.read(buf, sizeof(buf)); if (n <= 0) break; client.write(buf, n); delay(0); }
  f.close();
  client.print(post);

  String status = client.readStringUntil('\n'); status.trim();
  int code = 0;
  if (status.startsWith("HTTP/")) {
    int sp1 = status.indexOf(' ');
    if (sp1 > 0) { int sp2 = status.indexOf(' ', sp1 + 1); if (sp2 > sp1) code = status.substring(sp1 + 1, sp2).toInt(); }
  }
  wigleLastHttpCode = code;
  while (client.connected()) { while (client.available()) client.read(); delay(0); }
  client.stop();

  if (code == 200) { uploadLastResult = "Uploaded OK (200)"; return true; }
  uploadLastResult = "Upload failed (" + String(code) + ")";
  return false;
}

// uploadAllCsvsToWigle:
static uint32_t uploadAllCsvsToWigle(int maxFiles = -1) {
  if (!sdOk) { uploadLastResult = "SD not OK"; return 0; }

  uploadPausedScanWasEnabled = scanningEnabled;
  scanningEnabled = false;
  uploading = true;
  uploadDoneFiles = 0;
  uploadFailedFiles = 0;
  uploadTotalFiles = 0;
  uploadCurrentFile = "";
  ledBlue();

  digitalWrite(PINS.tft_cs, HIGH);
  File root = SD.open("/logs");
  if (root) {
    File f = root.openNextFile();
    while (f) {
      String name = normalizeSdPath("/logs", f.name());
      if (name.endsWith(".csv") && !(currentCsvPath.length() && name == currentCsvPath)) uploadTotalFiles++;
      f.close(); f = root.openNextFile();
    }
    root.close();
  }

  if (uploadTotalFiles == 0) {
    uploading = false; scanningEnabled = uploadPausedScanWasEnabled;
    uploadLastResult = "No CSVs to upload"; ledOff(); return 0;
  }

  uint32_t filesToUpload = uploadTotalFiles;
  if (maxFiles > 0 && (uint32_t)maxFiles < uploadTotalFiles) filesToUpload = (uint32_t)maxFiles;

  std::vector<String> paths;
  paths.reserve(filesToUpload + 4);
  root = SD.open("/logs");
  if (root) {
    File f = root.openNextFile();
    while (f) {
      String path = normalizeSdPath("/logs", f.name());
      bool isCsv = path.endsWith(".csv");
      bool isCurrent = (currentCsvPath.length() && path == currentCsvPath);
      f.close();
      if (isCsv && !isCurrent) {
        paths.push_back(path);
        if (maxFiles > 0 && paths.size() >= filesToUpload) break;
      }
      f = root.openNextFile();
    }
    root.close();
  }

  uint32_t okCount = 0;
  for (size_t i = 0; i < paths.size(); i++) {
    uploadCurrentFile = paths[i];

    // Skip and delete header-only files
    if (!csvHasDataRows(paths[i])) {
      Serial.printf("[UPLOAD] Empty CSV, deleting: %s\n", pathBasename(paths[i]).c_str());
      SD.remove(paths[i]);
      uploadDoneFiles++;
      continue;
    }

    // Step 1: WDGoWars first (if configured)
    bool wdgOk = false;
    if (cfg.wdgwarsApiKey.length() >= 8) {
      uploadTargetName = "WDGW UL";
      tftWigleUploadScreen(uploadDoneFiles, uploadTotalFiles, pathBasename(paths[i]));
      wdgOk = uploadFileToWdgwars(paths[i]);
      if (!wdgOk) uploadFailedFiles++;
      if (i < paths.size()-1 || cfg.wigleBasicToken.length() >= 8) delay(1500);
    }

    // Step 2: WiGLE (if configured)
    bool wigleOk = false;
    if (cfg.wigleBasicToken.length() >= 8) {
      uploadTargetName = "WiGLE UL";
      tftWigleUploadScreen(uploadDoneFiles, uploadTotalFiles, pathBasename(paths[i]));
      wigleOk = uploadFileToWigle(paths[i]);
      if (!wigleOk) uploadFailedFiles++;
    }

    if (wigleOk || wdgOk) { okCount++; moveToUploaded(paths[i]); }
    uploadDoneFiles++;
    tftWigleUploadScreen(uploadDoneFiles, uploadTotalFiles, pathBasename(uploadCurrentFile));
    if (i < paths.size()-1) delay(2000);
  }

  uploading = false;
  uploadTargetName = "";
  scanningEnabled = uploadPausedScanWasEnabled;
  uploadCurrentFile = "";
  uploadLastResult = "Uploaded " + String(okCount) + "/" + String(uploadTotalFiles);
  ledOff();
  forceStatusFullRedraw();
  return okCount;
}

// ---------------- TFT UI (80x160 portrait) ----------------

static const char* SPLASH_SLOGANS[] = {
  "Makin' Bacon", "Magic Smoke..", "Ahhhhhhhh",
  "BOSH", "Oink Oink", "Love your Face",
  "Pigs R Friends", "Ham Wuz Here", "NFC Propaganda",
};
static const size_t SPLASH_SLOGAN_COUNT = sizeof(SPLASH_SLOGANS) / sizeof(SPLASH_SLOGANS[0]);

static const char* pickSplashSlogan() {
  return SPLASH_SLOGANS[(uint32_t)esp_random() % SPLASH_SLOGAN_COUNT];
}

static void tftDrawCentered(int y, const char* txt, int textSize) {
  if (!txt) return;
  tft.setTextSize(textSize);
  int tw = tftTextWidth(txt, textSize);
  int x = (tft.width() - tw) / 2;
  if (x < 0) x = 0;
  tft.setCursor(x, y);
  tft.print(txt);
}

static void tftSplashAnimateOnce(const char* slogan, uint32_t animMs = 1400, uint32_t frameMs = 35) {
  int W = tft.width();   // 80
  int H = tft.height();  // 160

  const int BAR_W = 64;
  const int BAR_H = 12;
  const int BAR_X = (W - BAR_W) / 2;
  const int BAR_Y = 90;
  const int blockW = 16;

  int pos = 0, dir = 1;
  uint32_t start = millis();

  while (millis() - start < animMs) {
    tft.fillScreen(BLACK);
    tft.setTextColor(WHITE, BLACK);

    tftDrawCentered(14, "Piglet", 2);
    tftDrawCentered(42, "Wardriver", 1);

    tft.drawRect(BAR_X, BAR_Y, BAR_W, BAR_H, WHITE);
    tft.fillRect(BAR_X + 1 + pos, BAR_Y + 1, blockW, BAR_H - 2, WHITE);

    if (slogan && slogan[0]) {
      tft.setTextSize(1);
      int tw = tftTextWidth(slogan, 1);
      int tx = (W - tw) / 2;
      if (tx < 0) tx = 0;
      tft.setCursor(tx, BAR_Y + BAR_H + 8);
      tft.print(slogan);
    }

    // Version — lower-right corner
    tft.setTextSize(1);
    tft.setTextColor(WHITE, BLACK);
    tft.setCursor(W - (int)strlen(FIRMWARE_VERSION) * 6 - 2, tft.height() - 8 - 2);
    tft.print(FIRMWARE_VERSION);

    pos += dir * 4;
    int maxPos = BAR_W - 2 - blockW;
    if (pos <= 0) { pos = 0; dir = 1; }
    if (pos >= maxPos) { pos = maxPos; dir = -1; }

    delay(frameMs);
  }

  // Final filled state
  tft.fillScreen(BLACK);
  tft.setTextColor(WHITE, BLACK);
  tftDrawCentered(14, "Piglet", 2);
  tftDrawCentered(42, "Wardriver", 1);
  tft.drawRect(BAR_X, BAR_Y, BAR_W, BAR_H, WHITE);
  tft.fillRect(BAR_X + 1, BAR_Y + 1, BAR_W - 2, BAR_H - 2, WHITE);

  if (slogan && slogan[0]) {
    tft.setTextColor(BLACK, WHITE);
    tft.setTextSize(1);
    int tw = tftTextWidth(slogan, 1);
    int tx = (W - tw) / 2;
    if (tx < 0) tx = 0;
    tft.setCursor(tx, BAR_Y + BAR_H + 8);
    tft.print(slogan);
    tft.setTextColor(WHITE, BLACK);
  }

  // Version — lower-right corner
  tft.setTextSize(1);
  tft.setTextColor(WHITE, BLACK);
  tft.setCursor(W - (int)strlen(FIRMWARE_VERSION) * 6 - 2, tft.height() - 8 - 2);
  tft.print(FIRMWARE_VERSION);

  delay(200);
}

static void tftWigleUploadScreen(uint32_t done, uint32_t total, const String& filename) {
  int W = tft.width();

  const int BAR_W = 64;
  const int BAR_H = 10;
  const int BAR_X = (W - BAR_W) / 2;
  const int BAR_Y = 80;

  float pct = (total == 0) ? 0.0f : (float)done / (float)total;
  if (pct > 1) pct = 1;

  tft.fillScreen(BLACK);
  tft.setTextColor(WHITE, BLACK);

  tftDrawCentered(10, "Piglet", 2);

  // Service label: use uploadTargetName if set, otherwise generic
  const char* label = uploadTargetName.length() ? uploadTargetName.c_str() : "Uploading";
  tftDrawCentered(40, label, 1);

  // File count + right-aligned fail counter
  char buf[32];
  snprintf(buf, sizeof(buf), "%lu / %lu", (unsigned long)done, (unsigned long)total);
  tftDrawCentered(60, buf, 1);
  if (uploadFailedFiles > 0) {
    tft.setTextSize(1);
    char fbuf[10];
    snprintf(fbuf, sizeof(fbuf), "F:%lu", (unsigned long)uploadFailedFiles);
    tft.setCursor(W - (int)strlen(fbuf)*6 - 2, 60);
    tft.print(fbuf);
  }

  tft.drawRect(BAR_X, BAR_Y, BAR_W, BAR_H, WHITE);
  int fillW = (int)((BAR_W - 2) * pct);
  if (fillW > 0) tft.fillRect(BAR_X + 1, BAR_Y + 1, fillW, BAR_H - 2, WHITE);

  // Truncated filename
  String name = filename;
  if (name.length() > 12) name = name.substring(0, 12) + "..";
  tftDrawCentered(BAR_Y + BAR_H + 6, name.c_str(), 1);

  uiFirstDraw = true;
}

// Direction arrow (small, for 80px-wide screen)
static void drawDirectionArrow(int cx, int cy, float deg, int sizePx = 8) {
  float rad = (deg - 90.0f) * 0.0174533f;
  int x1 = cx + (int)(cos(rad) * sizePx);
  int y1 = cy + (int)(sin(rad) * sizePx);
  int x2 = cx + (int)(cos(rad + 2.6f) * (sizePx * 0.55f));
  int y2 = cy + (int)(sin(rad + 2.6f) * (sizePx * 0.55f));
  int x3 = cx + (int)(cos(rad - 2.6f) * (sizePx * 0.55f));
  int y3 = cy + (int)(sin(rad - 2.6f) * (sizePx * 0.55f));
  tft.fillTriangle(x1, y1, x2, y2, x3, y3, COLOR_RED);
}

static const char* headingToDir8(float deg) {
  static const char* dirs[] = {"N","NE","E","SE","S","SW","W","NW"};
  while (deg < 0) deg += 360.0f;
  while (deg >= 360.0f) deg -= 360.0f;
  return dirs[(int)((deg + 22.5f) / 45.0f) & 7];
}

// Main status display (80x160 portrait)
static void updateTFT(float speedValue) {
  int W = tft.width();

  // Capture and clear the full-redraw flag first.
  // IMPORTANT: do NOT use uiFirstDraw as the repaint condition after this point —
  // the diff-repaint cache values may match current values even after a fillScreen,
  // so we use `forceAll` to bypass the cache on the first draw after a page switch.
  bool forceAll = uiFirstDraw;
  if (uiFirstDraw) {
    tft.fillScreen(BLACK);
    tft.setTextColor(WHITE, BLACK);
    uiFirstDraw = false;
  }

  bool allowScan = scanningEnabled && sdOk && (userScanOverride || !autoPaused);
  bool staNow = (WiFi.status() == WL_CONNECTED);
  String ipNow = staNow ? WiFi.localIP().toString() : "";

  tft.setTextColor(WHITE, BLACK);
  tft.setTextSize(1);

  int y = 0;
  const int line = 12;

  auto repaintLine = [&](bool changed, int yPos, const String& txt) {
    if (!changed) return;
    tft.fillRect(0, yPos, W, line, BLACK);
    tft.setCursor(2, yPos + 2);
    tft.print(txt);
  };

  // Header
  tft.setTextSize(2);
  tftDrawCentered(2, "Piglet", 2);
  tft.setTextSize(1);
  y = 22;

  // Status dots row: Scan, SD, GPS, WiFi (always drawn — no diff cache)
  {
    int dotY = y + 3;
    int dotR = 3;
    int gap = 18;
    int startX = 4;

    // Scan dot
    uint16_t scanCol = allowScan ? COLOR_GREEN : COLOR_YELLOW;
    tft.fillCircle(startX, dotY, dotR, scanCol);
    // SD dot
    uint16_t sdCol = sdOk ? COLOR_GREEN : COLOR_RED;
    tft.fillCircle(startX + gap, dotY, dotR, sdCol);
    // GPS dot
    uint16_t gpsCol = gpsHasFix ? COLOR_GREEN : COLOR_YELLOW;
    tft.fillCircle(startX + gap*2, dotY, dotR, gpsCol);
    // WiFi dot
    uint16_t wifiCol = staNow ? COLOR_GREEN : (apWindowActive ? COLOR_CYAN : COLOR_RED);
    tft.fillCircle(startX + gap*3, dotY, dotR, wifiCol);
  }
  y += line;

  // 2.4G count
  repaintLine(forceAll || prevFound2G != networksFound2G, y,
    String("2.4G: ") + String(networksFound2G));
  prevFound2G = networksFound2G;
  y += line;

  // 5G count
  repaintLine(forceAll || prevFound5G != networksFound5G, y,
    String(" 5G:  ") + String(networksFound5G));
  prevFound5G = networksFound5G;
  y += line;

  // GPS
  repaintLine(forceAll || prevGpsFix != gpsHasFix, y,
    String("GPS: ") + (gpsHasFix ? "FIX" : "---") + " " + String(uiGpsSats) + "sat");
  prevGpsFix = gpsHasFix;
  y += line;

  // IP / AP — always repaint while the AP window is active so the countdown updates
  if (forceAll || prevIp != ipNow || prevSta != staNow || apWindowActive) {
    tft.fillRect(0, y, W, line, BLACK);
    tft.setCursor(2, y + 2);
    if (staNow) {
      tft.print(ipNow);
    } else if (apWindowActive) {
      uint32_t remaining = (AP_WINDOW_MS - (millis() - apStartMs) + 999) / 1000;
      tft.print("AP "); tft.print(remaining); tft.print("s");
    } else {
      tft.print("AP: OFF");
    }
    prevIp = ipNow;
    prevSta = staNow;
  }
  y += line;

  // Speed + heading
  bool speedChanged = forceAll || (fabs(prevSpeed - speedValue) > 0.1f);
  if (speedChanged) {
    tft.fillRect(0, y, W, line + 4, BLACK);
    tft.setCursor(2, y + 2);
    tft.print(String(speedValue, 1));
    tft.print(cfg.speedUnits == "mph" ? " mph" : " kmh");

    // Small direction arrow
    drawDirectionArrow(W - 14, y + 6, uiHeadingDeg, 6);
    prevSpeed = speedValue;
  }
  y += line + 4;

  // WiGLE / upload area
  if (uploading) {
    if (forceAll || prevUploadDone != uploadDoneFiles || prevUploadTotal != uploadTotalFiles) {
      tft.fillRect(0, y, W, 40, BLACK);
      tft.setCursor(2, y);
      tft.print("UL ");
      tft.print(uploadDoneFiles);
      tft.print("/");
      tft.print(uploadTotalFiles);

      float pct = (uploadTotalFiles == 0) ? 0.0f : (float)uploadDoneFiles / (float)uploadTotalFiles;
      int barW = W - 8;
      tft.drawRect(4, y + 12, barW, 6, WHITE);
      int fill = (int)((barW - 2) * pct);
      if (fill > 0) tft.fillRect(5, y + 13, fill, 4, WHITE);

      prevUploadDone = uploadDoneFiles;
      prevUploadTotal = uploadTotalFiles;
    }
  } else {
    if (forceAll || prevUploadMsg != uploadLastResult) {
      tft.fillRect(0, y, W, 40, BLACK);
      tft.setCursor(2, y);
      String msg = uploadLastResult.length() ? uploadLastResult : String("-");
      if (msg.length() > 13) msg = msg.substring(0, 13);
      tft.print(msg);
      prevUploadMsg = uploadLastResult;
    }
  }
}

static void forceStatusFullRedraw() {
  uiFirstDraw = true;
  prevAllowScan   = !prevAllowScan;
  prevSdOk        = !prevSdOk;
  prevGpsFix      = !prevGpsFix;
  prevSta         = !prevSta;
  prevFound2G     = 0xFFFFFFFF;
  prevFound5G     = 0xFFFFFFFF;
  prevSpeed       = -9999.0f;
  prevIp          = "";
  prevUploadMsg   = "";
  prevUploadDone  = 0xFFFFFFFF;
  prevUploadTotal = 0xFFFFFFFF;
}

// Called on every page change — resets all per-page state so draw functions
// do a full layout on next call. Status page uses forceStatusFullRedraw().
static void tftPageEntered(uint8_t newPage) {
  pageNeedsInit[1] = true;
  pageNeedsInit[2] = true;
  pageNeedsInit[3] = true;
  pageNeedsInit[4] = true;
  if (newPage == 0) forceStatusFullRedraw();
  if (newPage == 4) enterNodeMode();
  Serial.printf("[PAGE] -> %d\n", newPage);
}

// ================================================================
//  Page 1: Network counts (large text)
// ================================================================
static void drawPageNetworks() {
  static uint32_t prevN2G = 0xFFFFFFFF, prevN5G = 0xFFFFFFFF;
  int W = tft.width();

  if (pageNeedsInit[1]) {
    pageNeedsInit[1] = false;
    tft.fillScreen(BLACK);
    tft.setTextColor(WHITE, BLACK);
    tft.setTextSize(2);
    tftDrawCentered(2, "Networks", 2);
    tft.drawFastHLine(0, 20, W, WHITE);
    tft.setTextSize(1);
    tft.setCursor(2, 26);  tft.print("2.4 GHz");
    tft.setCursor(2, 70);  tft.print("5 GHz");
    tft.setCursor(2, 114); tft.print("Total");
    prevN2G = prevN5G = 0xFFFFFFFF;  // force number repaint
  }

  bool changed = false;
  if (networksFound2G != prevN2G) {
    prevN2G = networksFound2G;
    tft.fillRect(0, 36, W, 28, BLACK);
    tft.setTextColor(WHITE, BLACK); tft.setTextSize(3);
    tft.setCursor(2, 36); tft.print(networksFound2G);
    changed = true;
  }
  if (networksFound5G != prevN5G) {
    prevN5G = networksFound5G;
    tft.fillRect(0, 80, W, 28, BLACK);
    tft.setTextColor(WHITE, BLACK); tft.setTextSize(3);
    tft.setCursor(2, 80); tft.print(networksFound5G);
    changed = true;
  }
  if (changed) {
    tft.fillRect(0, 124, W, 28, BLACK);
    tft.setTextColor(WHITE, BLACK); tft.setTextSize(3);
    tft.setCursor(2, 124); tft.print(networksFound2G + networksFound5G);
  }
}

// ================================================================
//  Page 2: Navigation — large compass arrow + direction + speed
// ================================================================

// Draws GPS satellite-strength bars in the top-right of the header row.
static void drawGpsSatBars(int y) {
  int W = tft.width();
  int sats = (gpsHasFix && gps.satellites.isValid()) ? (int)gps.satellites.value() : 0;
  if (sats > 6) sats = 6;
  const int barStep = 5, barMaxH = 12, startX = W - 6 * barStep - 2;
  for (int i = 0; i < 6; i++) {
    int bh = 2 + i * 2;
    int bx = startX + i * barStep;
    int by = y + barMaxH - bh;
    if (i < sats) tft.fillRect(bx, by, 3, bh, WHITE);
    else          tft.drawRect(bx, by, 3, bh, WHITE);
  }
}

// Filled dart arrow pointing in direction deg (0=N, 90=E).
// haveHeld=false draws a no-fix ring+cross instead.
static void drawNavArrow(int cx, int cy, float deg, bool haveHeld) {
  if (!haveHeld) {
    tft.drawCircle(cx, cy, 27, WHITE);
    tft.drawCircle(cx, cy, 26, WHITE);
    tft.drawLine(cx - 9, cy - 9, cx + 9, cy + 9, WHITE);
    tft.drawLine(cx + 9, cy - 9, cx - 9, cy + 9, WHITE);
    return;
  }
  const float k  = 3.14159265f / 180.0f;
  const int   sz = 26;
  float rad = (deg - 90.0f) * k;
  int x1 = cx + (int)(cosf(rad) * sz);
  int y1 = cy + (int)(sinf(rad) * sz);
  int x2 = cx + (int)(cosf(rad + 2.42f) * (sz * 0.58f));
  int y2 = cy + (int)(sinf(rad + 2.42f) * (sz * 0.58f));
  int x3 = cx + (int)(cosf(rad - 2.42f) * (sz * 0.58f));
  int y3 = cy + (int)(sinf(rad - 2.42f) * (sz * 0.58f));
  tft.fillTriangle(x1, y1, x2, y2, x3, y3, WHITE);
  // Notch cutout toward center
  int nx = cx + (int)(cosf(rad) * (sz * 0.18f));
  int ny = cy + (int)(sinf(rad) * (sz * 0.18f));
  tft.fillTriangle(x2, y2, x3, y3, nx, ny, BLACK);
  tft.drawCircle(cx, cy, sz + 3, WHITE);
}

static void drawPageNavigation(float speedValue) {
  static float    prevSpeed   = -9999.0f;
  static double   prevHeading = NAN;
  static bool     prevHeld    = false;
  int W = tft.width();
  const int arrowCX = W / 2, arrowCY = 73;

  bool initFrame = pageNeedsInit[2];
  if (initFrame) {
    pageNeedsInit[2] = false;
    tft.fillScreen(BLACK);
    tft.setTextColor(WHITE, BLACK);
    tft.setTextSize(2); tft.setCursor(2, 2); tft.print("Nav");
    tft.drawFastHLine(0, 20, W, WHITE);
    prevSpeed = -9999.0f; prevHeading = NAN; prevHeld = false;
  }

  // Update held heading
  uint32_t nowMs = millis();
  if (gpsHasFix &&
      gps.speed.isValid()  && gps.speed.kmph()  >= HEADING_MIN_SPEED_KMPH &&
      gps.course.isValid() && gps.course.age()  < 2000) {
    lastGoodHeadingDeg = gps.course.deg();
    lastGoodHeadingMs  = nowMs;
  }
  bool   haveHeld   = !isnan(lastGoodHeadingDeg) &&
                      ((nowMs - lastGoodHeadingMs) <= HEADING_HOLD_MS);
  float  displayDeg = haveHeld ? (float)lastGoodHeadingDeg : 0.0f;

  // GPS sat bars (always refresh — sats can change independently)
  tft.fillRect(W - 32, 2, 32, 16, BLACK);
  drawGpsSatBars(2);

  // Compass + direction label — repaint when heading or fix state changes
  bool compassChanged = initFrame ||
                        (haveHeld != prevHeld) ||
                        (haveHeld && (isnan(prevHeading) ||
                         fabsf(displayDeg - (float)prevHeading) > 1.5f));
  if (compassChanged) {
    tft.fillRect(0, 21, W, 92, BLACK);  // clear compass + direction label area
    drawNavArrow(arrowCX, arrowCY, displayDeg, haveHeld);
    const char* dirStr = haveHeld ? headingToDir8((float)lastGoodHeadingDeg) : "---";
    tft.setTextColor(WHITE, BLACK); tft.setTextSize(2);
    int tw = tftTextWidth(dirStr, 2);
    tft.setCursor((W - tw) / 2, 110); tft.print(dirStr);
    prevHeld    = haveHeld;
    prevHeading = displayDeg;
  }

  // Speed (repaint when value changes by >=0.1)
  if (initFrame || fabsf(speedValue - prevSpeed) >= 0.1f) {
    tft.fillRect(0, 128, W, 32, BLACK);
    char buf[10];
    snprintf(buf, sizeof(buf), "%.1f", speedValue);
    tft.setTextColor(WHITE, BLACK); tft.setTextSize(2);
    int tw = tftTextWidth(buf, 2);
    tft.setCursor((W - tw) / 2, 130); tft.print(buf);
    tft.setTextSize(1);
    const char* units = (cfg.speedUnits == "mph") ? "mph" : "km/h";
    tw = tftTextWidth(units, 1);
    tft.setCursor((W - tw) / 2, 150); tft.print(units);
    prevSpeed = speedValue;
  }
}

// ================================================================
//  Page 3: Walking pig animation  (44×26 bounding box, +5 legs)
// ================================================================
static const int16_t PIG_W = 44;
static const int16_t PIG_H = 26;

static void drawPigTFT(int16_t x, int16_t y, uint8_t phase) {
  tft.fillRoundRect(x + 14, y + 3,  22, 15, 7, WHITE);  // body
  tft.fillCircle   (x + 14, y + 10, 7,        WHITE);   // head
  tft.fillRoundRect(x +  2, y +  8,  9,  7, 3, WHITE);  // snout
  tft.drawPixel(x +  4, y + 11, BLACK);                  // nostril L
  tft.drawPixel(x +  6, y + 11, BLACK);                  // nostril R
  tft.fillRect (x +  9, y +  7, 3, 3,   BLACK);          // eye socket
  tft.drawPixel(x + 10, y +  7, WHITE);                  // catchlight
  tft.fillTriangle(x+12,y+4, x+18,y+5, x+15,y+0, WHITE);// ear
  tft.drawLine (x + 14, y +  2, x + 16, y +  4, BLACK); // inner ear
  tft.drawLine (x +  6, y + 14, x +  9, y + 15, BLACK); // mouth
  // curly tail
  tft.drawPixel(x+36,y+6,WHITE); tft.drawPixel(x+37,y+5,WHITE);
  tft.drawPixel(x+38,y+5,WHITE); tft.drawPixel(x+39,y+6,WHITE);
  tft.drawPixel(x+39,y+7,WHITE); tft.drawPixel(x+38,y+8,WHITE);
  tft.drawPixel(x+39,y+9,WHITE); tft.drawPixel(x+40,y+9,WHITE);
  tft.drawPixel(x+41,y+8,WHITE);
  tft.drawFastHLine(x + 15, y + 18, 20, WHITE);          // belly line
  // legs — 2-phase walk cycle
  bool liftA = (phase & 1);
  int16_t legTop = y + 18;
  const int16_t lx[4] = { (int16_t)(x+16),(int16_t)(x+22),
                           (int16_t)(x+28),(int16_t)(x+33) };
  for (int i = 0; i < 4; i++) {
    bool lift = (i == 0 || i == 3) ? liftA : !liftA;
    if (lift) {
      tft.drawLine(lx[i],     legTop,     lx[i]-2, legTop+5, WHITE);
      tft.drawLine(lx[i]-2,   legTop+5,   lx[i],   legTop+5, WHITE);
    } else {
      tft.drawLine(lx[i],   legTop,     lx[i],   legTop+5, WHITE);
      tft.drawLine(lx[i],   legTop+5,   lx[i]+2, legTop+5, WHITE);
    }
  }
}

// ---- Twerk state (T-Dongle) ----
static bool     pigTwerking_   = false;
static uint32_t pigTwerkMs_    = 0;
static uint8_t  pigTwerkPhase_ = 0;
static int16_t  pigTwerkX_     = 18;  // centered: (80-44)/2

static void pigTwerkStartTFT() {
  if (pigTwerking_) return;
  pigTwerking_   = true;
  pigTwerkMs_    = millis();
  pigTwerkPhase_ = 0;
  pigTwerkX_     = pig.x;
  Serial.println("[PIG] TWERK ACTIVATED");
}

// ---- Sasquatch walk ----
static bool     sqActive_  = false;
static int16_t  sqX_       = -26;
static uint32_t sqMs_      = 0;
static uint8_t  sqPhase_   = 0;

static void drawSasquatchTFT(int16_t x, int16_t y, uint8_t phase) {
  // Classic frame 352 pose: mid-stride, upright, head turned back over right shoulder.
  bool swing = (phase & 1);
  // --- Head (turned back, flat-top sagittal crest) ---
  tft.fillRoundRect(x + 14, y, 7, 7, 2, WHITE);
  tft.fillRect(x + 14, y, 7, 3, WHITE);
  tft.fillRect(x + 13, y + 3, 2, 4, WHITE);
  // --- Neck ---
  tft.fillRect(x + 14, y + 6, 5, 3, WHITE);
  // --- Torso ---
  tft.fillRoundRect(x + 10, y + 8, 10, 14, 3, WHITE);
  tft.fillRect(x + 9, y + 9, 3, 10, WHITE);
  tft.fillRect(x + 19, y + 9, 2, 8, WHITE);
  // --- Shoulder hump ---
  tft.fillRect(x + 12, y + 7, 8, 3, WHITE);
  // --- Arms ---
  if (swing) {
    tft.drawLine(x + 10, y + 11, x + 6, y + 22, WHITE);
    tft.drawLine(x + 10, y + 12, x + 7, y + 22, WHITE);
    tft.drawLine(x + 11, y + 12, x + 7, y + 23, WHITE);
    tft.drawLine(x + 19, y + 11, x + 23, y + 20, WHITE);
    tft.drawLine(x + 19, y + 12, x + 24, y + 20, WHITE);
  } else {
    tft.drawLine(x + 10, y + 11, x + 7, y + 20, WHITE);
    tft.drawLine(x + 10, y + 12, x + 8, y + 20, WHITE);
    tft.drawLine(x + 19, y + 11, x + 22, y + 22, WHITE);
    tft.drawLine(x + 19, y + 12, x + 23, y + 22, WHITE);
    tft.drawLine(x + 20, y + 12, x + 23, y + 23, WHITE);
  }
  // --- Legs ---
  int16_t hip = y + 21;
  if (swing) {
    tft.fillRect(x + 11, hip, 4, 4, WHITE);
    tft.fillRect(x + 13, hip + 4, 3, 4, WHITE);
    tft.fillRect(x + 14, hip + 8, 3, 2, WHITE);
    tft.fillRect(x + 13, hip + 10, 6, 2, WHITE);
    tft.fillRect(x + 14, hip, 4, 5, WHITE);
    tft.drawLine(x + 14, hip + 5, x + 10, hip + 9, WHITE);
    tft.drawLine(x + 15, hip + 5, x + 11, hip + 9, WHITE);
    tft.drawLine(x + 16, hip + 5, x + 12, hip + 9, WHITE);
    tft.fillRect(x + 7, hip + 9, 6, 2, WHITE);
    tft.fillRect(x + 6, hip + 10, 3, 2, WHITE);
  } else {
    tft.fillRect(x + 11, hip, 4, 5, WHITE);
    tft.drawLine(x + 11, hip + 5, x + 8, hip + 9, WHITE);
    tft.drawLine(x + 12, hip + 5, x + 9, hip + 9, WHITE);
    tft.drawLine(x + 13, hip + 5, x + 10, hip + 9, WHITE);
    tft.fillRect(x + 5, hip + 9, 6, 2, WHITE);
    tft.fillRect(x + 4, hip + 10, 3, 2, WHITE);
    tft.fillRect(x + 14, hip, 4, 4, WHITE);
    tft.fillRect(x + 16, hip + 4, 3, 4, WHITE);
    tft.fillRect(x + 17, hip + 8, 3, 2, WHITE);
    tft.fillRect(x + 16, hip + 10, 6, 2, WHITE);
  }
}

static void sasquatchStartTFT() {
  if (sqActive_ || pigTwerking_) return;
  sqActive_ = true;
  sqX_      = -26;
  sqMs_     = millis();
  sqPhase_  = 0;
  tft.fillRect(0, 21, tft.width(), tft.height() - 21, BLACK);
  Serial.println("[SQ] SIGHTING");
}

// Twerk draw: FRONT HALF (head, front legs) stays at y.
//             BACK HALF (body, tail, back legs) rises up by 'rise' pixels.
static void drawPigTwerkTFT(int16_t x, int16_t y, uint8_t phase) {
  int8_t rise = 0;
  if      (phase == 1) rise = 8;
  else if (phase == 2) rise = 2;
  else if (phase == 3) rise = 10;
  int16_t by = y - rise;

  // ---- BACK HALF (drawn first so front overlaps) ----
  tft.fillRoundRect(x + 14, by + 3, 22, 15, 7, WHITE);
  tft.drawFastHLine(x + 15, by + 18, 20, WHITE);
  // Tail: flick up when butt is high
  if (rise >= 6) {
    tft.drawPixel(x+36,by+1,WHITE); tft.drawPixel(x+37,by+0,WHITE);
    tft.drawPixel(x+38,by+0,WHITE); tft.drawPixel(x+39,by+1,WHITE);
    tft.drawPixel(x+40,by+2,WHITE); tft.drawPixel(x+41,by+1,WHITE);
  } else {
    tft.drawPixel(x+36,by+6,WHITE); tft.drawPixel(x+37,by+5,WHITE);
    tft.drawPixel(x+38,by+5,WHITE); tft.drawPixel(x+39,by+6,WHITE);
    tft.drawPixel(x+39,by+7,WHITE); tft.drawPixel(x+38,by+8,WHITE);
    tft.drawPixel(x+39,by+9,WHITE); tft.drawPixel(x+40,by+9,WHITE);
    tft.drawPixel(x+41,by+8,WHITE);
  }
  // Back 2 legs — splay outward when butt is high
  int16_t blt = by + 18;
  const int16_t blx[2] = { (int16_t)(x+28), (int16_t)(x+33) };
  for (int i = 0; i < 2; i++) {
    if (rise >= 6) {
      tft.drawLine(blx[i],   blt,   blx[i]+4, blt+5, WHITE);
      tft.drawLine(blx[i]+4, blt+5, blx[i]+6, blt+5, WHITE);
    } else {
      tft.drawLine(blx[i], blt,   blx[i],   blt+5, WHITE);
      tft.drawLine(blx[i], blt+5, blx[i]+2, blt+5, WHITE);
    }
  }

  // ---- FRONT HALF (drawn on top, stays at y) ----
  tft.fillCircle   (x + 14, y + 10, 7,        WHITE);
  tft.fillRoundRect(x +  2, y +  8,  9,  7, 3, WHITE);
  tft.drawPixel(x +  4, y + 11, BLACK);
  tft.drawPixel(x +  6, y + 11, BLACK);
  tft.fillRect (x +  9, y +  7, 3, 3,   BLACK);
  tft.drawPixel(x + 10, y +  7, WHITE);
  tft.fillTriangle(x+12,y+4, x+18,y+5, x+15,y+0, WHITE);
  tft.drawLine (x + 14, y +  2, x + 16, y +  4, BLACK);
  tft.drawLine (x +  6, y + 14, x +  9, y + 15, BLACK);
  // Front 2 legs
  int16_t flt = y + 18;
  const int16_t flx[2] = { (int16_t)(x+16), (int16_t)(x+22) };
  for (int i = 0; i < 2; i++) {
    tft.drawLine(flx[i], flt,   flx[i],   flt+5, WHITE);
    tft.drawLine(flx[i], flt+5, flx[i]+2, flt+5, WHITE);
  }
}

static void pigAnimTickTFT() {
  // Full-layout init on page entry (checked before frame timer)
  if (pageNeedsInit[3]) {
    pageNeedsInit[3] = false;
    tft.fillScreen(BLACK);
    tft.setTextColor(WHITE, BLACK); tft.setTextSize(2);
    tftDrawCentered(2, "Piglet", 2);
    tft.drawFastHLine(0, 20, tft.width(), WHITE);
    pig.x = 0; pig.dx = 1; pig.phase = 0;
    pig.lastMs = millis();
    return;  // draw pig on next tick
  }

  uint32_t now = millis();

  // Sasquatch walk — runs instead of pig when active
  if (sqActive_) {
    if (now - sqMs_ < 80) return;
    sqMs_ = now;
    tft.fillRect(0, 21, tft.width(), tft.height() - 21, BLACK);
    sqX_ += 2;
    sqPhase_ = (sqPhase_ + 1) & 3;
    if (sqX_ < tft.width()) drawSasquatchTFT(sqX_, 76, sqPhase_);
    if (sqX_ > tft.width() + 4) {
      sqActive_ = false;
      tft.fillRect(0, 21, tft.width(), tft.height() - 21, BLACK);
      pig.x = 0; pig.dx = 1; pig.phase = 0;
      Serial.println("[SQ] Gone");
    }
    return;
  }

  // Twerk expires after 3 s
  if (pigTwerking_ && (now - pigTwerkMs_ >= 3000)) {
    pigTwerking_ = false;
    tft.fillRect(0, 21, tft.width(), tft.height() - 21, BLACK);
    Serial.println("[PIG] Twerk complete");
  }

  uint16_t fms = pigTwerking_ ? 55 : pig.frameMs;
  if (now - pig.lastMs < fms) return;
  pig.lastMs = now;

  if (pigTwerking_) {
    pigTwerkPhase_ = (pigTwerkPhase_ + 1) & 3;
    // drawPigTwerkTFT handles rise internally — just erase and redraw
    tft.fillRect(0, 21, tft.width(), tft.height() - 21, BLACK);
    drawPigTwerkTFT(pigTwerkX_, pig.y, pigTwerkPhase_);
    // Flash "OINK!" above the pig on odd phases
    if (pigTwerkPhase_ & 1) {
      tft.setTextSize(1);
      tft.setTextColor(COLOR_YELLOW, BLACK);
      tftDrawCentered(30, "OINK!", 1);
      tft.setTextColor(WHITE, BLACK);
    }
  } else {
    // Erase previous pig position (body + legs + 1px bob margin)
    tft.fillRect(pig.x, pig.y, PIG_W, PIG_H + 3, BLACK);
    // Move and bounce
    pig.x += pig.dx;
    int16_t maxX = tft.width() - PIG_W;
    if (pig.x <= 0)    { pig.x = 0;    pig.dx =  1; }
    if (pig.x >= maxX) { pig.x = maxX; pig.dx = -1; }
    pig.phase = (pig.phase + 1) & 3;
    int16_t bob = (pig.phase == 1 || pig.phase == 3) ? 1 : 0;
    drawPigTFT(pig.x, pig.y + bob, pig.phase);
  }
}

// ================================================================
//  JCMK ESP-Now Mesh Node (Page 4)
//  Wire-compatible with justcallmekoko/ESP32DualBandWardriver node role.
// ================================================================

// ------------- Protocol constants --------------------------------
static const uint8_t  JCMK_ESPNOW_CH       = 6;
static const uint8_t  JCMK_MAGIC[4]        = {'E','N','O','W'};
static const uint8_t  JCMK_BCAST[6]        = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
static const uint32_t JCMK_REQ_INIT_MS     = 300;
static const uint32_t JCMK_REQ_MAX_MS      = 5000;
static const uint32_t JCMK_HB_MS          = 5000;
static const uint32_t NODE_SCAN_DWELL_MS  = 80;    // ms per channel (JCMK CHANNEL_TIMER)
static const uint32_t NODE_ADMIN_WIN_MS   = 300;   // ch-6 window after each full cycle
#define JCMK_TEXT_MAX 200

enum JcmkMsgType : uint8_t {
  JCMK_MSG_CORE_REQUEST = 1,
  JCMK_MSG_CORE_REPLY   = 2,
  JCMK_MSG_HEARTBEAT    = 3,
  JCMK_MSG_TEXT         = 4,
  JCMK_MSG_ADMIN        = 5
};

// Packed structs match JCMK wire layout exactly
typedef struct __attribute__((packed)) {
  char     magic[4];
  uint8_t  type;
  uint32_t counter;
  uint16_t len;
  char     text[JCMK_TEXT_MAX + 1];
} jcmk_text_msg_t;

typedef struct __attribute__((packed)) {
  char    magic[4];
  uint8_t type;
  uint8_t assignment_version;
  uint8_t node_index;
  uint8_t node_count;
  uint8_t start_channel_idx;
  uint8_t end_channel_idx;
} jcmk_admin_msg_t;

typedef struct __attribute__((packed)) {
  char     magic[4];
  uint8_t  type;
  uint32_t counter;
} jcmk_hb_msg_t;

typedef struct __attribute__((packed)) {
  char    magic[4];
  uint8_t type;
} jcmk_req_msg_t;

// JCMK scan-channel table — must match Core exactly
static const uint8_t JCMK_CHANNELS[] = {
  1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14,
  36, 40, 44, 48, 52, 56, 60, 64,
  100, 112, 116, 120, 124, 128, 132, 136, 140, 144,
  149, 153, 157, 161, 165, 169, 173, 177
};
static const uint8_t JCMK_NUM_CHANNELS = (uint8_t)(sizeof(JCMK_CHANNELS));

// ------------- Node state ----------------------------------------
static bool     meshNodeActive   = false;
static bool     jcmkHaveCore     = false;
static uint8_t  jcmkCoreMac[6]  = {0};
static uint8_t  jcmkStartIdx    = 0;
static uint8_t  jcmkEndIdx      = 0;  // set in enterNodeMode
static uint8_t  jcmkAssignVer   = 0;
static uint32_t jcmkNetworksFound = 0;  // raw networks seen each session
static uint32_t jcmkSentCount   = 0;
static uint32_t jcmkHbCounter   = 0;
static uint32_t jcmkLastHbMs    = 0;
static uint32_t jcmkLastReqMs   = 0;
static uint32_t jcmkReqInterval = JCMK_REQ_INIT_MS;

// Per-channel async scan state
static bool     nodeScanActive   = false;
static uint8_t  nodeScanChOffset = 0;
static bool     nodeScanAdminWin = false;
static uint32_t nodeScanAdminMs  = 0;

// Pending core-found event — set from ESP-Now callback, consumed in loop
static volatile bool  jcmkCoreFoundPending = false;
static uint8_t        jcmkCoreMacPending[6] = {0};

// ---- Core mode state ----
#define CORE_MAX_NODES 4
struct CoreNodeInfo {
  bool     active;
  uint8_t  mac[6];
  uint8_t  startIdx, endIdx;
  uint32_t lastHbMs;
  uint32_t recordsRx;
  bool     isBiscuit;  // true = Biscuit Node protocol (requires full-size 212-byte packets)
};
static bool         meshCoreActive  = false;
static uint32_t     coreRecordsRx   = 0;
static uint8_t      coreNodeCount   = 0;
static CoreNodeInfo coreNodes[CORE_MAX_NODES] = {};
static uint8_t      coreAssignVer   = 0;
static uint32_t     coreLastHbMs    = 0;
static uint32_t     coreHbCounter   = 0;
static const uint32_t CORE_HB_MS         = 5000;
static const uint32_t CORE_NODE_TIMEOUT  = 45000;  // 45 s — accounts for blocking scan latency

#define CORE_REQ_QUEUE   4
#define CORE_TEXT_QUEUE 64   // large enough for burst from two nodes per cycle
struct CorReqSlot  { uint8_t mac[6]; bool isBiscuit; };
struct CorTextSlot { char    line[JCMK_TEXT_MAX + 1]; };
static CorReqSlot         coreReqBuf[CORE_REQ_QUEUE];
static volatile uint8_t   coreReqHead = 0, coreReqTail = 0;
static CorTextSlot        coreTextBuf[CORE_TEXT_QUEUE];
static volatile uint8_t   coreTextHead = 0, coreTextTail = 0;

// ------------- ESP-Now helpers -----------------------------------
static void jcmkSetChannel(uint8_t ch) {
  esp_wifi_set_ps(WIFI_PS_NONE);
  // Do NOT use promiscuous mode: on IDF 5.x, disabling promiscuous after
  // a prior STA connection causes the driver to revert to the home router channel.
  // Direct esp_wifi_set_channel() works correctly when the STA is not connected.
  esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
}

static bool jcmkAddPeer(const uint8_t* mac) {
  if (esp_now_is_peer_exist(mac)) esp_now_del_peer(mac);
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, mac, 6);
  peer.channel = 0;  // follow current home channel
  peer.encrypt = false;
  return (esp_now_add_peer(&peer) == ESP_OK);
}

// Node-mode sends always use full jcmk_text_msg_t (212 bytes).
// Biscuit Pro drops any packet < 212 bytes; JCMK Cores only check len >= 5
// so full-size packets are backward-compatible with both coordinators.

static void jcmkSendCoreRequest() {
  jcmk_text_msg_t msg = {};
  memcpy(msg.magic, JCMK_MAGIC, 4);
  msg.type    = JCMK_MSG_CORE_REQUEST;
  msg.counter = 0;
  msg.len     = 0;
  esp_now_send(JCMK_BCAST, (uint8_t*)&msg, sizeof(msg));
}

static void jcmkSendHeartbeat() {
  jcmk_text_msg_t msg = {};
  memcpy(msg.magic, JCMK_MAGIC, 4);
  msg.type    = JCMK_MSG_HEARTBEAT;
  msg.counter = ++jcmkHbCounter;
  msg.len     = 0;
  esp_now_send(JCMK_BCAST, (uint8_t*)&msg, sizeof(msg));
}

static void jcmkSendText(const String& s) {
  jcmk_text_msg_t msg = {};
  memcpy(msg.magic, JCMK_MAGIC, 4);
  msg.type    = JCMK_MSG_TEXT;
  msg.counter = jcmkHbCounter;
  uint16_t slen = (uint16_t)((s.length() < JCMK_TEXT_MAX) ? s.length() : JCMK_TEXT_MAX);
  msg.len = slen;
  memcpy(msg.text, s.c_str(), slen);
  msg.text[slen] = '\0';
  // Always send full struct size — Biscuit Pro drops variable-length packets < 212 bytes.
  esp_now_send(JCMK_BCAST, (uint8_t*)&msg, sizeof(msg));
}

// ---- Core forward decls (used inside jcmkOnRecv callback) ----
static void coreSendReply(const uint8_t* mac) {
  // Use full-size jcmk_text_msg_t (212 bytes) instead of jcmk_req_msg_t (5 bytes).
  // Biscuit Node drops any packet < sizeof(enow_text_msg_t) = 212 bytes.
  // JCMK nodes only check len >= 5, so this is fully backward-compatible.
  jcmk_text_msg_t msg = {};
  memcpy(msg.magic, JCMK_MAGIC, 4);
  msg.type    = JCMK_MSG_CORE_REPLY;
  msg.counter = 0;
  msg.len     = 0;
  esp_now_send(mac, (uint8_t*)&msg, sizeof(msg));
}

// Send Biscuit Node a role assignment (MSG_ROLE_ASSIGN, type=5) then a channel
// config (MSG_CONFIG_UPDATE, type=10). Both packets must be full size (212 bytes).
static void coreSendBiscuitRoleAndConfig(const uint8_t* mac, uint8_t startIdx, uint8_t endIdx) {
  // Step 1: role assignment — text[0] = 1 (ROLE_WIFI)
  jcmk_text_msg_t roleMsg = {};
  memcpy(roleMsg.magic, JCMK_MAGIC, 4);
  roleMsg.type    = JCMK_MSG_ADMIN;  // = 5 = MSG_ROLE_ASSIGN on Biscuit
  roleMsg.counter = 0;
  roleMsg.len     = 1;
  roleMsg.text[0] = 1;  // ROLE_WIFI
  esp_now_send(mac, (uint8_t*)&roleMsg, sizeof(roleMsg));
  delay(10);

  // Step 2: channel config — "channels=1,2,...;dwell=80"
  jcmk_text_msg_t cfgMsg = {};
  memcpy(cfgMsg.magic, JCMK_MAGIC, 4);
  cfgMsg.type    = 10;  // MSG_CONFIG_UPDATE
  cfgMsg.counter = 0;

  String chList = "channels=";
  uint8_t end = (endIdx < JCMK_NUM_CHANNELS) ? endIdx : (JCMK_NUM_CHANNELS - 1);
  for (uint8_t j = startIdx; j <= end; j++) {
    if (j > startIdx) chList += ",";
    chList += String(JCMK_CHANNELS[j]);
  }
  chList += ";dwell=";
  chList += String((uint32_t)NODE_SCAN_DWELL_MS);

  uint16_t slen = (chList.length() < JCMK_TEXT_MAX)
                ? (uint16_t)chList.length() : (uint16_t)JCMK_TEXT_MAX;
  cfgMsg.len = slen;
  memcpy(cfgMsg.text, chList.c_str(), slen);
  cfgMsg.text[slen] = '\0';
  esp_now_send(mac, (uint8_t*)&cfgMsg, sizeof(cfgMsg));

  Serial.printf("[CORE] Biscuit role+config sent (ch %d-%d, dwell %dms)\n",
    JCMK_CHANNELS[startIdx],
    JCMK_CHANNELS[end],
    (int)NODE_SCAN_DWELL_MS);
}

// ESP-Now receive callback — handles both Node and Core roles
static void jcmkOnRecv(const esp_now_recv_info_t* info,
                        const uint8_t* data, int len) {
  if (len < 5) return;
  if (data[0] != 'E' || data[1] != 'N' || data[2] != 'O' || data[3] != 'W') return;
  uint8_t type = data[4];

  if (meshCoreActive) {
    if (type == JCMK_MSG_CORE_REQUEST) {
      Serial.printf("[CORE] RX CORE_REQUEST from %02X:%02X:%02X:%02X:%02X:%02X len=%d\n",
        info->src_addr[0],info->src_addr[1],info->src_addr[2],
        info->src_addr[3],info->src_addr[4],info->src_addr[5], len);
      coreSendReply(info->src_addr);
      uint8_t next = (coreReqTail + 1) % CORE_REQ_QUEUE;
      if (next != coreReqHead) {
        memcpy(coreReqBuf[coreReqTail].mac, info->src_addr, 6);
        coreReqBuf[coreReqTail].isBiscuit = (len >= (int)sizeof(jcmk_text_msg_t));
        coreReqTail = next;
      }
    } else if (type == JCMK_MSG_TEXT && len >= 11) {
      const jcmk_text_msg_t* tm = (const jcmk_text_msg_t*)data;
      uint8_t next = (coreTextTail + 1) % CORE_TEXT_QUEUE;
      if (next != coreTextHead) {
        uint16_t slen = (tm->len < JCMK_TEXT_MAX) ? tm->len : JCMK_TEXT_MAX;
        memcpy(coreTextBuf[coreTextTail].line, tm->text, slen);
        coreTextBuf[coreTextTail].line[slen] = '\0';
        coreTextTail = next;
      }
      // Update heartbeat for known node; queue unknown node for registration.
      {
        bool found = false;
        for (uint8_t i = 0; i < CORE_MAX_NODES; i++) {
          if (coreNodes[i].active && memcmp(coreNodes[i].mac, info->src_addr, 6) == 0) {
            coreNodes[i].lastHbMs = millis();
            coreNodes[i].recordsRx++;
            found = true; break;
          }
        }
        if (!found) {
          uint8_t nxt = (coreReqTail + 1) % CORE_REQ_QUEUE;
          if (nxt != coreReqHead) {
            memcpy(coreReqBuf[coreReqTail].mac, info->src_addr, 6);
            coreReqBuf[coreReqTail].isBiscuit = (len >= (int)sizeof(jcmk_text_msg_t));
            coreReqTail = nxt;
          }
        }
      }
    } else if (type == JCMK_MSG_HEARTBEAT) {
      // Update heartbeat for known node; queue unknown node for registration.
      bool found = false;
      for (uint8_t i = 0; i < CORE_MAX_NODES; i++) {
        if (coreNodes[i].active && memcmp(coreNodes[i].mac, info->src_addr, 6) == 0) {
          coreNodes[i].lastHbMs = millis();
          found = true; break;
        }
      }
      if (!found) {
        uint8_t nxt = (coreReqTail + 1) % CORE_REQ_QUEUE;
        if (nxt != coreReqHead) {
          memcpy(coreReqBuf[coreReqTail].mac, info->src_addr, 6);
          coreReqBuf[coreReqTail].isBiscuit = (len >= (int)sizeof(jcmk_text_msg_t));
          coreReqTail = nxt;
        }
      }
    }
  } else {
    if (type == JCMK_MSG_CORE_REPLY && !jcmkHaveCore && !jcmkCoreFoundPending) {
      memcpy(jcmkCoreMacPending, info->src_addr, 6);
      jcmkCoreFoundPending = true;
    } else if (type == JCMK_MSG_ADMIN && len >= (int)sizeof(jcmk_admin_msg_t)) {
      const jcmk_admin_msg_t* adm = (const jcmk_admin_msg_t*)data;
      if (adm->assignment_version != jcmkAssignVer) {
        jcmkAssignVer = adm->assignment_version;
        jcmkStartIdx  = adm->start_channel_idx;
        jcmkEndIdx    = adm->end_channel_idx;
      }
    }
  }
}

// ---- Core mode helpers (main loop only) ----
static void coreFindOrAddNode(const uint8_t* mac, bool isBiscuit) {
  for (uint8_t i = 0; i < CORE_MAX_NODES; i++) {
    if (coreNodes[i].active && memcmp(coreNodes[i].mac, mac, 6) == 0) {
      coreNodes[i].lastHbMs = millis(); return;
    }
  }
  for (uint8_t i = 0; i < CORE_MAX_NODES; i++) {
    if (!coreNodes[i].active) {
      coreNodes[i].active = true; coreNodes[i].lastHbMs = millis();
      coreNodes[i].recordsRx = 0; coreNodes[i].isBiscuit = isBiscuit;
      memcpy(coreNodes[i].mac, mac, 6);
      coreNodeCount++; jcmkAddPeer(mac);
      // Re-send CORE_REPLY now that the peer is registered.
      coreSendReply(mac);
      Serial.printf("[CORE] New %s node %d: %02X:%02X:%02X:%02X:%02X:%02X\n",
        isBiscuit ? "Biscuit" : "JCMK",
        i, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
      return;
    }
  }
}

static void coreReassignChannels() {
  uint8_t slots[CORE_MAX_NODES]; uint8_t count = 0;
  for (uint8_t i = 0; i < CORE_MAX_NODES; i++) if (coreNodes[i].active) slots[count++] = i;
  if (count == 0) return;
  uint8_t perNode = JCMK_NUM_CHANNELS / count, startIdx = 0;
  for (uint8_t n = 0; n < count; n++) {
    coreNodes[slots[n]].startIdx = startIdx;
    coreNodes[slots[n]].endIdx   = (n < count-1) ? (startIdx+perNode-1) : (JCMK_NUM_CHANNELS-1);
    startIdx += perNode;
  }
  coreAssignVer++;
  for (uint8_t n = 0; n < count; n++) {
    uint8_t slot = slots[n];
    if (coreNodes[slot].isBiscuit) {
      coreSendBiscuitRoleAndConfig(coreNodes[slot].mac,
                                   coreNodes[slot].startIdx,
                                   coreNodes[slot].endIdx);
    } else {
      jcmk_admin_msg_t msg; memcpy(msg.magic, JCMK_MAGIC, 4);
      msg.type = JCMK_MSG_ADMIN; msg.node_count = count;
      msg.assignment_version = coreAssignVer;
      msg.node_index = n; msg.start_channel_idx = coreNodes[slot].startIdx;
      msg.end_channel_idx = coreNodes[slot].endIdx;
      esp_now_send(coreNodes[slot].mac, (uint8_t*)&msg, sizeof(msg));
    }
    delay(10);
  }
  Serial.printf("[CORE] Reassigned: %d nodes v%d\n", count, coreAssignVer);
}

// Re-send the current assignment to every node using its protocol.
static void coreResendAdminToAll() {
  if (coreNodeCount == 0) return;
  uint8_t n = 0;
  for (uint8_t i = 0; i < CORE_MAX_NODES; i++) {
    if (!coreNodes[i].active) continue;
    if (coreNodes[i].isBiscuit) {
      coreSendBiscuitRoleAndConfig(coreNodes[i].mac,
                                   coreNodes[i].startIdx,
                                   coreNodes[i].endIdx);
    } else {
      jcmk_admin_msg_t msg; memcpy(msg.magic, JCMK_MAGIC, 4);
      msg.type = JCMK_MSG_ADMIN; msg.node_count = coreNodeCount;
      msg.assignment_version = coreAssignVer;
      msg.node_index = n; msg.start_channel_idx = coreNodes[i].startIdx;
      msg.end_channel_idx = coreNodes[i].endIdx;
      esp_now_send(coreNodes[i].mac, (uint8_t*)&msg, sizeof(msg));
    }
    n++;
    delay(10);
  }
}

static void coreSendHeartbeatToAll() {
  // Use full-size jcmk_text_msg_t (212 bytes) so Biscuit Node accepts it.
  jcmk_text_msg_t msg = {};
  memcpy(msg.magic, JCMK_MAGIC, 4);
  msg.type = JCMK_MSG_HEARTBEAT; msg.counter = ++coreHbCounter; msg.len = 0;
  for (uint8_t i = 0; i < CORE_MAX_NODES; i++)
    if (coreNodes[i].active) esp_now_send(coreNodes[i].mac, (uint8_t*)&msg, sizeof(msg));
}

static void coreParseAndLogText(const char* line) {
  String s(line);
  int p0=s.indexOf(','); if(p0<0)return;
  int p1=s.indexOf(',',p0+1); if(p1<0)return;
  int p2=s.indexOf(',',p1+1); if(p2<0)return;
  int p3=s.indexOf(',',p2+1); if(p3<0)return;
  String bssid=s.substring(0,p0), ssid=s.substring(p0+1,p1);
  String auth=s.substring(p1+1,p2);
  int ch=s.substring(p2+1,p3).toInt(), rssi=s.substring(p3+1).toInt();
  double lat=0, lon=0, altM=0, accM=0;
  if (gpsHasFix) { lat=gps.location.lat(); lon=gps.location.lng();
                   altM=gps.altitude.meters(); accM=gps.hdop.hdop(); }
  digitalWrite(PINS.tft_cs, HIGH);
  appendWigleRow(bssid, ssid, auth, iso8601NowUTC(), ch, rssi, lat, lon, altM, accM);
  coreRecordsRx++;
}

static void enterCoreMode() {
  Serial.println("[CORE] Entering Core mode");
  meshCoreActive=false; coreRecordsRx=0; coreNodeCount=0;
  coreAssignVer=0; coreHbCounter=0; coreLastHbMs=0;
  coreReqHead=coreReqTail=0; coreTextHead=coreTextTail=0;
  memset(coreNodes, 0, sizeof(coreNodes));
  // Mesh mode owns the WiFi stack — prevent stopAPIfAllowed() from firing
  // WiFi.disconnect(true,true) after esp_now_init() would kill the ESP-Now driver.
  apWindowActive = false;
  // Full WiFi deinit → reinit: clears previous AP channel/state from driver.
  WiFi.softAPdisconnect(true);
  WiFi.setAutoReconnect(false); WiFi.persistent(false);
  WiFi.mode(WIFI_OFF);  // full deinit — clears previous AP channel state
  delay(200);
  WiFi.mode(WIFI_STA);  // clean reinit
  delay(200);
  esp_err_t err=esp_now_init();
  if (err!=ESP_OK) { Serial.printf("[CORE] init failed: %d\n",(int)err); return; }
  esp_now_register_recv_cb(jcmkOnRecv);
  delay(50); jcmkSetChannel(JCMK_ESPNOW_CH);
  { uint8_t pri; wifi_second_chan_t sec; esp_wifi_get_channel(&pri, &sec);
    Serial.printf("[CORE] Channel after set: %d (target=%d)%s\n", pri, JCMK_ESPNOW_CH,
      (pri==JCMK_ESPNOW_CH)?" OK":" *** MISMATCH!");
    if (pri!=JCMK_ESPNOW_CH){delay(50);jcmkSetChannel(JCMK_ESPNOW_CH);
      esp_wifi_get_channel(&pri,&sec);
      Serial.printf("[CORE] Channel after retry: %d\n",pri);} }
  jcmkAddPeer(JCMK_BCAST);
  meshCoreActive=true;
  pageNeedsInit[4]=true;  // force TFT header redraw for Core mode
  Serial.println("[CORE] Ready — listening for nodes on ch 6");
}

static void exitCoreMode() {
  Serial.println("[CORE] Exiting Core mode");
  if (meshCoreActive) esp_now_deinit();
  meshCoreActive=false; coreNodeCount=0;
  WiFi.mode(WIFI_OFF); delay(150);
  WiFi.setAutoReconnect(true);  // restore auto-reconnect for normal wardriving
  WiFi.mode(WIFI_STA); delay(100);
  while (GPSSerial.available()) gps.encode(GPSSerial.read());
  scanningEnabled=true;
  pageNeedsInit[4]=true;  // force TFT header redraw for Node mode
}

static void coreModeTick() {
  if (!meshCoreActive) return;
  uint32_t now=millis();
  while (coreReqHead!=coreReqTail) {
    uint8_t i=coreReqHead; coreReqHead=(coreReqHead+1)%CORE_REQ_QUEUE;
    coreFindOrAddNode(coreReqBuf[i].mac, coreReqBuf[i].isBiscuit); coreReassignChannels();
  }
  while (coreTextHead!=coreTextTail) {
    uint8_t i=coreTextHead; coreTextHead=(coreTextHead+1)%CORE_TEXT_QUEUE;
    coreParseAndLogText(coreTextBuf[i].line);
  }
  // Heartbeat + ADMIN refresh — nodes that missed the ADMIN while scanning recover here.
  if (now-coreLastHbMs>=CORE_HB_MS) { coreLastHbMs=now; coreSendHeartbeatToAll(); coreResendAdminToAll(); }
  bool changed=false;
  for (uint8_t i=0; i<CORE_MAX_NODES; i++) {
    if (coreNodes[i].active && (now-coreNodes[i].lastHbMs)>CORE_NODE_TIMEOUT) {
      Serial.printf("[CORE] Node %d timed out\n",i);
      esp_now_del_peer(coreNodes[i].mac);
      memset(&coreNodes[i], 0, sizeof(CoreNodeInfo));
      coreNodeCount--; changed=true;
    }
  }
  if (changed) coreReassignChannels();
}

// ================================================================
//  Per-channel async scan (JCMK startNextNodeAssignedScan pattern).
//  Scans one assigned channel per call at NODE_SCAN_DWELL_MS dwell,
//  cycles through the full assigned range, then enters a ch-6 admin
//  window (heartbeat + NODE_ADMIN_WIN_MS) before the next cycle.
// ================================================================
static void nodeDoScanTick() {
  uint8_t numCh = (jcmkEndIdx >= jcmkStartIdx)
                ? (jcmkEndIdx - jcmkStartIdx + 1) : 0;
  if (numCh == 0) return;

  // Admin window: radio is on ch 6, heartbeat already sent, just waiting
  if (nodeScanAdminWin) {
    if (millis() - nodeScanAdminMs >= NODE_ADMIN_WIN_MS) {
      nodeScanAdminWin = false;
      nodeScanChOffset = 0;  // begin next cycle
    }
    return;
  }

  if (!nodeScanActive) {
    // Full cycle complete — enter admin window
    if (nodeScanChOffset >= numCh) {
      jcmkSetChannel(JCMK_ESPNOW_CH);
      if (jcmkHaveCore) { jcmkSendHeartbeat(); jcmkLastHbMs = millis(); }
      nodeScanAdminWin = true;
      nodeScanAdminMs  = millis();
      return;
    }

    uint8_t chIdx = jcmkStartIdx + nodeScanChOffset;
    if (chIdx >= JCMK_NUM_CHANNELS) { nodeScanChOffset++; return; }
    uint8_t channel = JCMK_CHANNELS[chIdx];

    // Async scan of this single channel only (no blocking)
    int16_t rc = WiFi.scanNetworks(/*async*/true, /*hidden*/true,
                                    /*passive*/false, NODE_SCAN_DWELL_MS, channel);
    if (rc == WIFI_SCAN_RUNNING || rc == 0) {
      nodeScanActive = true;
    } else {
      nodeScanChOffset++;  // skip failed channel
    }
    return;
  }

  // Scan in progress — poll for completion
  int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) return;

  if (n > 0) {
    jcmkNetworksFound += (uint32_t)n;
    // Return to ch 6 before any ESP-Now send (JCMK sendBroadcastStringPlain pattern).
    // The scan left the radio on the scan channel; Core listens only on ch 6.
    jcmkSetChannel(JCMK_ESPNOW_CH);
    static WifiSeenRing gFwdSeen;  // node-side: forward each BSSID once
    for (int i = 0; i < n; i++) {
      String bssid = WiFi.BSSIDstr(i);
      if (!gFwdSeen.firstTime(bssid)) continue;  // already forwarded
      String ssid  = WiFi.SSID(i);
      String auth  = authModeToString(WiFi.encryptionType(i));
      int    ch    = WiFi.channel(i);
      int    rssi  = WiFi.RSSI(i);
      String line  = bssid + "," + ssid + "," + auth + ","
                   + String(ch) + "," + String(rssi) + ",W";
      jcmkSendText(line);
      jcmkSentCount++;
    }
    ledPulseGreen();
  }
  WiFi.scanDelete();
  nodeScanActive = false;
  nodeScanChOffset++;
}

// Enter mesh node mode — call on page 4 entry
static void enterNodeMode() {
  Serial.println("[MESH] Entering node mode");
  meshNodeActive        = false;
  jcmkHaveCore          = false;
  jcmkCoreFoundPending  = false;
  jcmkNetworksFound     = 0;
  jcmkSentCount         = 0;
  jcmkHbCounter         = 0;
  jcmkLastHbMs          = 0;
  jcmkLastReqMs         = 0;
  jcmkReqInterval       = JCMK_REQ_INIT_MS;
  jcmkStartIdx          = 0;
  jcmkEndIdx            = JCMK_NUM_CHANNELS - 1;
  jcmkAssignVer         = 0;
  nodeScanActive        = false;
  nodeScanChOffset      = 0;
  nodeScanAdminWin      = false;

  // Mesh mode owns the WiFi stack — prevent stopAPIfAllowed() from firing
  // WiFi.disconnect(true,true) after esp_now_init() would kill the ESP-Now driver.
  apWindowActive = false;

  // Full WiFi deinit → reinit: clears previous AP channel/state from driver.
  WiFi.softAPdisconnect(true);
  WiFi.setAutoReconnect(false);
  WiFi.persistent(false);
  WiFi.mode(WIFI_OFF);  // full deinit — clears previous AP channel state
  delay(200);
  WiFi.mode(WIFI_STA);  // clean reinit
  delay(200);

  // Init ESP-Now FIRST, then lock the home channel.
  esp_err_t err = esp_now_init();
  if (err != ESP_OK) {
    Serial.printf("[MESH] esp_now_init failed: %d\n", (int)err);
    return;
  }
  esp_now_register_recv_cb(jcmkOnRecv);

  delay(50);
  jcmkSetChannel(JCMK_ESPNOW_CH);
  { uint8_t pri; wifi_second_chan_t sec; esp_wifi_get_channel(&pri, &sec);
    Serial.printf("[MESH] Channel after set: %d (target=%d)%s\n", pri, JCMK_ESPNOW_CH,
      (pri==JCMK_ESPNOW_CH)?" OK":" *** MISMATCH!");
    if (pri!=JCMK_ESPNOW_CH){delay(50);jcmkSetChannel(JCMK_ESPNOW_CH);
      esp_wifi_get_channel(&pri,&sec);
      Serial.printf("[MESH] Channel after retry: %d\n",pri);} }

  jcmkAddPeer(JCMK_BCAST);

  // Random stagger before first scan (JCMK begin() pattern: delay(random(100,5000))).
  // Prevents multiple nodes starting cycles in sync and flooding simultaneously.
  delay(random(200, 3000));

  meshNodeActive = true;
  Serial.println("[MESH] ESP-Now ready — searching for Core on ch 6...");
}

// Exit mesh node mode — call when leaving page 4
static void exitNodeMode() {
  Serial.println("[MESH] Exiting node mode");
  if (meshNodeActive) esp_now_deinit();
  meshNodeActive = false;
  jcmkHaveCore   = false;

  // Full WiFi stack reset: OFF then STA gives a clean state after esp_now_deinit
  WiFi.mode(WIFI_OFF);
  delay(150);
  WiFi.setAutoReconnect(true);  // restore auto-reconnect for normal wardriving
  WiFi.mode(WIFI_STA);
  delay(100);

  // Drain GPS serial buffer that accumulated during mesh mode scans
  while (GPSSerial.available()) gps.encode(GPSSerial.read());

  // Re-enable scanning so the status page resumes immediately
  scanningEnabled = true;
}

// Called every loop() iteration while page 4 is active
static void nodeModeTick() {
  if (!meshNodeActive) return;
  uint32_t now = millis();

  // Consume pending core-found event from the ESP-Now callback
  if (jcmkCoreFoundPending) {
    jcmkCoreFoundPending = false;
    memcpy(jcmkCoreMac, jcmkCoreMacPending, 6);
    jcmkHaveCore    = true;
    jcmkReqInterval = JCMK_REQ_INIT_MS;
    jcmkAddPeer(jcmkCoreMac);
    Serial.printf("[MESH] Core: %02X:%02X:%02X:%02X:%02X:%02X\n",
      jcmkCoreMac[0], jcmkCoreMac[1], jcmkCoreMac[2],
      jcmkCoreMac[3], jcmkCoreMac[4], jcmkCoreMac[5]);
  }

  // CORE_REQUEST with backoff (only while radio is free)
  if (!jcmkHaveCore && !nodeScanActive && (now - jcmkLastReqMs >= jcmkReqInterval)) {
    jcmkLastReqMs  = now;
    jcmkSetChannel(JCMK_ESPNOW_CH);
    jcmkSendCoreRequest();
    jcmkReqInterval = (jcmkReqInterval * 2 > JCMK_REQ_MAX_MS)
                      ? JCMK_REQ_MAX_MS : jcmkReqInterval * 2;
  }

  // Heartbeat backup timer (cycle-end heartbeat is primary; fires if scan stalls)
  if (jcmkHaveCore && !nodeScanActive && (now - jcmkLastHbMs >= JCMK_HB_MS)) {
    jcmkLastHbMs = now;
    jcmkSetChannel(JCMK_ESPNOW_CH);
    jcmkSendHeartbeat();
  }

  // Per-channel async scan — runs continuously while connected to Core
  if (jcmkHaveCore) nodeDoScanTick();
}

// ================================================================
//  Page 4: handles both Mesh Node and Core mode
// ================================================================
static void drawPageMeshNode() {
  int W = tft.width();

  if (meshCoreActive) {
    // ---- Core mode ----
    if (pageNeedsInit[4]) {
      pageNeedsInit[4] = false;
      tft.fillScreen(BLACK);
      tft.setTextColor(WHITE, BLACK); tft.setTextSize(2);
      tftDrawCentered(2, "CORE", 2);
      tft.drawFastHLine(0, 20, W, WHITE);
    }
    tft.setTextColor(WHITE, BLACK); tft.setTextSize(1);

    // Node count (y=24)
    tft.fillRect(0, 24, W, 10, BLACK); tft.setCursor(2, 25);
    if (coreNodeCount == 0) {
      tft.setTextColor(COLOR_YELLOW, BLACK); tft.print("Waiting for nodes");
    } else {
      tft.setTextColor(COLOR_GREEN, BLACK);
      char buf[20]; snprintf(buf, sizeof(buf), "%d node%s",
        coreNodeCount, coreNodeCount==1?" active":"s active"); tft.print(buf);
    }
    tft.setTextColor(WHITE, BLACK);

    // Node slots (y=36, y=58) — show first 2
    uint8_t shown = 0;
    for (uint8_t i = 0; i < CORE_MAX_NODES && shown < 2; i++) {
      int y = 36 + (int)shown * 22;
      tft.fillRect(0, y, W, 20, BLACK);
      tft.setCursor(2, y + 1);
      if (coreNodes[i].active) {
        uint8_t* m = coreNodes[i].mac;
        uint8_t si = coreNodes[i].startIdx, ei = coreNodes[i].endIdx;
        char buf[20];
        snprintf(buf, sizeof(buf), "%02X%02X%02X %d-%d",
          m[3], m[4], m[5],
          (si<JCMK_NUM_CHANNELS)?JCMK_CHANNELS[si]:0,
          (ei<JCMK_NUM_CHANNELS)?JCMK_CHANNELS[ei]:0);
        tft.setTextColor(COLOR_GREEN, BLACK); tft.print(buf);
        tft.setTextColor(WHITE, BLACK);
        tft.setCursor(2, y + 11);
        char rbuf[14]; snprintf(rbuf, sizeof(rbuf), "  rx:%lu", (unsigned long)coreNodes[i].recordsRx);
        tft.print(rbuf);
        shown++;
      }
    }
    // Fill empty slots
    for (; shown < 2; shown++) {
      int y = 36 + (int)shown * 22;
      tft.fillRect(0, y, W, 20, BLACK);
      tft.setCursor(2, y + 1);
      tft.setTextColor(0x4208, BLACK); tft.print("-- no node --");  // dim gray
      tft.setTextColor(WHITE, BLACK);
    }

    // Records total (y=82)
    tft.fillRect(0, 82, W, 10, BLACK); tft.setCursor(2, 83);
    char tbuf[20]; snprintf(tbuf, sizeof(tbuf), "Rcvd: %lu", (unsigned long)coreRecordsRx);
    tft.print(tbuf);

    // GPS (y=96)
    tft.fillRect(0, 96, W, 10, BLACK); tft.setCursor(2, 97);
    tft.setTextColor(gpsHasFix ? COLOR_GREEN : COLOR_YELLOW, BLACK);
    tft.print(gpsHasFix ? "GPS: FIX" : "GPS: ---");
    tft.setTextColor(WHITE, BLACK);

    // Hint (y=110)
    tft.fillRect(0, 110, W, 10, BLACK); tft.setCursor(2, 111);
    tft.setTextColor(0x4208, BLACK); tft.print("Hold btn = Node mode");
    tft.setTextColor(WHITE, BLACK);

  } else {
    // ---- Node mode (original layout) ----
    if (pageNeedsInit[4]) {
      pageNeedsInit[4] = false;
      tft.fillScreen(BLACK);
      tft.setTextColor(WHITE, BLACK); tft.setTextSize(2);
      tftDrawCentered(2, "Mesh Node", 2);
      tft.drawFastHLine(0, 20, W, WHITE);
    }
    tft.setTextColor(WHITE, BLACK); tft.setTextSize(1);

    tft.fillRect(0, 24, W, 12, BLACK); tft.setCursor(2, 26);
    if (!meshNodeActive) {
      tft.setTextColor(COLOR_RED, BLACK); tft.print("Init error");
    } else if (!jcmkHaveCore) {
      tft.setTextColor(COLOR_YELLOW, BLACK); tft.print("Searching...");
    } else {
      tft.setTextColor(COLOR_GREEN, BLACK); tft.print("Core linked");
    }
    tft.setTextColor(WHITE, BLACK);

    tft.fillRect(0, 38, W, 10, BLACK); tft.setCursor(2, 39);
    if (jcmkHaveCore) {
      char macStr[18];
      snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
        jcmkCoreMac[0], jcmkCoreMac[1], jcmkCoreMac[2],
        jcmkCoreMac[3], jcmkCoreMac[4], jcmkCoreMac[5]);
      tft.print(macStr);
    } else { tft.print("--:--:--:--:--:--"); }

    tft.fillRect(0, 51, W, 10, BLACK); tft.setCursor(2, 52);
    tft.print("Ch: ");
    if (jcmkAssignVer>0 && jcmkStartIdx<JCMK_NUM_CHANNELS && jcmkEndIdx<JCMK_NUM_CHANNELS) {
      tft.print(JCMK_CHANNELS[jcmkStartIdx]); tft.print("-"); tft.print(JCMK_CHANNELS[jcmkEndIdx]);
    } else { tft.print("all"); }

    tft.fillRect(0, 64, W, 10, BLACK); tft.setCursor(2, 65);
    tft.print("Found: "); tft.print(jcmkNetworksFound);

    tft.fillRect(0, 77, W, 10, BLACK); tft.setCursor(2, 78);
    tft.print("Sent: "); tft.print(jcmkSentCount);

    tft.fillRect(0, 90, W, 10, BLACK); tft.setCursor(2, 91);
    tft.print("ENOW ch: "); tft.print(JCMK_ESPNOW_CH);

    // Hold hint
    tft.fillRect(0, 104, W, 10, BLACK); tft.setCursor(2, 105);
    tft.setTextColor(0x4208, BLACK); tft.print("Hold btn = Core mode");
    tft.setTextColor(WHITE, BLACK);
  }
}

// ---------------- SD init (shared SPI) ----------------
static bool initSD_SharedSPI() {
  pinMode(PINS.sd_cs, OUTPUT);  digitalWrite(PINS.sd_cs, HIGH);
  pinMode(PINS.tft_cs, OUTPUT); digitalWrite(PINS.tft_cs, HIGH);
  pinMode(PINS.tft_rst, OUTPUT); digitalWrite(PINS.tft_rst, LOW);
  pinMode(PINS.tft_dc, OUTPUT); digitalWrite(PINS.tft_dc, HIGH);
  pinMode(PINS.sd_miso, INPUT_PULLUP);
  delay(20);

  SPI.end(); delay(10);
  SPI.begin(PINS.sd_sck, PINS.sd_miso, PINS.sd_mosi, PINS.sd_cs);
  delay(20);

  // SD spec requires ≥74 clock pulses with CS HIGH before the first command.
  // Also resets any card stuck in a partial-command state from a prior boot.
  {
    SPI.beginTransaction(SPISettings(100000, MSBFIRST, SPI_MODE0));
    for (int i = 0; i < 10; i++) SPI.transfer(0xFF);  // 10 bytes = 80 clocks
    SPI.endTransaction();
  }
  delay(5);

  digitalWrite(PINS.sd_cs, HIGH);
  digitalWrite(PINS.tft_cs, HIGH);
  delay(100);

  const uint32_t freqs[] = { 100000, 200000, 400000, 1000000, 2000000, 4000000, 8000000 };
  for (size_t i = 0; i < sizeof(freqs)/sizeof(freqs[0]); i++) {
    SD.end(); delay(20);
    Serial.printf("[SD] SD.begin(CS=%d) @ %lu Hz ... ", PINS.sd_cs, (unsigned long)freqs[i]);
    bool ok = SD.begin(PINS.sd_cs, SPI, freqs[i]);
    Serial.println(ok ? "OK" : "FAIL");

    if (ok) {
      uint8_t type = SD.cardType();
      if (type == CARD_NONE) { SD.end(); continue; }
      Serial.printf("[SD] cardType=%u, size=%llu MB\n", type, SD.cardSize() / (1024ULL * 1024ULL));
      if (!SD.exists("/logs")) SD.mkdir("/logs");
      if (!SD.exists("/uploaded")) SD.mkdir("/uploaded");
      return true;
    }
  }
  Serial.println("[SD] All attempts failed.");
  return false;
}

// ---------------- Web UI (in separate header to avoid Arduino ctags bug) ----------------
#include "WebUI_HTML.h"

// ---------------- Web Handlers ----------------
static void handleRoot() { server.sendHeader("Cache-Control", "no-store"); server.send_P(200, "text/html", INDEX_HTML); }

static void handleStatus() {
  DynamicJsonDocument doc(2048);  // heap, not stack — avoids task stack overflow
  bool allowScan = scanningEnabled && sdOk && (userScanOverride || !autoPaused);
  doc["scanningEnabled"] = scanningEnabled;
  doc["allowScan"] = allowScan;
  doc["sdOk"] = sdOk;
  doc["gpsFix"] = gpsHasFix;
  doc["found2g"] = networksFound2G;
  doc["found5g"] = networksFound5G;
  doc["wifiConnected"] = (WiFi.status() == WL_CONNECTED);
  doc["staIp"] = (WiFi.status() == WL_CONNECTED) ? WiFi.localIP().toString() : "";
  doc["apClientsSeen"] = apClientSeen;

  // AP timer fields for the WebUI keep-alive modal
  doc["apActive"]   = apWindowActive;
  doc["apExtended"] = apExtended;
  if (apWindowActive) {
    uint32_t elapsed, budget;
    if (apExtended) {
      elapsed = millis() - apExtendedStartMs;
      budget  = AP_EXTENDED_WINDOW_MS;
    } else {
      elapsed = millis() - apStartMs;
      budget  = AP_WINDOW_MS;
    }
    doc["apRemainingMs"] = (elapsed >= budget) ? 0 : (budget - elapsed);
  } else {
    doc["apRemainingMs"] = 0;
  }
  doc["apExtendPromptLeadMs"] = AP_EXTEND_PROMPT_LEAD_MS;
  doc["uploading"] = uploading;
  doc["uploadTotalFiles"] = uploadTotalFiles;
  doc["uploadDoneFiles"] = uploadDoneFiles;
  doc["uploadLastResult"] = uploadLastResult;
  doc["uploadFailedFiles"] = uploadFailedFiles;
  doc["wigleTokenStatus"] = wigleTokenStatus;
  doc["wigleLastHttpCode"] = wigleLastHttpCode;

  JsonObject c = doc.createNestedObject("config");
  c["wigleBasicToken"] = cfg.wigleBasicToken;
  c["wdgwarsApiKey"]   = cfg.wdgwarsApiKey;
  c["homeSsid"] = cfg.homeSsid;
  c["homePsk"] = cfg.homePsk.length() ? "(set)" : "";
  c["wardriverSsid"] = cfg.wardriverSsid;
  c["wardriverPsk"] = cfg.wardriverPsk;
  c["gpsBaud"] = cfg.gpsBaud;
  c["scanMode"] = cfg.scanMode;
  c["speedUnits"] = cfg.speedUnits;
  c["maxBootUploads"] = cfg.maxBootUploads;
  c["deviceName"]     = cfg.deviceName;
  c["meshModeOnBoot"] = cfg.meshModeOnBoot;
  c["rotateScreen180"] = cfg.rotateScreen180;

  String out; serializeJson(doc, out);
  server.send(200, "application/json", out);
}

static void addDirFiles(JsonArray arr, const char* dir) {
  digitalWrite(PINS.tft_cs, HIGH);
  File root = SD.open(dir);
  if (!root) return;
  File f = root.openNextFile();
  while (f) {
    String fullPath = normalizeSdPath(dir, f.name());
    if (fullPath.length() > 0) {
      JsonObject o = arr.createNestedObject();
      o["name"] = fullPath;
      o["size"] = (uint32_t)f.size();
    }
    f.close(); f = root.openNextFile();
  }
  root.close();
}

static void handleFiles() {
  DynamicJsonDocument doc(4096);  // heap, not stack — 4KB static would overflow 8KB task stack
  doc["ok"] = sdOk;
  JsonArray arr = doc.createNestedArray("files");
  if (sdOk) { addDirFiles(arr, "/logs"); addDirFiles(arr, "/uploaded"); }
  String out; serializeJson(doc, out);
  server.send(200, "application/json", out);
}

static bool isAllowedDataPath(const String& p) { return p.startsWith("/logs/") || p.startsWith("/uploaded/"); }

static void handleDownload() {
  if (!sdOk) { server.send(500, "text/plain", "SD not available"); return; }
  if (!server.hasArg("name")) { server.send(400, "text/plain", "Missing name"); return; }
  String name = server.arg("name");
  if (!isAllowedDataPath(name)) { server.send(403, "text/plain", "Forbidden"); return; }
  digitalWrite(PINS.tft_cs, HIGH);
  if (!SD.exists(name)) { server.send(404, "text/plain", "Not found"); return; }
  File f = SD.open(name, FILE_READ);
  server.streamFile(f, "text/csv"); f.close();
}

static void handleDelete() {
  if (!sdOk) { server.send(500, "text/plain", "SD not available"); return; }
  if (!server.hasArg("name")) { server.send(400, "text/plain", "Missing name"); return; }
  String name = server.arg("name");
  if (!isAllowedDataPath(name)) { server.send(403, "text/plain", "Forbidden"); return; }
  digitalWrite(PINS.tft_cs, HIGH);
  bool ok = SD.remove(name);
  server.send(ok ? 200 : 500, "text/plain", ok ? "OK" : "FAIL");
}

static void handleDeleteAll() {
  if (!sdOk) { server.send(500, "application/json", "{\"ok\":false,\"error\":\"SD not available\"}"); return; }

  uint32_t deleted = 0;
  const char* dirs[] = { "/logs", "/uploaded" };
  for (const char* dir : dirs) {
    digitalWrite(PINS.tft_cs, HIGH);
    File root = SD.open(dir);
    if (!root) continue;
    std::vector<String> paths;
    File f = root.openNextFile();
    while (f) {
      String p = normalizeSdPath(dir, f.name());
      f.close();
      if (p.length() > 0) paths.push_back(p);
      f = root.openNextFile();
    }
    root.close();
    for (const String& p : paths) {
      if (currentCsvPath.length() > 0 && p == currentCsvPath) continue;
      digitalWrite(PINS.tft_cs, HIGH);
      if (SD.remove(p)) deleted++;
    }
  }

  DynamicJsonDocument doc(128);
  doc["ok"] = true;
  doc["deleted"] = deleted;
  String out; serializeJson(doc, out);
  server.send(200, "application/json", out);
}

static void handleStart() {
  scanningEnabled = true; userScanOverride = true;
  if (apWindowActive) { Serial.println("[WEB] /start -> force-close AP"); apForceClose = true; }
  server.send(200, "text/plain", "OK");
}

static void handleExtend() {
  if (!apWindowActive) { server.send(409, "text/plain", "AP not active"); return; }
  apExtended = true;
  apExtendedStartMs = millis();
  Serial.println("[WEB] /extend -> AP extended window reset");
  server.send(200, "text/plain", "OK");
}
static void handleStop()     { scanningEnabled = false; userScanOverride = true; server.send(200, "text/plain", "OK"); }
static void handleNextPage() { advancePage(); server.send(200, "text/plain", String(currentPage)); }

static void handleSaveConfig() {
  if (!sdOk) { server.send(500, "text/plain", "SD not available"); return; }
  String body = server.arg("plain"); body.trim();
  if (body.length() == 0) { server.send(400, "text/plain", "Empty body"); return; }
  bool any = false;
  int pos = 0;
  while (pos < (int)body.length()) {
    int nl = body.indexOf('\n', pos); if (nl < 0) nl = body.length();
    String line = body.substring(pos, nl); pos = nl + 1;
    String k, v;
    if (parseKeyValueLine(line, k, v)) { cfgAssignKV(k, v); any = true; }
  }
  if (!any) { server.send(400, "text/plain", "No valid lines"); return; }
  bool ok = saveConfigToSD();
  server.send(ok ? 200 : 500, "text/plain", ok ? "OK" : "FAIL");
}

static void handleWigleTest() {
  bool ok = wigleTestToken();
  DynamicJsonDocument doc(384);
  doc["ok"] = ok; doc["tokenStatus"] = wigleTokenStatus;
  doc["httpCode"] = wigleLastHttpCode; doc["message"] = uploadLastResult;
  String out; serializeJson(doc, out);
  server.send(ok ? 200 : 400, "application/json", out);
}

static void handleWdgwarsTest() {
  if (WiFi.status() != WL_CONNECTED) { server.send(400,"application/json","{\"ok\":false,\"message\":\"STA WiFi not connected\"}"); return; }
  if (cfg.wdgwarsApiKey.length() < 8)  { server.send(400,"application/json","{\"ok\":false,\"message\":\"No API key configured\"}"); return; }
  bool ok = wdgwarsTestKey();
  DynamicJsonDocument doc(256);
  doc["ok"] = ok; doc["message"] = uploadLastResult;
  String out; serializeJson(doc, out);
  server.send(ok ? 200 : 400, "application/json", out);
}

static void handleWdgwarsUploadAll() {
  if (!sdOk) { server.send(500,"text/plain","SD not available"); return; }
  if (WiFi.status() != WL_CONNECTED) { server.send(400,"text/plain","Not connected"); return; }
  if (cfg.wdgwarsApiKey.length() < 8)  { server.send(400,"text/plain","No API key configured"); return; }
  uint32_t okCount = uploadAllCsvsToWdgwars();
  DynamicJsonDocument doc(256);
  doc["ok"]=(okCount>0); doc["uploaded"]=okCount; doc["total"]=uploadTotalFiles; doc["message"]=uploadLastResult;
  String out; serializeJson(doc, out);
  server.send(200, "application/json", out);
}

static void handleCleanup() {
  if (!sdOk) { server.send(500, "text/plain", "SD not available"); return; }
  // Scan /logs and delete header-only CSV files
  if (SD.exists("/logs")) {
    File root = SD.open("/logs");
    std::vector<String> toDelete;
    if (root) {
      File f = root.openNextFile();
      while (f) {
        String path = normalizeSdPath("/logs", f.name());
        bool isCsv     = path.endsWith(".csv");
        bool isCurrent = (currentCsvPath.length() && path == currentCsvPath);
        f.close();
        if (isCsv && !isCurrent) toDelete.push_back(path);
        f = root.openNextFile();
      }
      root.close();
    }
    for (const String& path : toDelete) {
      if (!csvHasDataRows(path)) {
        Serial.printf("[CLEANUP] Deleting empty CSV: %s\n", pathBasename(path).c_str());
        SD.remove(path);
      }
    }
  }
  server.send(200, "text/plain", "OK");
}

static void handleReboot() {
  closeLogFile();                       // flush active CSV log cleanly
  server.send(200, "text/plain", "OK");
  server.client().stop();
  delay(200);
  ESP.restart();
}

static void handleWigleUploadAll() {
  if (!sdOk) { server.send(500, "text/plain", "SD not available"); return; }
  if (WiFi.status() != WL_CONNECTED) { server.send(400, "text/plain", "Not connected"); return; }
  uint32_t okCount = uploadAllCsvsToWigle(-1);  // web always uploads all
  DynamicJsonDocument doc(384);
  doc["ok"] = (okCount > 0); doc["uploaded"] = okCount;
  doc["total"] = uploadTotalFiles; doc["message"] = uploadLastResult;
  String out; serializeJson(doc, out);
  server.send(200, "application/json", out);
}

static void handleWigleUploadOne() {
  if (!sdOk) { server.send(500, "text/plain", "SD not available"); return; }
  if (WiFi.status() != WL_CONNECTED) { server.send(400, "text/plain", "Not connected"); return; }
  if (!server.hasArg("name")) { server.send(400, "text/plain", "Missing name"); return; }
  String path = server.arg("name");
  digitalWrite(PINS.tft_cs, HIGH);
  if (!SD.exists(path)) { server.send(404, "text/plain", "Not found"); return; }

  uploading = true; uploadPausedScanWasEnabled = scanningEnabled; scanningEnabled = false;
  uploadTotalFiles = 1; uploadDoneFiles = 0; uploadCurrentFile = path;
  ledBlue();

  bool ok = uploadFileToWigle(path);
  uploadDoneFiles = 1; uploading = false;
  scanningEnabled = uploadPausedScanWasEnabled; uploadCurrentFile = "";
  ledOff(); forceStatusFullRedraw();
  if (ok) moveToUploaded(path);

  DynamicJsonDocument doc(384);
  doc["ok"] = ok; doc["httpCode"] = wigleLastHttpCode; doc["message"] = uploadLastResult;
  String out; serializeJson(doc, out);
  server.send(ok ? 200 : 500, "application/json", out);
}

// Streams all CSVs from /logs and /uploaded merged into one RFC 1952 gzip file,
// using DEFLATE stored blocks (BTYPE=00 — no compression library required).
// Non-first files have their 2-line WiGLE header stripped so the merged result
// is one valid WiGLE CSV uploadable directly to WiGLE.net.
static void handleDownloadAll() {
  if (!sdOk) { server.send(500, "text/plain", "SD not available"); return; }

  std::vector<String> paths;
  const char* dirs[] = { "/logs", "/uploaded" };
  for (const char* d : dirs) {
    digitalWrite(PINS.tft_cs, HIGH);
    File root = SD.open(d); if (!root) continue;
    File f = root.openNextFile();
    while (f) {
      // Skip subdirectories and non-CSV files
      if (!f.isDirectory()) {
        String p = normalizeSdPath(d, f.name());
        if (p.endsWith(".csv") && !(currentCsvPath.length() && p == currentCsvPath))
          paths.push_back(p);
      }
      f.close(); f = root.openNextFile();
    }
    root.close();
  }

  Serial.printf("[DOWNLOAD] Collected %u CSV file(s) for gzip\n", (unsigned)paths.size());
  if (paths.empty()) { server.send(200, "text/plain", "No CSV files to download"); return; }

  server.sendHeader("Content-Disposition", "attachment; filename=\"piglet_logs.csv.gz\"");
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "application/gzip", "");
  WiFiClient cl = server.client();

  auto writeChunk = [&](const uint8_t* d, size_t n) {
    if (!n) return;
    char hdr[10]; snprintf(hdr, sizeof(hdr), "%X\r\n", (unsigned)n);
    cl.write((const uint8_t*)hdr, strlen(hdr));
    cl.write(d, n);
    cl.write((const uint8_t*)"\r\n", 2);
  };

  auto crc32Up = [](uint32_t crc, const uint8_t* b, size_t n) -> uint32_t {
    static const uint32_t T[16] = {
      0x00000000,0x1DB71064,0x3B6E20C8,0x26D930AC,
      0x76DC4190,0x6B6B51F4,0x4DB26158,0x5005713C,
      0xEDB88320,0xF00F9344,0xD6D6A3E8,0xCB61B38C,
      0x9B64C2B0,0x86D3D2D4,0xA00AE278,0xBDBDF21C
    };
    crc = ~crc;
    for (size_t i = 0; i < n; i++) {
      crc = (crc >> 4) ^ T[(crc ^ b[i]) & 0xF];
      crc = (crc >> 4) ^ T[(crc ^ (b[i] >> 4)) & 0xF];
    }
    return ~crc;
  };

  const uint8_t gzHdr[10] = {
    0x1f, 0x8b, 0x08, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0xff
  };
  writeChunk(gzHdr, 10);

  uint32_t crc = 0, isize = 0;
  uint8_t  buf[512];

  bool first = true;
  for (const String& path : paths) {
    digitalWrite(PINS.tft_cs, HIGH);
    File f = SD.open(path, FILE_READ); if (!f) continue;
    if (!first) {
      if (f.available()) f.readStringUntil('\n');
      if (f.available()) f.readStringUntil('\n');
    }
    first = false;
    for (;;) {
      int n = f.read(buf, sizeof(buf)); if (n <= 0) break;
      uint16_t ln = (uint16_t)n;
      uint8_t blk[5] = {
        0x00,
        (uint8_t)(ln & 0xFF),   (uint8_t)(ln >> 8),
        (uint8_t)((~ln) & 0xFF),(uint8_t)((~ln) >> 8)
      };
      writeChunk(blk, 5);
      writeChunk(buf, (size_t)n);
      crc = crc32Up(crc, buf, (size_t)n);
      isize += (uint32_t)n;
      yield();
    }
    f.close();
  }

  const uint8_t finalBlk[5] = { 0x01, 0x00, 0x00, 0xFF, 0xFF };
  writeChunk(finalBlk, 5);

  uint8_t trailer[8] = {
    (uint8_t)crc,        (uint8_t)(crc >> 8),
    (uint8_t)(crc >> 16),(uint8_t)(crc >> 24),
    (uint8_t)isize,      (uint8_t)(isize >> 8),
    (uint8_t)(isize >> 16),(uint8_t)(isize >> 24)
  };
  writeChunk(trailer, 8);

  cl.write((const uint8_t*)"0\r\n\r\n", 5);
}

static void startWebServer() {
  server.on("/", handleRoot);
  server.on("/status.json", handleStatus);
  server.on("/files.json", handleFiles);
  server.on("/download",    handleDownload);
  server.on("/downloadAll", handleDownloadAll);
  server.on("/delete", HTTP_POST, handleDelete);
  server.on("/start",    HTTP_POST, handleStart);
  server.on("/stop",     HTTP_POST, handleStop);
  server.on("/extend",   HTTP_POST, handleExtend);
  server.on("/nextpage", HTTP_POST, handleNextPage);
  server.on("/saveConfig", HTTP_POST, handleSaveConfig);
  server.on("/wigle/test",      HTTP_POST, handleWigleTest);
  server.on("/wigle/uploadAll", HTTP_POST, handleWigleUploadAll);
  server.on("/wigle/upload",    HTTP_POST, handleWigleUploadOne);
  server.on("/wdgwars/test",    HTTP_POST, handleWdgwarsTest);
  server.on("/wdgwars/uploadAll",HTTP_POST, handleWdgwarsUploadAll);
  server.on("/reboot",          HTTP_POST, handleReboot);
  server.on("/cleanup",         HTTP_POST, handleCleanup);
  server.on("/deleteAll",       HTTP_POST, handleDeleteAll);
  server.begin();
  Serial.println("[WEB] Server started");
}

// ---------------- WiFi ----------------
static void startAP() {
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(cfg.wardriverSsid.c_str(), cfg.wardriverPsk.c_str());
  apStartMs = millis(); apClientSeen = false; apWindowActive = true;
  apExtended = false; apExtendedStartMs = 0; apForceClose = false;
  Serial.print("[WIFI] AP SSID: "); Serial.println(cfg.wardriverSsid);
  Serial.print("[WIFI] AP IP: ");   Serial.println(WiFi.softAPIP());
}

static void forceDnsEspNetif(IPAddress dns1, IPAddress dns2) {
  esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  if (!netif) return;
  esp_netif_dns_info_t info;
  memset(&info, 0, sizeof(info));
  info.ip.type = ESP_IPADDR_TYPE_V4;
  info.ip.u_addr.ip4.addr = (uint32_t)dns1;
  esp_netif_set_dns_info(netif, ESP_NETIF_DNS_MAIN, &info);
  memset(&info, 0, sizeof(info));
  info.ip.type = ESP_IPADDR_TYPE_V4;
  info.ip.u_addr.ip4.addr = (uint32_t)dns2;
  esp_netif_set_dns_info(netif, ESP_NETIF_DNS_BACKUP, &info);
}

static void fixDnsIfNeeded() {
  IPAddress dns = WiFi.dnsIP();
  IPAddress gw  = WiFi.gatewayIP();
  bool badDns = (dns == IPAddress(0,0,0,0)) || (dns == gw);
  IPAddress dns1(1,1,1,1), dns2(8,8,8,8);
  if (badDns) {
    WiFi.config(INADDR_NONE, INADDR_NONE, INADDR_NONE, dns1, dns2);
    delay(100);
    if (WiFi.dnsIP() == IPAddress(0,0,0,0) || WiFi.dnsIP() == WiFi.gatewayIP()) {
      forceDnsEspNetif(dns1, dns2); delay(100);
    }
  }
}

static bool connectSTA(uint32_t timeoutMs) {
  if (cfg.homeSsid.length() == 0) { Serial.println("[WIFI] No home SSID"); return false; }
  Serial.print("[WIFI] Connecting to: "); Serial.println(cfg.homeSsid);
  IPAddress dns1(1,1,1,1), dns2(8,8,8,8);
  WiFi.config(INADDR_NONE, INADDR_NONE, INADDR_NONE, dns1, dns2);
  delay(50);
  WiFi.begin(cfg.homeSsid.c_str(), cfg.homePsk.c_str());
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - start) < timeoutMs) {
    // Drain GPS buffer every poll cycle — at 9600 baud, ~192 bytes arrive per 200ms
    // delay, which would overflow the buffer in ~2 iterations without draining.
    while (GPSSerial.available()) gps.encode(GPSSerial.read());
    delay(200);
    if (WiFi.softAPgetStationNum() > 0) apClientSeen = true;
  }
  bool ok = (WiFi.status() == WL_CONNECTED);
  Serial.println(ok ? "[WIFI] Connected" : "[WIFI] Failed");
  if (ok) { Serial.print("[WIFI] IP: "); Serial.println(WiFi.localIP()); fixDnsIfNeeded(); }
  return ok;
}

static void stopAPIfAllowed() {
  if (!apWindowActive) return;

  bool shouldClose = false;
  const char* reason = "";

  if (apForceClose) {
    shouldClose = true;
    reason = "WebUI force-close";
  } else if (apExtended) {
    if ((millis() - apExtendedStartMs) > AP_EXTENDED_WINDOW_MS) {
      shouldClose = true;
      reason = "extended window expired";
    }
  } else {
    if ((millis() - apStartMs) > AP_WINDOW_MS) {
      shouldClose = true;
      reason = "60 s window expired";
    }
  }

  if (shouldClose) {
    Serial.printf("[WIFI] Stopping AP (%s).\n", reason);
    WiFi.softAPdisconnect(true); apWindowActive = false;
    forceStatusFullRedraw();
    apExtended = false; apForceClose = false;
    WiFi.setAutoReconnect(false); WiFi.persistent(false);
    WiFi.disconnect(true, true); delay(50);
    if (WiFi.status() != WL_CONNECTED) WiFi.mode(WIFI_STA);
    if (!(userScanOverride && !scanningEnabled)) scanningEnabled = true;
  }
}

static void handleStaTransitions() {
  wl_status_t now = WiFi.status();
  if (lastStaStatus == WL_CONNECTED && now != WL_CONNECTED) {
    Serial.println("[WIFI] STA disconnected -> switching to wardriving mode");
    // Disable auto-reconnect so the WiFi stack is idle for scanning.
    // Background reconnection attempts interfere with WiFi.scanNetworks()
    // and cause it to return 0 results even while the scan indicator shows active.
    WiFi.setAutoReconnect(false);
    WiFi.persistent(false);
    WiFi.disconnect(true, false);  // stop reconnect; eraseap=false keeps NVS credentials
    delay(50);
    WiFi.mode(WIFI_STA);  // clean idle STA state ready for scanning
    if (!(userScanOverride && !scanningEnabled)) scanningEnabled = true;
  }
  static wl_status_t prev = WL_IDLE_STATUS;
  if (prev != now) forceStatusFullRedraw();
  prev = now;
  lastStaStatus = now;
}

static bool shouldPauseScanning() {
  if (WiFi.status() == WL_CONNECTED) return true;
  wifi_mode_t m = WiFi.getMode();
  if (m == WIFI_AP || m == WIFI_AP_STA) return true;
  return false;
}

// ---------------- Scan (2.4 + 5 GHz) ----------------
static void processScanResults(int n) {
  if (n <= 0) { WiFi.scanDelete(); return; }

  String firstSeen = iso8601NowUTC();
  double lat = 0, lon = 0, altM = 0, accM = 0;
  if (gpsHasFix) {
    lat = gps.location.lat(); lon = gps.location.lng();
    altM = gps.altitude.meters(); accM = gps.hdop.hdop();
  }

  uint32_t wrote = 0;
  for (int i = 0; i < n; i++) {
    int ch = WiFi.channel(i);
    bool chUnknown = (ch == 0);
    bool is2g = (ch >= 1 && ch <= 14) || chUnknown;
    bool is5g = (ch >= 32 && ch <= 177);

    if (!is2g && !is5g) continue;

    String ssid = WiFi.SSID(i);
    String mac  = WiFi.BSSIDstr(i);
    int rssi = WiFi.RSSI(i);
    String authStr = authModeToString(WiFi.encryptionType(i));

    if (is2g) networksFound2G++;
    else      networksFound5G++;

    appendWigleRow(mac, ssid, authStr, firstSeen, ch, rssi, lat, lon, altM, accM);
    wrote++;
  }

  WiFi.scanDelete();

  // Force flush after each scan batch so data reaches the SD card promptly.
  // Minimises data loss if the device loses power between scan cycles.
  if (wrote > 0 && sdOk && logFile) {
    digitalWrite(PINS.tft_cs, HIGH);
    logFile.flush();
  }

  if (wrote > 0) ledPulseGreen();
  Serial.printf("[SCAN] Wrote %lu rows\n", (unsigned long)wrote);
}

static void doScanOnce() {
  static uint32_t lastScanStartMs = 0;
  static bool     scanInProgress  = false;
  static uint8_t  zeroScanCount   = 0;

  // aggressive:  100 ms/channel dwell, 4500 ms gap (gives radio ~3 s idle per cycle)
  // powersaving: 200 ms/channel dwell, 12000 ms gap
  bool powersave   = (cfg.scanMode == "powersaving");
  uint32_t gapMs   = powersave ? 12000 : 4500;
  uint32_t dwellMs = powersave ?   200 :  100;

  // Check if the async scan launched last iteration has finished
  if (scanInProgress) {
    int n = WiFi.scanComplete();
    if (n == WIFI_SCAN_RUNNING) return;  // still running — come back next tick

    scanInProgress = false;
    lastScanStartMs = millis();

    if (n == WIFI_SCAN_FAILED || n < 0) {
      WiFi.scanDelete();
      zeroScanCount++;
      Serial.printf("[SCAN] Failed/empty (%u)\n", zeroScanCount);
      if (zeroScanCount >= 3) {
        Serial.println("[SCAN] Resetting WiFi radio (stuck recovery)");
        WiFi.mode(WIFI_OFF); delay(200);
        WiFi.mode(WIFI_STA); delay(200);
        zeroScanCount = 0;
      }
      return;
    }

    zeroScanCount = 0;
    Serial.printf("[SCAN] Async complete: %d networks\n", n);
    processScanResults(n);
    return;
  }

  // Wait for the minimum gap before starting the next scan
  if (millis() - lastScanStartMs < gapMs) return;

  // Kick off a new async scan
  // async=true, show_hidden=true, passive=false, max_ms_per_chan=dwellMs
  int16_t rc = WiFi.scanNetworks(/*async*/true, /*show_hidden*/true,
                                 /*passive*/false, dwellMs);
  if (rc == WIFI_SCAN_RUNNING || rc == 0) {
    scanInProgress = true;
    Serial.printf("[SCAN] Async scan started (dwell=%lu ms)\n", (unsigned long)dwellMs);
  } else {
    Serial.printf("[SCAN] scanNetworks start failed (%d)\n", rc);
    lastScanStartMs = millis();
  }
}

// Enable WiFi modem sleep so the radio powers down between scan cycles.
// Called once after boot and after any WiFi mode change that re-enters STA.
static void enableModemSleep() {
  esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
}

// ---------------- Page cycling ----------------
// Pages advance two ways:
//   1. User button on GPIO28 (press to advance).
//   2. Web endpoint POST /nextpage.
static void advancePage() {
  uint8_t prev = currentPage;
  uint8_t next = (currentPage + 1) % PAGE_COUNT;
  if (prev == 4) {
    if (meshCoreActive) exitCoreMode();
    else                exitNodeMode();
  }
  currentPage = next;
  tftPageEntered(next);
  Serial.printf("[PAGE] -> %d\n", next);
}

// Button: triple-tap on pig page (3) = twerk; single press = advance page;
//         2 s hold on mesh page (4) = toggle Core / Node.
static void pollButton() {
  static uint32_t pressStartMs  = 0;
  static bool     pressing      = false;
  static bool     longTriggered = false;
  // Multi-click detection
  static uint8_t  clickCount    = 0;
  static uint32_t firstClickMs  = 0;
  static const uint32_t MULTI_MS = 400;  // max gap between taps

  bool low = (digitalRead(PINS.btn) == LOW);

  if (low && !pressing) {
    pressing = true; pressStartMs = millis(); longTriggered = false;
  }

  if (pressing && !longTriggered && (millis() - pressStartMs >= 2000)) {
    longTriggered = true;
    clickCount = 0;  // cancel any pending multi-click
    if (currentPage == 4) {
      if (meshCoreActive) { exitCoreMode(); enterNodeMode(); }
      else                { exitNodeMode(); enterCoreMode(); }
      Serial.printf("[BTN] Long press on mesh page -> %s\n",
                    meshCoreActive ? "Core" : "Node");
    }
  }

  if (!low && pressing) {
    if (!longTriggered) {
      clickCount++;
      if (clickCount == 1) firstClickMs = millis();
    }
    pressing = false;
  }

  // Evaluate click count after multi-tap window expires
  if (clickCount > 0 && (millis() - firstClickMs) > MULTI_MS) {
    if (clickCount >= 3 && currentPage == 3) {
      sasquatchStartTFT();
    } else if (clickCount == 2 && currentPage == 3) {
      pigTwerkStartTFT();
    } else {
      // Single (or unrecognised multi) press -> advance page
      advancePage();
      Serial.printf("[BTN] GPIO%d pressed (clicks=%d)\n", PINS.btn, clickCount);
    }
    clickCount = 0;
  }
}


// ================================================================
//  setup()
// ================================================================
void setup() {
  // Drive SD CS HIGH immediately — GPIO23 floats to input on any reset,
  // which can hold the SD card in a selected/confused state for the entire
  // Serial.begin + delay period (~3s) before the normal pin setup runs.
  pinMode(PINS.sd_cs, OUTPUT);
  digitalWrite(PINS.sd_cs, HIGH);

  // Reduce CPU clock to lower heat — 80 MHz is plenty for scanning + TFT + GPS.
  setCpuFrequencyMhz(80);

  Serial.begin(115200);
  delay(300);
  // Print immediately — if this line never appears, the device is crashing before
  // setup() runs. Fix: Arduino IDE → Tools → PSRAM = Disabled,
  //                                      USB CDC on Boot = Enabled.
  Serial.println("[BOOT] setup() start");
  delay(900);
  uint32_t t0 = millis();
  while (!Serial && (millis() - t0) < 1000) delay(10);

  Serial.println();
  Serial.printf("[BOOT] Reset reason: %d\n", (int)esp_reset_reason());
  networksFound2G = 0;
  networksFound5G = 0;

  Serial.println("=== Piglet Wardriver Boot (T-Dongle C5) ===");

  // User button on GPIO28
  pinMode(PINS.btn, INPUT_PULLUP);

  // TFT + SD share SPI — init SD first, then TFT
  pinMode(PINS.tft_cs, OUTPUT); digitalWrite(PINS.tft_cs, HIGH);
  pinMode(PINS.sd_cs,  OUTPUT); digitalWrite(PINS.sd_cs,  HIGH);
  pinMode(PINS.tft_rst, OUTPUT); digitalWrite(PINS.tft_rst, LOW);
  pinMode(PINS.tft_dc, OUTPUT); digitalWrite(PINS.tft_dc, HIGH);
  delay(50);

  SPI.begin(PINS.sd_sck, PINS.sd_miso, PINS.sd_mosi);
  delay(20);

  // SD init
  Serial.println("[SD] Initializing...");
  sdOk = initSD_SharedSPI();
  if (sdOk && SD.cardType() == CARD_NONE) sdOk = false;
  Serial.print("[SD] sdOk="); Serial.println(sdOk ? "true" : "false");

  if (!sdOk) ledRed(); // Show SD fail on LED

  // Load config
  if (sdOk) loadConfigFromSD();

  // TFT init (after SD)
  digitalWrite(PINS.tft_rst, HIGH);
  delay(120);
  tft.initR(INITR_MINI160x80);
  tft.invertDisplay(true);          // ST7735S panel powers up in inverted mode
  tft.setColRowStartPublic(26, 1);  // ST7735S panel window: col offset 26, row offset 1
  tft.setRotation(cfg.rotateScreen180 ? 2 : 0);  // Portrait (0=normal, 2=180° flip)
  tft.fillScreen(BLACK);
  tft.setTextColor(WHITE, BLACK);
  // Backlight at ~50% via PWM (active-LOW P-ch MOSFET gate: 0=full, 255=off)
  ledcAttach(PINS.tft_bl, 5000, 8);  // 5 kHz, 8-bit resolution
  ledcWrite(PINS.tft_bl, 128);       // duty 128/255 ≈ 50% brightness

  // Splash
  bootSlogan = pickSplashSlogan();
  tftSplashAnimateOnce(bootSlogan);
  uiFirstDraw = true;
  forceStatusFullRedraw();

  // Verify SD still OK after TFT init
  if (sdOk) {
    File test = SD.open("/logs");
    if (test) test.close();
    else { sdOk = false; ledRed(); }
  }

  // GPS (UART via Qwiic connector)
  // QWIIC pinout: RX = GPIO12 (from GPS TX), TX = GPIO11 (to GPS RX).
  // Increase RX buffer to 512 bytes: at 9600 baud, the default 256-byte buffer fills
  // in ~267ms — not enough headroom for WiFi scan (~300ms) or SD flush bursts.
  // setRxBufferSize() must be called before begin().
  Serial.printf("[GPS] UART on RX=%d TX=%d Baud=%lu\n", PINS.gps_rx, PINS.gps_tx, (unsigned long)cfg.gpsBaud);
  GPSSerial.setRxBufferSize(512);
  GPSSerial.begin(cfg.gpsBaud, SERIAL_8N1, PINS.gps_rx, PINS.gps_tx);

  // WiFi setup: mesh boot with a home network connects STA first for auto-upload,
  // then hands off to mesh. Mesh boot with no home network skips STA/AP entirely.
  // Normal wardriving boot tries STA and falls back to AP.
  WiFi.mode(WIFI_STA);
  bool staOk = false;
  {
    String mm = cfg.meshModeOnBoot; mm.toLowerCase();
    bool coreBoot    = (mm == "core");
    bool nodeBoot    = (mm == "node");
    bool hasHomeSsid = (cfg.homeSsid.length() > 0);

    if (nodeBoot) {
      // Node mode always skips STA/AP — go straight to mesh.
      Serial.println("[BOOT] meshModeOnBoot=node — skipping STA/AP");
    } else if (coreBoot && !hasHomeSsid) {
      // Core with no home network — skip STA/AP, go straight to mesh.
      Serial.println("[BOOT] meshModeOnBoot=core, no homeSsid — skipping STA/AP");
    } else {
      staOk = connectSTA(12000);
      if (!staOk) {
        WiFi.setAutoReconnect(false); WiFi.persistent(false);
        WiFi.disconnect(true, true); delay(100);
        if (!coreBoot) {
          // Normal wardriving: fall back to AP for web UI access.
          startAP();
        } else {
          // Core boot: STA failed, skip AP — proceed to mesh mode.
          Serial.println("[BOOT] meshModeOnBoot=core, STA failed — skipping AP");
        }
      }
    }
  }

  startWebServer();
  lastStaStatus = WiFi.status();

  if (staOk) {
    Serial.print("[WEB] STA IP: "); Serial.println(WiFi.localIP());
  } else {
    Serial.print("[WEB] AP IP: "); Serial.println(WiFi.softAPIP());
  }

  // Purge header-only CSVs before scanning or uploading
  if (sdOk) {
    Serial.println("[SD] Cleaning up empty CSVs...");
    // inline cleanup for T-Dongle (no shared WigleUpload module)
    if (SD.exists("/logs")) {
      File root = SD.open("/logs");
      std::vector<String> toDelete;
      if (root) {
        File f = root.openNextFile();
        while (f) {
          String path = normalizeSdPath("/logs", f.name());
          if (path.endsWith(".csv")) toDelete.push_back(path);
          f.close(); f = root.openNextFile();
        }
        root.close();
      }
      for (const String& path : toDelete)
        if (!csvHasDataRows(path)) { Serial.printf("[CLEANUP] Deleting: %s\n", pathBasename(path).c_str()); SD.remove(path); }
    }
  }

  // Boot upload: WDGoWars first (if key set), then WiGLE (if token set).
  // maxBootUploads: -1=all, 0=disabled, 1+=capped.
  {
    bool hasWigle = cfg.wigleBasicToken.length() > 0;
    bool hasWdg   = cfg.wdgwarsApiKey.length()   > 0;
    if (staOk && sdOk && (hasWigle || hasWdg) && cfg.maxBootUploads != 0) {
      Serial.print("[UPLOAD] Services: ");
      if (hasWdg)   Serial.print("WDGoWars ");
      if (hasWigle) Serial.print("WiGLE");
      Serial.println();
      uploadAllCsvsToWigle(cfg.maxBootUploads);
    } else if (!staOk || !sdOk) {
      Serial.println("[UPLOAD] Skipped (STA/SD not ready).");
    } else if (cfg.maxBootUploads == 0) {
      Serial.println("[UPLOAD] Disabled (maxBootUploads=0).");
    }
  }

  // Open fresh log
  if (sdOk) {
    bool lfOk = openLogFile();
    Serial.print("[SD] Log file: "); Serial.println(lfOk ? "OK" : "FAIL");
  }

  // Auto-start mesh mode if meshModeOnBoot=Core|Node.
  // Core mode: STA was used for upload above and must be torn down so
  // ESP-Now can start cleanly on the admin channel without interference.
  // Node mode: STA was already skipped.
  {
    String mm = cfg.meshModeOnBoot;
    mm.toLowerCase();
    if (mm == "node") {
      Serial.println("[BOOT] meshModeOnBoot=Node — entering Mesh Node mode");
      currentPage = 4;  // mesh page on T-Dongle
      pageNeedsInit[4] = true;
      enterNodeMode();
    } else if (mm == "core") {
      // Tear down STA cleanly before entering Core — the STA connection
      // (if any) was only needed for auto-upload and must not remain active
      // or ESP-Now channel control will conflict with the STA home channel.
      if (WiFi.status() == WL_CONNECTED || WiFi.getMode() != WIFI_OFF) {
        Serial.println("[BOOT] Tearing down STA before Core mode");
        WiFi.disconnect(true, true);
        WiFi.mode(WIFI_OFF);
        delay(150);
      }
      Serial.println("[BOOT] meshModeOnBoot=Core — entering Mesh Core mode");
      currentPage = 4;
      enterCoreMode();
      pageNeedsInit[4] = true;
    }
  }

  updateTFT(0);
  ledOff();
  enableModemSleep();
  Serial.printf("[BOOT] CPU: %lu MHz\n", (unsigned long)getCpuFrequencyMhz());
  Serial.println("=== Boot complete ===");
}

// ================================================================
//  loop()
// ================================================================
void loop() {
  server.handleClient();

  // AP window
  if (apWindowActive && WiFi.getMode() == WIFI_AP_STA) {
    if (WiFi.softAPgetStationNum() > 0) {
      if (!apClientSeen) Serial.println("[WIFI] AP client connected");
      apClientSeen = true;
    }
  }
  stopAPIfAllowed();

  // GPS
  while (GPSSerial.available()) gps.encode(GPSSerial.read());

  bool prevFix = gpsHasFix;
  gpsHasFix = gps.location.isValid() && gps.location.age() < 2000;
  if (gpsHasFix != prevFix) {
    Serial.println(gpsHasFix ? "[GPS] LOCKED" : "[GPS] NO FIX");
  }

  uiGpsSats = gps.satellites.isValid() ? (int)gps.satellites.value() : 0;

  float speedKmph = gps.speed.isValid() ? gps.speed.kmph() : 0.0f;
  float speedDisplay = (cfg.speedUnits == "mph") ? speedKmph * 0.621371f : speedKmph;

  if (gps.course.isValid() && gps.course.age() < 2000) {
    uiHeadingDeg = gps.course.deg();
    uiHeadingTxt = headingToDir8(uiHeadingDeg);
  }

  // Set system time from GPS (once)
  static bool timeSet = false;
  if (!timeSet && gps.date.isValid() && gps.time.isValid() &&
      gps.date.age() < 5000 && gps.time.age() < 5000) {
    struct tm t {};
    t.tm_year = gps.date.year() - 1900;
    t.tm_mon  = gps.date.month() - 1;
    t.tm_mday = gps.date.day();
    t.tm_hour = gps.time.hour();
    t.tm_min  = gps.time.minute();
    t.tm_sec  = gps.time.second();
    time_t epoch = makeUtcEpochFromTm(&t);
    struct timeval now = { .tv_sec = epoch, .tv_usec = 0 };
    settimeofday(&now, nullptr);
    timeSet = true;
    Serial.println("[TIME] Set from GPS (UTC)");
  }

  pollButton();

  // TFT dispatch — pig ~90 ms, mesh node 1 s, others 500 ms
  if (currentPage == 3) {
    pigAnimTickTFT();
  } else if (currentPage == 4) {
    static uint32_t lastMeshTft = 0;
    if (millis() - lastMeshTft > 1000) {
      lastMeshTft = millis();
      drawPageMeshNode();
    }
  } else {
    static uint32_t lastTft = 0;
    if (millis() - lastTft > 500) {
      lastTft = millis();
      if      (currentPage == 0) updateTFT(speedDisplay);
      else if (currentPage == 1) drawPageNetworks();
      else if (currentPage == 2) drawPageNavigation(speedDisplay);
    }
  }

  // LED tick (non-blocking pulse)
  ledTick();

  // Skip STA transition handler in mesh mode — it calls WiFi.disconnect(wifioff=true)
  // which stops the WiFi driver and deinits ESP-Now.
  if (!meshNodeActive && !meshCoreActive) handleStaTransitions();

  // Scanning — mesh page handles its own logic; skip normal scan path
  if (currentPage == 4) {
    if (meshCoreActive) coreModeTick();
    else                nodeModeTick();
  } else {
    autoPaused = shouldPauseScanning();
    wifi_mode_t m = WiFi.getMode();
    bool apActive = (m == WIFI_AP || m == WIFI_AP_STA);
    bool allowScan = scanningEnabled && sdOk && !apActive && (userScanOverride || !autoPaused);
    if (allowScan) doScanOnce();
  }

  delay(10);
}
