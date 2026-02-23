/*
 * Waveshare ESP32-C6 LCD 1.47" Piglet Wardriver (WiGLE CSV + GPS + SD + TFT + Web UI)
 *
 * WAVESHARE-ONLY PORT (NO OLED, NO MULTI-BOARD CONFIG)
 *
 * Features:
 * - 2.4GHz WiFi scan -> WiGLE CSV on SD (append GPS per row)
 * - 1.47" TFT status display (SPI)
 * - User button toggles scanning on/off
 * - Boot: attempt STA to home WiFi
 * - If STA fails: start SoftAP (192.168.4.1) for 60s HARD cutoff, then wardrive begins
 * - If STA connects: upload previous CSV files to WiGLE using Basic token
 * - Web UI: browse/download/delete files, edit config (saved to SD)
 *
 * Pin map (AS CURRENTLY DEFINED IN CODE BELOW):
 *
 *   TFT (SPI via TFT_eSPI / ST7789):
 *     SCK = GPIO7
 *     MOSI= GPIO6
 *     CS  = GPIO14
 *     DC  = GPIO15
 *     RST = GPIO21
 *     BL  = GPIO22
 *
 *   SD / TF (SPI):
 *     CS  = GPIO4
 *     SCK = GPIO7
 *     MISO= GPIO5
 *     MOSI= GPIO6
 *
 *   GPS (UART) [project-chosen]:
 *     GPS RX = GPIO17
 *     GPS TX = GPIO16   (ESP32 receives on RX pin)
 *
 *   Button [project-chosen]:
 *     BTN = GPIO2 (INPUT_PULLUP)
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
#include <time.h>
#include <math.h>



// ---------------- Pins (WAVESHARE ONLY) ----------------
struct PinMap {
  // TFT (SPI)
  int tft_sck, tft_mosi, tft_cs, tft_dc, tft_rst, tft_bl;

  // SD (SPI)
  int sd_cs, sd_sck, sd_miso, sd_mosi;

  // GPS (UART)
  int gps_rx, gps_tx;

  // User button
  int btn;
};

static const PinMap PINS = {
  // TFT (Waveshare ESP32-C6-LCD-1.47)
  7,   // tft_sck
  6,   // tft_mosi
  14,  // tft_cs
  15,  // tft_dc
  21,  // tft_rst
  22,  // tft_bl

  // SD / TF (shares SCK/MOSI with TFT per this pinmap; SD adds MISO and uses its own CS)
  4,   // sd_cs
  7,   // sd_sck
  5,   // sd_miso
  6,   // sd_mosi

  // GPS
  17,  // gps_rx
  16,  // gps_tx

  // BTN
  2
};


// ---------------- TFT (LovyanGFX) ----------------
#include <LovyanGFX.hpp>

// --- RGB565 color constants ---
#ifndef BLACK
  #define BLACK 0x0000
#endif
#ifndef WHITE
  #define WHITE 0xFFFF
#endif

// LovyanGFX device for Waveshare ESP32-C6 LCD 1.47" (SPI)
// NOTE: Many 1.47" modules are 172x320 with a 240x320 controller window.
// If your screen is shifted/cropped, tweak offset_x/offset_y below.
class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ST7789 _panel;
  lgfx::Bus_SPI      _bus;
  lgfx::Light_PWM    _light;

public:
  LGFX() {
    { // SPI bus
      auto cfg = _bus.config();
      cfg.spi_host   = SPI2_HOST;     // ESP32-C6
      cfg.spi_mode   = 0;
      cfg.freq_write = 40000000;      // try 40MHz; if unstable drop to 27000000
      cfg.freq_read  = 16000000;
      cfg.spi_3wire  = false;         // MUST be 4-wire when sharing SPI with SD (needs MISO)
      cfg.use_lock   = true;
      cfg.dma_channel = 0;

      cfg.pin_sclk = PINS.tft_sck;
      cfg.pin_mosi = PINS.tft_mosi;
      cfg.pin_miso = PINS.sd_miso;    // keep SPI host MISO enabled for SD card
      cfg.pin_dc   = PINS.tft_dc;

      _bus.config(cfg);
      _panel.setBus(&_bus);
    }

    { // Panel
      auto cfg = _panel.config();

      cfg.pin_cs   = PINS.tft_cs;
      cfg.pin_rst  = PINS.tft_rst;
      cfg.pin_busy = -1;

      // Common for 1.47" Waveshare-style modules:
      cfg.panel_width  = 172;
      cfg.panel_height = 320;

      // Controller memory is often 240x320, with active area centered => offset_x 34
      cfg.memory_width  = 240;
      cfg.memory_height = 320;

      cfg.offset_x = 34;   // <-- if image is shifted, adjust this (0..68)
      cfg.offset_y = 0;

      cfg.invert = true;  // if colors look inverted, set true
      cfg.rgb_order = true; // if colors swapped (BGR/RGB), toggle this

      cfg.dlen_16bit = false;
      cfg.bus_shared = true; // IMPORTANT: shared with SD

      _panel.config(cfg);
      _panel.setColorDepth(lgfx::color_depth_t::rgb565_2Byte);
      setSwapBytes(true);
    }

    { // Backlight (PWM)
      auto cfg = _light.config();
      cfg.pin_bl = PINS.tft_bl;
      cfg.invert = false;       // set true if backlight logic is inverted
      cfg.freq   = 44100;
      cfg.pwm_channel = 0;

      _light.config(cfg);
      _panel.setLight(&_light);
    }

    setPanel(&_panel);
  }
};

static LGFX tft;   // keep your existing "tft" name so the rest of your code changes less


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
  String scanMode      = "aggressive"; // aggressive | powersaving
  String speedUnits    = "kmh";        // kmh | mph
};

Config cfg;

static const char* bootSlogan = nullptr;

// ---------------- State ----------------
static bool sdOk = false;
static bool scanningEnabled = true;
static bool gpsHasFix = false;

// ---------- UI polish state ----------
static int  uiGpsSats     = 0;          // satellites count
static float uiHeadingDeg  = 0.0f;      // last-known course (kept if signal lost)
static String uiHeadingTxt = "—";       // N/NE/E/... (kept if signal lost)

static bool uiSdPulse     = false;      // SD write blink
static uint32_t uiLastPulse = 0;

// Bottom nav redraw cache
static float prevNavHeading = -9999.0f;
static String prevNavTxt    = "";

static bool uiFirstDraw = true;        // partial redraw control
// ---- cached values for diff repaint ----
static bool  prevAllowScan = false;
static bool  prevSdOk = false;
static bool  prevGpsFix = false;
static bool  prevSta = false;
static uint32_t prevFound2G = 0;
static float prevSpeed = -999;
static String prevIp = "";
static String prevUploadMsg = "";
static uint32_t prevUploadDone = 0;
static uint32_t prevUploadTotal = 0;

// Auto-pause + manual override
static bool userScanOverride = false;   // becomes true after FIRST button press
static bool autoPaused = false;         // reflects current auto-pause condition

static uint32_t apStartMs = 0;
static bool apClientSeen = false;

// AP window control (HARD limit)
static bool apWindowActive = false;                 // true while AP is allowed to run
static const uint32_t AP_WINDOW_MS = 60000UL;      // 60 seconds

static uint32_t networksFound2G = 0;
static uint32_t networksFound5G = 0; // (unused on C6; kept for status consistency)

static File logFile;
static String currentCsvPath;

static wl_status_t lastStaStatus = WL_IDLE_STATUS;

// --------- De-dupe logic (hash table) -----------
static const uint32_t SEEN_TABLE_SIZE = 16384; // must be power-of-two
static uint64_t seenTable[SEEN_TABLE_SIZE];    // 0 means empty slot
static uint32_t seenCount = 0;
static uint32_t seenCollisions = 0;

static void resetSeenTable() {
  memset(seenTable, 0, sizeof(seenTable));
  seenCount = 0;
  seenCollisions = 0;
  Serial.printf("[DEDUP] Seen-table reset (size=%lu entries, %lu bytes)\n",
                (unsigned long)SEEN_TABLE_SIZE,
                (unsigned long)sizeof(seenTable));
}

// ---------------- WiGLE Upload State ----------------
static bool     uploading = false;
static bool     uploadPausedScanWasEnabled = false;

static uint32_t uploadTotalFiles = 0;
static uint32_t uploadDoneFiles  = 0;

static String   uploadCurrentFile = "";
static String   uploadLastResult  = "";   // human readable
static int      wigleTokenStatus  = 0;    // 0=unknown, 1=valid, -1=invalid
static int      wigleLastHttpCode = 0;

static const char* WIGLE_HOST = "api.wigle.net";
static const uint16_t WIGLE_PORT = 443;

// ---------------- Helpers ----------------
static String pathBasename(const String& p) {
  int slash = p.lastIndexOf('/');
  if (slash < 0) return p;
  return p.substring(slash + 1);
}

static inline uint32_t hash48(uint64_t k) {
  k ^= (k >> 33);
  k *= 0xff51afd7ed558ccdULL;
  k ^= (k >> 33);
  k *= 0xc4ceb9fe1a85ec53ULL;
  k ^= (k >> 33);
  return (uint32_t)k;
}

static uint64_t bssidStrToKey48(const String& mac) {
  uint64_t v = 0;
  int nibs = 0;
  for (int i = 0; i < mac.length(); i++) {
    char c = mac[i];
    int val = -1;
    if (c >= '0' && c <= '9') val = c - '0';
    else if (c >= 'a' && c <= 'f') val = 10 + (c - 'a');
    else if (c >= 'A' && c <= 'F') val = 10 + (c - 'A');
    else continue;
    v = (v << 4) | (uint64_t)val;
    nibs++;
    if (nibs >= 12) break;
  }
  if (nibs != 12) return 0;
  return v;
}

static bool seenCheckOrInsert(uint64_t key48) {
  if (key48 == 0) return false;
  uint64_t stored = (key48 | 1ULL);
  uint32_t idx = hash48(stored) & (SEEN_TABLE_SIZE - 1);
  for (uint32_t probe = 0; probe < SEEN_TABLE_SIZE; probe++) {
    uint32_t p = (idx + probe) & (SEEN_TABLE_SIZE - 1);
    uint64_t slot = seenTable[p];
    if (slot == 0) {
      seenTable[p] = stored;
      seenCount++;
      if (probe) seenCollisions++;
      return false;
    }
    if (slot == stored) return true;
  }
  return false;
}

static String authModeToString(wifi_auth_mode_t m) {
  switch (m) {
    case WIFI_AUTH_OPEN: return "OPEN";
    case WIFI_AUTH_WEP: return "WEP";
    case WIFI_AUTH_WPA_PSK: return "WPA";
    case WIFI_AUTH_WPA2_PSK: return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK: return "WPAWPA2";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2EAP";
    case WIFI_AUTH_WPA3_PSK: return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2WPA3";
    default: return "UNKNOWN";
  }
}

static time_t makeUtcEpochFromTm(struct tm* t) {
  const char* oldTz = getenv("TZ");
  setenv("TZ", "UTC0", 1);
  tzset();
  time_t epoch = mktime(t);
  if (oldTz) setenv("TZ", oldTz, 1);
  else unsetenv("TZ");
  tzset();
  return epoch;
}

static String iso8601NowUTC() {
  if (gps.date.isValid() && gps.time.isValid() &&
      gps.date.age() < 5000 && gps.time.age() < 5000) {

    char buf[32];
    snprintf(buf, sizeof(buf),
             "%04d-%02d-%02dT%02d:%02d:%02dZ",
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

// ---------------- Config file (plain text) ----------------
static const char* CFG_PATH = "/wardriver.cfg";

static inline String trimCopy(String s) { s.trim(); return s; }

static bool parseKeyValueLine(const String& lineIn, String& keyOut, String& valOut) {
  String line = lineIn;
  line.trim();
  if (line.length() == 0) return false;
  if (line[0] == '#') return false;
  if (line.startsWith("//")) return false;

  int eq = line.indexOf('=');
  if (eq < 0) return false;

  String k = line.substring(0, eq);
  String v = line.substring(eq + 1);
  k.trim(); v.trim();

  if (v.length() >= 2 && ((v[0] == '"' && v[v.length()-1] == '"') || (v[0] == '\'' && v[v.length()-1] == '\''))) {
    v = v.substring(1, v.length()-1);
    v.trim();
  }

  if (k.length() == 0) return false;
  keyOut = k;
  valOut = v;
  return true;
}

static void cfgAssignKV(const String& k, const String& v) {
  if (k == "wigleBasicToken") cfg.wigleBasicToken = v;
  else if (k == "homeSsid")   cfg.homeSsid = v;
  else if (k == "homePsk")    cfg.homePsk = v;
  else if (k == "wardriverSsid") cfg.wardriverSsid = v;
  else if (k == "wardriverPsk")  cfg.wardriverPsk = v;
  else if (k == "gpsBaud") {
    uint32_t b = (uint32_t)v.toInt();
    if (b > 0) cfg.gpsBaud = b;
  }
  else if (k == "scanMode") {
    if (v == "aggressive" || v == "powersaving") cfg.scanMode = v;
  }
  else if (k == "speedUnits") {
    String vv = v; vv.toLowerCase();
    if (vv == "kmh" || vv == "mph") cfg.speedUnits = vv;
  }
}

static bool saveConfigToSD() {
  Serial.println("[CFG] saveConfigToSD() start");
  if (!sdOk) {
    Serial.println("[CFG] SD not OK, cannot save config");
    return false;
  }

  digitalWrite(PINS.tft_cs, HIGH);
if (SD.exists(CFG_PATH)) { digitalWrite(PINS.tft_cs, HIGH); SD.remove(CFG_PATH); }

digitalWrite(PINS.tft_cs, HIGH);

  File f = SD.open(CFG_PATH, FILE_WRITE);
  if (!f) {
    Serial.println("[CFG] Failed to open /wardriver.cfg for write");
    return false;
  }

  f.println("# Piglet Wardriver config (key=value) [WAVESHARE C6 LCD 1.47]");
  f.println("# Lines starting with # are comments");
  f.println("");

  f.print("wigleBasicToken="); f.println(cfg.wigleBasicToken);
  f.print("homeSsid=");        f.println(cfg.homeSsid);
  f.print("homePsk=");         f.println(cfg.homePsk);
  f.print("wardriverSsid=");   f.println(cfg.wardriverSsid);
  f.print("wardriverPsk=");    f.println(cfg.wardriverPsk);
  f.print("gpsBaud=");         f.println(cfg.gpsBaud);
  f.print("scanMode=");        f.println(cfg.scanMode);
  f.print("speedUnits=");      f.println(cfg.speedUnits);

  f.flush();
  f.close();

  Serial.println("[CFG] Saved /wardriver.cfg OK");
  return true;
}

static bool loadConfigFromSD() {
  Serial.println("[CFG] loadConfigFromSD() start");
  if (!sdOk) {
    Serial.println("[CFG] SD not OK, skipping config load");
    return false;
  }

digitalWrite(PINS.tft_cs, HIGH);
if (SD.exists(CFG_PATH)) {
  digitalWrite(PINS.tft_cs, HIGH);
  File f = SD.open(CFG_PATH, FILE_READ);
    if (!f) {
      Serial.println("[CFG] Failed to open /wardriver.cfg for read");
      return false;
    }
    while (f.available()) {
      String line = f.readStringUntil('\n');
      String k, v;
      if (parseKeyValueLine(line, k, v)) cfgAssignKV(k, v);
    }
    f.close();

    Serial.println("[CFG] Loaded config:");
    Serial.print("      wardriverSsid: "); Serial.println(cfg.wardriverSsid);
    Serial.print("      wardriverPsk:  "); Serial.println(cfg.wardriverPsk.length() ? "(set)" : "(empty)");
    Serial.print("      homeSsid:      "); Serial.println(cfg.homeSsid);
    Serial.print("      homePsk:       "); Serial.println(cfg.homePsk.length() ? "(set)" : "(empty)");
    Serial.print("      gpsBaud:       "); Serial.println(cfg.gpsBaud);
    Serial.print("      scanMode:      "); Serial.println(cfg.scanMode);
    Serial.print("      wigle token:   "); Serial.println(cfg.wigleBasicToken.length() ? "(set)" : "(empty)");
    Serial.print("      speedUnits:    "); Serial.println(cfg.speedUnits);
    return true;
  }

  Serial.println("[CFG] No /wardriver.cfg. Using defaults.");
  return false;
}

// ---------------- SD logging ----------------
static String normalizeSdPath(const char* dir, const char* nameIn) {
  if (!dir || !nameIn) return "";
  String d(dir);
  String n(nameIn);
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

static String newCsvFilename() {
  digitalWrite(PINS.tft_cs, HIGH);
if (!SD.exists("/logs")) { digitalWrite(PINS.tft_cs, HIGH); SD.mkdir("/logs"); }
  for (int tries = 0; tries < 25; tries++) {
    uint32_t r = (uint32_t)esp_random();
    char buf[64];
    snprintf(buf, sizeof(buf), "/logs/WiGLE_%lu_%08lX.csv",
             (unsigned long)millis(), (unsigned long)r);
    String p(buf);
    digitalWrite(PINS.tft_cs, HIGH);
    if (!SD.exists(p)) return p;
  }
  char buf2[64];
  snprintf(buf2, sizeof(buf2), "/logs/WiGLE_%lu.csv", (unsigned long)millis());
  return String(buf2);
}

static void closeLogFile() {
  if (logFile) {
    Serial.println("[SD] Closing log file");
    digitalWrite(PINS.tft_cs, HIGH);
    logFile.flush();
    digitalWrite(PINS.tft_cs, HIGH);
    logFile.close();
  }
}

static bool openLogFile() {
  if (!sdOk) return false;
  closeLogFile();
  currentCsvPath = newCsvFilename();

  Serial.print("[SD] Opening log file: ");
  Serial.println(currentCsvPath);
  digitalWrite(PINS.tft_cs, HIGH);  // ensure TFT is not listening during SD transactions
  logFile = SD.open(currentCsvPath, FILE_WRITE);
  if (!logFile) {
    Serial.println("[SD] Failed to open log file for write");
    return false;
  }

  logFile.println("WigleWifi-1.4,appRelease=1,model=Waveshare-ESP32C6-LCD-1.47,release=1,device=Piglet-Wardriver");
  logFile.println("MAC,SSID,AuthMode,FirstSeen,Channel,RSSI,CurrentLatitude,CurrentLongitude,AltitudeMeters,AccuracyMeters,Type");
  logFile.flush();

  Serial.println("[SD] Log file initialized with WiGLE headers");
  return true;
}

static void appendWigleRow(const String& mac, const String& ssid, const String& auth,
                           const String& firstSeen, int channel, int rssi,
                           double lat, double lon, double altM, double accM) {
  if (!sdOk || !logFile) return;

  String safeSsid = ssid;
  safeSsid.replace("\"", "\"\"");

  String line;
  line.reserve(256);
  line += mac; line += ",";
  line += "\""; line += safeSsid; line += "\",";
  line += auth; line += ",";
  line += firstSeen; line += ",";
  line += String(channel); line += ",";
  line += String(rssi); line += ",";
  line += String(lat, 6); line += ",";
  line += String(lon, 6); line += ",";
  line += String(altM, 1); line += ",";
  line += String(accM, 1); line += ",";
  line += "WIFI";


    digitalWrite(PINS.tft_cs, HIGH);
    logFile.println(line);
  static uint32_t lastFlushMs = 0;
  static uint32_t linesSinceFlush = 0;
  linesSinceFlush++;

  uint32_t nowMs = millis();
  if (linesSinceFlush >= 25 || (nowMs - lastFlushMs) >= 2000) {
    digitalWrite(PINS.tft_cs, HIGH);
    logFile.flush();
    lastFlushMs = nowMs;
    linesSinceFlush = 0;
  }
}

// ---------------- WiGLE upload helpers ----------------
static bool moveToUploaded(const String& srcPath) {
  if (!sdOk) return false;
  digitalWrite(PINS.tft_cs, HIGH);
    if (!SD.exists(srcPath)) {
    Serial.print("[SD] moveToUploaded: source missing: ");
    Serial.println(srcPath);
    return false;
  }

  digitalWrite(PINS.tft_cs, HIGH);
  if (!SD.exists("/uploaded")) {
    Serial.println("[SD] Creating /uploaded ...");
      digitalWrite(PINS.tft_cs, HIGH);
      if (!SD.mkdir("/uploaded")) {
      Serial.println("[SD] ERROR: SD.mkdir(/uploaded) failed");
      return false;
    }
  }

  String dstPath = String("/uploaded/") + pathBasename(srcPath);

  digitalWrite(PINS.tft_cs, HIGH);
    if (SD.exists(dstPath)) {
    Serial.print("[SD] Removing existing dst: ");
    Serial.println(dstPath);
    digitalWrite(PINS.tft_cs, HIGH);
    SD.remove(dstPath);
  }

  Serial.print("[SD] Moving ");
  Serial.print(srcPath);
  Serial.print(" -> ");
  Serial.println(dstPath);

  digitalWrite(PINS.tft_cs, HIGH);
  bool ok = SD.rename(srcPath, dstPath);
  if (!ok) {
    Serial.println("[SD] ERROR: SD.rename failed");
    digitalWrite(PINS.tft_cs, HIGH);
    File in = SD.open(srcPath, FILE_READ);
    if (!in) { Serial.println("[SD] copy fallback: open src failed"); return false; }

    digitalWrite(PINS.tft_cs, HIGH);
    File out = SD.open(dstPath, FILE_WRITE);
    if (!out) { Serial.println("[SD] copy fallback: open dst failed"); in.close(); return false; }

    uint8_t buf[1024];
    while (true) {
      int n = in.read(buf, sizeof(buf));
      if (n <= 0) break;
      out.write(buf, n);
      delay(0);
    }
    out.flush();
    out.close();
    in.close();

    digitalWrite(PINS.tft_cs, HIGH);
    if (!SD.exists(dstPath)) {
      Serial.println("[SD] copy fallback: dst does not exist after write");
      return false;
    }

    digitalWrite(PINS.tft_cs, HIGH);
    if (!SD.remove(srcPath)) {
      Serial.println("[SD] copy fallback: WARNING failed to remove src after copy");
    }

    Serial.println("[SD] copy fallback: OK");
    return true;
  }

  Serial.println("[SD] Move OK");
  return true;
}

static bool wigleTestToken() {
  wigleLastHttpCode = 0;
  if (WiFi.status() != WL_CONNECTED) {
    uploadLastResult = "No STA WiFi";
    wigleTokenStatus = -1;
    return false;
  }
  if (cfg.wigleBasicToken.length() < 8) {
    uploadLastResult = "No token set";
    wigleTokenStatus = -1;
    return false;
  }

  const char* pathsToTry[] = {
    "/api/v2/profile",
    "/api/v2/stats/user",
    "/api/v2/user/profile"
  };

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(15000);

  for (size_t i = 0; i < sizeof(pathsToTry)/sizeof(pathsToTry[0]); i++) {
    if (!client.connect(WIGLE_HOST, WIGLE_PORT)) {
      uploadLastResult = "TLS connect fail";
      wigleTokenStatus = -1;
      return false;
    }

    String req =
      String("GET ") + pathsToTry[i] + " HTTP/1.1\r\n" +
      "Host: " + WIGLE_HOST + "\r\n" +
      "Authorization: Basic " + cfg.wigleBasicToken + "\r\n" +
      "Connection: close\r\n\r\n";

    client.print(req);

    String status = client.readStringUntil('\n');
    status.trim();

    while (client.connected()) {
      String line = client.readStringUntil('\n');
      if (line == "\r" || line.length() == 0) break;
    }

    client.stop();

    int code = 0;
    if (status.startsWith("HTTP/")) {
      int sp1 = status.indexOf(' ');
      if (sp1 > 0) {
        int sp2 = status.indexOf(' ', sp1 + 1);
        if (sp2 > sp1) code = status.substring(sp1 + 1, sp2).toInt();
      }
    }

    wigleLastHttpCode = code;

    if (code == 200) {
      wigleTokenStatus = 1;
      uploadLastResult = "Token valid (200)";
      return true;
    }
    if (code == 401 || code == 403) {
      wigleTokenStatus = -1;
      uploadLastResult = "Token invalid (" + String(code) + ")";
      return false;
    }
  }

  wigleTokenStatus = 0;
  uploadLastResult = "Token test inconclusive (" + String(wigleLastHttpCode) + ")";
  return false;
}

static bool uploadFileToWigle(const String& path) {
  Serial.print("[WiGLE] uploadFileToWigle: ");
  Serial.println(path);

  wigleLastHttpCode = 0;

  if (WiFi.status() != WL_CONNECTED) {
    uploadLastResult = "No STA WiFi";
    return false;
  }
  if (cfg.wigleBasicToken.length() < 8) {
    uploadLastResult = "No token set";
    return false;
  }

  digitalWrite(PINS.tft_cs, HIGH);
  if (!SD.exists(path)) {
    uploadLastResult = "File missing";
    return false;
  }

  digitalWrite(PINS.tft_cs, HIGH);
  File f = SD.open(path, FILE_READ);
  if (!f) {
    uploadLastResult = "Open fail";
    return false;
  }

  String boundary = "----Piglet-WARDRIVE-BOUNDARY";
  String filename = pathBasename(path);

  String pre =
    "--" + boundary + "\r\n"
    "Content-Disposition: form-data; name=\"file\"; filename=\"" + filename + "\"\r\n"
    "Content-Type: text/csv\r\n\r\n";

  String post =
    "\r\n--" + boundary + "--\r\n";

  uint32_t contentLen = (uint32_t)pre.length() + (uint32_t)f.size() + (uint32_t)post.length();

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(25000);  // Reduced from 30s - most uploads complete much faster

  if (!client.connect(WIGLE_HOST, WIGLE_PORT)) {
    uploadLastResult = "TLS connect fail";
    f.close();
    return false;
  }

  client.print(String("POST /api/v2/file/upload HTTP/1.0\r\n"));
  client.print(String("Host: ") + WIGLE_HOST + "\r\n");
  client.print(String("Authorization: Basic ") + cfg.wigleBasicToken + "\r\n");
  client.print(String("Content-Type: multipart/form-data; boundary=") + boundary + "\r\n");
  client.print(String("Content-Length: ") + String(contentLen) + "\r\n");
  client.print("Connection: close\r\n\r\n");

  client.print(pre);

  uint8_t buf[1024];
  while (true) {
    int n = f.read(buf, sizeof(buf));
    if (n <= 0) break;
    client.write(buf, n);
    delay(0);
  }
  f.close();

  client.print(post);

  String status = client.readStringUntil('\n');
  status.trim();

  int code = 0;
  if (status.startsWith("HTTP/")) {
    int sp1 = status.indexOf(' ');
    if (sp1 > 0) {
      int sp2 = status.indexOf(' ', sp1 + 1);
      if (sp2 > sp1) code = status.substring(sp1 + 1, sp2).toInt();
    }
  }

  wigleLastHttpCode = code;

  while (client.connected()) {
    while (client.available()) client.read();
    delay(0);
  }
  client.stop();

  if (code == 200) {
    uploadLastResult = "Uploaded OK (200)";
    return true;
  }

  uploadLastResult = "Upload failed (" + String(code) + ")";
  return false;
}

// ---------------- TFT UI forward declarations ----------------
static const char* pickSplashSlogan();
static void tftSplashAnimateOnce(const char* slogan, uint32_t animMs = 1600, uint32_t frameMs = 35);
static void tftWigleUploadScreen(uint32_t done, uint32_t total, const String& filename);

// used earlier during uploads to force a clean repaint
static void forceStatusFullRedraw();

static uint32_t uploadAllCsvsToWigle() {
  if (!sdOk) { uploadLastResult = "SD not OK"; return 0; }

  uploadPausedScanWasEnabled = scanningEnabled;
  scanningEnabled = false;

  uploading = true;
  uploadDoneFiles = 0;
  uploadTotalFiles = 0;
  uploadCurrentFile = "";

  digitalWrite(PINS.tft_cs, HIGH);
  File root = SD.open("/logs");
  if (root) {
    File f = root.openNextFile();
    while (f) {
      String name = normalizeSdPath("/logs", f.name());
      bool isCsv = name.endsWith(".csv");
      bool isCurrent = (currentCsvPath.length() && name == currentCsvPath);
      if (isCsv && !isCurrent) uploadTotalFiles++;
      f.close();
      f = root.openNextFile();
    }
    root.close();
  }

  if (uploadTotalFiles == 0) {
    uploading = false;
    scanningEnabled = uploadPausedScanWasEnabled;
    uploadLastResult = "No CSVs to upload";
    return 0;
  }

  // Skip token pre-check to avoid 30-45s delay at start of batch upload.
  // If token is invalid, first upload will fail with 401/403.
  // Note: wigleTokenStatus may be stale; web UI still validates on demand.

  std::vector<String> paths;
  paths.reserve(uploadTotalFiles + 4);

  digitalWrite(PINS.tft_cs, HIGH);
  root = SD.open("/logs");
  if (root) {
    File f = root.openNextFile();
    while (f) {
      String path = normalizeSdPath("/logs", f.name());
      bool isCsv = path.endsWith(".csv");
      bool isCurrent = (currentCsvPath.length() && path == currentCsvPath);
      f.close();
      if (isCsv && !isCurrent) paths.push_back(path);
      f = root.openNextFile();
    }
    root.close();
  }

  uint32_t okCount = 0;
for (size_t i = 0; i < paths.size(); i++) {
  uploadCurrentFile = paths[i];

  // Show upload UI BEFORE each file
  tftWigleUploadScreen(uploadDoneFiles, uploadTotalFiles, pathBasename(uploadCurrentFile));

  bool ok = uploadFileToWigle(paths[i]);

  if (ok) {
    okCount++;
    moveToUploaded(paths[i]);
  }

  uploadDoneFiles++;

  // Show upload UI AFTER each file (progress advances)
  tftWigleUploadScreen(uploadDoneFiles, uploadTotalFiles, pathBasename(uploadCurrentFile));

  delay(0);

  forceStatusFullRedraw();
}
prevUploadMsg = "";          // force a repaint of final message
prevUploadDone = 0xFFFFFFFF; // force repaint next time
prevUploadTotal = 0xFFFFFFFF;

  uploading = false;
  scanningEnabled = uploadPausedScanWasEnabled;
  uploadCurrentFile = "";

  uploadLastResult = "Uploaded " + String(okCount) + "/" + String(uploadTotalFiles);
  return okCount;
}

// ---------------- Web UI ----------------
static const char INDEX_HTML[] PROGMEM = R"HTML(
<!doctype html>
<html>
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Piglet Wardriver (Waveshare C6 1.47)</title>
<style>
  :root{
    --bg:#0b0f14;
    --card:#121925;
    --text:#e6edf6;
    --muted:#9fb0c3;
    --border:#253043;
    --input:#0f1622;
    --inputBorder:#2b3a52;
    --btn:#1d2a3d;
    --btnHover:#253650;
    --btnText:#e6edf6;
    --code:#0f1622;
    --ok:#2dd4bf;
    --bad:#fb7185;
    --warn:#fbbf24;
    --bar:#e6edf6;
    --barBg:#0f1622;
  }
  html{ color-scheme: dark; }
  body{
    font-family:system-ui,-apple-system,Segoe UI,Roboto,Arial,sans-serif;
    margin:16px;
    max-width:900px;
    background:var(--bg);
    color:var(--text);
  }
  h2,h3,h4{ color:var(--text); }
  .card{
    border:1px solid var(--border);
    border-radius:12px;
    padding:14px;
    margin:12px 0;
    background:var(--card);
    box-shadow: 0 6px 24px rgba(0,0,0,.25);
  }
  input,select,button,pre{
    padding:10px;
    border-radius:10px;
    border:1px solid var(--inputBorder);
    width:100%;
    box-sizing:border-box;
    background:var(--input);
    color:var(--text);
  }
  label{ color:var(--muted); }
  button{
    cursor:pointer;
    background:var(--btn);
    border:1px solid var(--inputBorder);
    color:var(--btnText);
    transition: background .12s ease, transform .05s ease;
  }
  button:hover{ background:var(--btnHover); }
  button:active{ transform: translateY(1px); }
  button:disabled{ opacity:.55; cursor:not-allowed; }
  a{ color:var(--ok); text-decoration:none; }
  a:hover{ text-decoration:underline; }
  .row{
    display:grid;
    grid-template-columns:1fr 1fr;
    gap:12px
  }
  code{
    background:var(--code);
    padding:2px 6px;
    border-radius:6px;
    border:1px solid var(--border);
  }
  .barWrap{
    height:10px;
    border:1px solid var(--border);
    border-radius:8px;
    overflow:hidden;
    margin-top:8px;
    background:var(--barBg);
  }
  #wigleBar{
    height:10px;
    width:0%;
    background:var(--bar);
  }
  .ok{ color:var(--ok); }
  .bad{ color:var(--bad); }
  .warn{ color:var(--warn); }
  .muted{ color:var(--muted); }
  .statusGrid{
    display:flex;
    flex-wrap:wrap;
    gap:8px;
    margin:8px 0 12px 0;
  }
  .pill{
    display:inline-flex;
    align-items:center;
    gap:8px;
    border:1px solid var(--border);
    border-radius:999px;
    padding:6px 10px;
    font-size:14px;
    background:var(--input);
    color:var(--text);
  }
  .pill.ok{ border-color: rgba(45,212,191,.6); }
  .pill.bad{ border-color: rgba(251,113,133,.6); }
  .pill.warn{ border-color: rgba(251,191,36,.6); }
  .kv{
    display:grid;
    grid-template-columns:1fr;
    gap:8px;
  }
  @media (min-width:700px){
    .kv{ grid-template-columns:1fr 1fr; }
  }
  .kv > div{
    display:flex;
    justify-content:space-between;
    gap:12px;
    border:1px solid var(--border);
    border-radius:10px;
    padding:10px 12px;
    background:var(--input);
  }
  .k{ color:var(--muted); }
  .v{ font-weight:600; color:var(--text); }
</style>
</head>
<body>
  <h2>Piglet Wardriver (Waveshare ESP32-C6 1.47")</h2>

  <div class="card">
    <h3>Status</h3>

    <div class="statusGrid">
      <div class="pill" id="pillScan">Scan: —</div>
      <div class="pill" id="pillSd">SD: —</div>
      <div class="pill" id="pillGps">GPS: —</div>
      <div class="pill" id="pillSta">STA: —</div>
      <div class="pill" id="pillWigle">WiGLE: —</div>
    </div>

    <div class="kv">
      <div><span class="k">2.4G Found</span><span class="v" id="vFound2g">—</span></div>
      <div><span class="k">STA IP</span><span class="v" id="vStaIp">—</span></div>
      <div><span class="k">AP Clients Seen</span><span class="v" id="vApSeen">—</span></div>
      <div><span class="k">De-dupe</span><span class="v" id="vDedup">—</span></div>
      <div><span class="k">Last Upload</span><span class="v" id="vLastUpload">—</span></div>
    </div>

    <details style="margin-top:10px">
      <summary style="cursor:pointer">Advanced</summary>
       <div class="kv" style="margin-top:8px">
        <div><span class="k">Scan Mode</span><span class="v" id="vScanMode">—</span></div>
        <div><span class="k">GPS Baud</span><span class="v" id="vGpsBaud">—</span></div>
        <div><span class="k">Home SSID</span><span class="v" id="vHomeSsid">—</span></div>
        <div><span class="k">Wardriver SSID</span><span class="v" id="vApSsid">—</span></div>
      </div>
      <div style="margin-top:8px">
        <a href="/status.json" target="_blank" rel="noopener">Open raw status.json</a>
      </div>
    </details>

    <div class="row" style="margin-top:12px">
      <button onclick="fetch('/start',{method:'POST'}).then(loadStatus)">Start Scan</button>
      <button onclick="fetch('/stop',{method:'POST'}).then(loadStatus)">Stop Scan</button>
    </div>

    <div class="row" style="margin-top:10px">
      <button onclick="wigleTest()">Test WiGLE Token</button>
      <button onclick="wigleUploadAll()">Upload all CSVs to WiGLE</button>
    </div>

    <div class="card" style="margin-top:12px">
      <h4 style="margin:0 0 8px 0">WiGLE</h4>
      <div id="wigleMsg" class="muted">—</div>
      <div class="barWrap">
        <div id="wigleBar"></div>
      </div>
    </div>
  </div>

  <div class="card">
    <h3>Config</h3>
    <div class="row">
      <div><label>WiGLE Basic Token (Encoded for Use)</label><input id="wigleBasicToken" placeholder="BASE64(apiName:apiToken)"></div>
      <div><label>GPS Baud</label><input id="gpsBaud" type="number" value="9600"></div>
      <div><label>Home SSID</label><input id="homeSsid"></div>
      <div><label>Home PSK</label><input id="homePsk" type="password"></div>
      <div><label>Wardriver SSID</label><input id="wardriverSsid"></div>
      <div><label>Wardriver PSK</label><input id="wardriverPsk" type="password"></div>
      <div><label>Scan Mode</label>
        <select id="scanMode">
          <option value="aggressive">aggressive</option>
          <option value="powersaving">powersaving</option>
        </select>
      </div>
      <div><label>Speed Units</label>
        <select id="speedUnits">
          <option value="kmh">km/h</option>
          <option value="mph">mph</option>
        </select>
      </div>
      <div style="display:flex;align-items:flex-end;">
        <button onclick="saveCfg()">Save Config</button>
      </div>
    </div>
    <p style="margin-top:10px;">Config is stored at <code>/wardriver.cfg</code> on the SD card.</p>
  </div>

  <div class="card">
    <h3>SD Files</h3>
    <div id="files">Loading...</div>
  </div>

<script>
function setPill(id, text, cls){
  const el = document.getElementById(id);
  if(!el) return;
  el.classList.remove('ok','bad','warn');
  if (cls) el.classList.add(cls);
  el.textContent = text;
}
function wigleStatusText(s){
  if (s === 1) return "WiGLE: Valid";
  if (s === -1) return "WiGLE: Invalid";
  return "WiGLE: Unknown";
}

async function loadStatus(){
  const r = await fetch('/status.json');
  const j = await r.json();

  setPill('pillScan', `Scan: ${j.allowScan ? 'ACTIVE' : 'PAUSED'}`, j.allowScan ? 'ok' : 'warn');
  setPill('pillSd',  `SD: ${j.sdOk ? 'OK' : 'FAIL'}`, j.sdOk ? 'ok' : 'bad');
  setPill('pillGps', `GPS: ${j.gpsFix ? 'LOCK' : 'NO FIX'}`, j.gpsFix ? 'ok' : 'warn');
  setPill('pillSta', `STA: ${j.wifiConnected ? 'CONNECTED' : 'DISCONNECTED'}`, j.wifiConnected ? 'ok' : 'warn');

  const wigleCls = (j.wigleTokenStatus === 1) ? 'ok' : (j.wigleTokenStatus === -1 ? 'bad' : 'warn');
  setPill('pillWigle', wigleStatusText(j.wigleTokenStatus), wigleCls);

  const setText = (id, val) => {
    const el = document.getElementById(id);
    if (el) el.textContent = (val === undefined || val === null || val === "") ? "—" : String(val);
  };

  setText('vFound2g', j.found2g);
  setText('vStaIp', j.wifiConnected ? (j.staIp || '—') : '—');
  setText('vApSeen', j.apClientsSeen ? 'Yes' : 'No');
  setText('vDedup', `${j.seenCount || 0} seen, ${j.seenCollisions || 0} collisions`);

  const lastUp = j.uploadLastResult
    ? `${j.uploadLastResult} (HTTP ${j.wigleLastHttpCode || '—'})`
    : '—';
  setText('vLastUpload', lastUp);

  setText('vScanMode', (j && j.config && j.config.scanMode) ? j.config.scanMode : '—');
  setText('vGpsBaud',  (j && j.config && j.config.gpsBaud) ? j.config.gpsBaud : '—');
  setText('vHomeSsid', (j && j.config && j.config.homeSsid) ? j.config.homeSsid : '—');
  setText('vApSsid',   (j && j.config && j.config.wardriverSsid) ? j.config.wardriverSsid : '—');

  // IMPORTANT: do NOT auto-fill secrets from status.json (prevents "(set)" overwriting token/psk)
  for (const k of ['gpsBaud','homeSsid','wardriverSsid','wardriverPsk','scanMode','speedUnits']){
    if (j.config && (k in j.config)) {
      const el = document.getElementById(k);
      if (el) el.value = j.config[k];
    }
  }
}

async function loadFiles(){
  const r = await fetch('/files.json'); const j = await r.json();
  const el = document.getElementById('files');
  if(!j.ok){ el.textContent = 'SD not available'; return; }

  el.innerHTML = (j.files || []).map(f => `
    <div style="display:flex;gap:8px;align-items:center;margin:8px 0;flex-wrap:wrap">
      <a href="/download?name=${encodeURIComponent(f.name)}">${f.name}</a>
      <span class="muted">${f.size} bytes</span>
      <button style="width:auto" onclick="wigleUploadOne('${f.name}')">Upload</button>
      <button style="width:auto" onclick="delFile('${f.name}')">Delete</button>
    </div>
  `).join('') || '(no files)';
}
async function delFile(name){
  await fetch('/delete?name='+encodeURIComponent(name), {method:'POST'});
  loadFiles();
}

async function saveCfg(){
  const keys = ['wigleBasicToken','gpsBaud','homeSsid','homePsk','wardriverSsid','wardriverPsk','scanMode','speedUnits'];
  let body = "";
  body += "# Saved from Web UI\n";
  body += "# key=value\n";
  for (const k of keys){
    const el = document.getElementById(k);
    const v = el ? (el.value ?? "") : "";
    body += `${k}=${String(v).replace(/\r?\n/g, " ")}\n`;
  }
  await fetch('/saveConfig', { method:'POST', headers:{'Content-Type':'text/plain'}, body });
  await loadStatus();
  alert('Saved. Reboot device to apply GPS/WiFi changes.');
}

function setWigleMsg(t){ document.getElementById('wigleMsg').textContent = t; }

async function wigleTest(){
  setWigleMsg("Testing token...");
  const r = await fetch('/wigle/test', {method:'POST'});
  const j = await r.json().catch(()=>({ok:false,message:"Bad JSON"}));
  setWigleMsg(j.message || (j.ok ? "OK" : "FAIL"));
  await loadStatus();
}

async function wigleUploadAll(){
  setWigleMsg("Uploading all...");
  await fetch('/wigle/uploadAll', {method:'POST'});
}

async function wigleUploadOne(name){
  setWigleMsg("Uploading " + name + " ...");
  await fetch('/wigle/upload?name='+encodeURIComponent(name), {method:'POST'});
  await loadFiles();
  await loadStatus();
}

async function pollUpload(){
  const r = await fetch('/status.json'); const j = await r.json();
  const uploading = !!j.uploading;
  const done = j.uploadDoneFiles || 0;
  const total = j.uploadTotalFiles || 0;

  let pct = 0;
  if (total > 0) pct = Math.round((done/total)*100);

  const bar = document.getElementById('wigleBar');
  bar.style.width = pct + "%";
  bar.style.background = uploading ? "#333" : "#999";

  if (uploading){
    setWigleMsg(`Uploading... ${done}/${total} (${pct}%)`);
  } else if (j.uploadLastResult){
    setWigleMsg(j.uploadLastResult);
  }
}
setInterval(pollUpload, 1000);

loadStatus(); loadFiles();
</script>
</body>
</html>
)HTML";

static void handleRoot() { server.send(200, "text/html", INDEX_HTML); }

static void handleStatus() {
  DynamicJsonDocument doc(1024);
  bool allowScan = scanningEnabled && sdOk && (userScanOverride || !autoPaused);

  doc["scanningEnabled"] = scanningEnabled;
  doc["allowScan"] = allowScan;
  doc["userScanOverride"] = userScanOverride;
  doc["autoPaused"] = autoPaused;

  doc["sdOk"] = sdOk;
  doc["gpsFix"] = gpsHasFix;

  doc["found2g"] = networksFound2G;
  doc["found5g"] = 0;
  doc["wifiConnected"] = (WiFi.status() == WL_CONNECTED);
  doc["staIp"] = (WiFi.status() == WL_CONNECTED) ? WiFi.localIP().toString() : "";
  doc["apClientsSeen"] = apClientSeen;

  doc["seenCount"] = seenCount;
  doc["seenCollisions"] = seenCollisions;
  doc["seenTableSize"] = SEEN_TABLE_SIZE;

  doc["uploading"] = uploading;
  doc["uploadTotalFiles"] = uploadTotalFiles;
  doc["uploadDoneFiles"] = uploadDoneFiles;
  doc["uploadCurrentFile"] = uploadCurrentFile;
  doc["uploadLastResult"] = uploadLastResult;
  doc["wigleTokenStatus"] = wigleTokenStatus;
  doc["wigleLastHttpCode"] = wigleLastHttpCode;

  JsonObject c = doc.createNestedObject("config");
  c["wigleBasicToken"] = cfg.wigleBasicToken.length() ? "(set)" : "";
  c["homeSsid"] = cfg.homeSsid;
  c["homePsk"] = cfg.homePsk.length() ? "(set)" : "";
  c["wardriverSsid"] = cfg.wardriverSsid;
  c["wardriverPsk"] = cfg.wardriverPsk;
  c["gpsBaud"] = cfg.gpsBaud;
  c["scanMode"] = cfg.scanMode;
  c["speedUnits"] = cfg.speedUnits;

  String out;
  serializeJsonPretty(doc, out);
  server.send(200, "application/json", out);
}

static void addDirFiles(JsonArray arr, const char* dir) {
  digitalWrite(PINS.tft_cs, HIGH);
  File root = SD.open(dir);
  if (!root) return;

  File f = root.openNextFile();
  while (f) {
    String fullPath = normalizeSdPath(dir, f.name());
    if (fullPath.length() == 0) { f.close(); f = root.openNextFile(); continue; }

    JsonObject o = arr.createNestedObject();
    o["name"] = fullPath;
    o["size"] = (uint32_t)f.size();

    f.close();
    f = root.openNextFile();
  }
  root.close();
}

static void handleFiles() {
  DynamicJsonDocument doc(4096);
  doc["ok"] = sdOk;

  JsonArray arr = doc.createNestedArray("files");
  if (sdOk) {
    addDirFiles(arr, "/logs");
    addDirFiles(arr, "/uploaded");
  }

  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

static bool isAllowedDataPath(const String& p) {
  return p.startsWith("/logs/") || p.startsWith("/uploaded/");
}

static void handleDownload() {
  if (!sdOk) { server.send(500, "text/plain", "SD not available"); return; }
  if (!server.hasArg("name")) { server.send(400, "text/plain", "Missing name"); return; }
  String name = server.arg("name");
  if (!isAllowedDataPath(name)) { server.send(403, "text/plain", "Forbidden"); return; }
  digitalWrite(PINS.tft_cs, HIGH);
  if (!SD.exists(name)) { server.send(404, "text/plain", "Not found"); return; }

  digitalWrite(PINS.tft_cs, HIGH);
  File f = SD.open(name, FILE_READ);
  server.streamFile(f, "text/csv");
  f.close();
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

static void handleStart() {
  scanningEnabled = true;
  userScanOverride = true;
  server.send(200, "text/plain", "OK");
}

static void handleStop() {
  scanningEnabled = false;
  userScanOverride = true;
  server.send(200, "text/plain", "OK");
}

static void handleSaveConfig() {
  if (!sdOk) { server.send(500, "text/plain", "SD not available"); return; }

  String body = server.arg("plain");
  body.trim();
  if (body.length() == 0) { server.send(400, "text/plain", "Empty body"); return; }

  bool any = false;

  if (body[0] == '{') {
    DynamicJsonDocument doc(1024);
    DeserializationError err = deserializeJson(doc, body);
    if (err) { server.send(400, "text/plain", "Bad JSON"); return; }

    cfg.wigleBasicToken = doc["wigleBasicToken"] | cfg.wigleBasicToken;
    cfg.homeSsid        = doc["homeSsid"]        | cfg.homeSsid;
    cfg.homePsk         = doc["homePsk"]         | cfg.homePsk;
    cfg.wardriverSsid   = doc["wardriverSsid"]   | cfg.wardriverSsid;
    cfg.wardriverPsk    = doc["wardriverPsk"]    | cfg.wardriverPsk;
    cfg.gpsBaud         = doc["gpsBaud"]         | cfg.gpsBaud;
    cfg.scanMode        = doc["scanMode"]        | cfg.scanMode;
    cfg.speedUnits      = doc["speedUnits"]      | cfg.speedUnits;

    any = true;
  } else {
    int pos = 0;
    while (pos < body.length()) {
      int nl = body.indexOf('\n', pos);
      if (nl < 0) nl = body.length();
      String line = body.substring(pos, nl);
      pos = nl + 1;

      String k, v;
      if (parseKeyValueLine(line, k, v)) {
        cfgAssignKV(k, v);
        any = true;
      }
    }
  }

  if (!any) { server.send(400, "text/plain", "No valid key=value lines"); return; }

  Serial.println("[CFG] Updated config from Web UI (in-RAM). Saving to SD...");
  bool ok = saveConfigToSD();
  server.send(ok ? 200 : 500, "text/plain", ok ? "OK" : "FAIL");
}

static void handleWigleTest() {
  bool ok = wigleTestToken();

  DynamicJsonDocument doc(384);
  doc["ok"] = ok;
  doc["tokenStatus"] = wigleTokenStatus;
  doc["httpCode"] = wigleLastHttpCode;
  doc["message"] = uploadLastResult;

  String out;
  serializeJson(doc, out);
  server.send(ok ? 200 : 400, "application/json", out);
}

static void handleWigleUploadAll() {
  if (!sdOk) { server.send(500, "text/plain", "SD not available"); return; }
  if (WiFi.status() != WL_CONNECTED) { server.send(400, "text/plain", "STA WiFi not connected"); return; }

  uint32_t okCount = uploadAllCsvsToWigle();

  DynamicJsonDocument doc(384);
  doc["ok"] = (okCount > 0);
  doc["uploaded"] = okCount;
  doc["total"] = uploadTotalFiles;
  doc["message"] = uploadLastResult;

  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

static void handleWigleUploadOne() {
  if (!sdOk) { server.send(500, "text/plain", "SD not available"); return; }
  if (WiFi.status() != WL_CONNECTED) { server.send(400, "text/plain", "STA WiFi not connected"); return; }
  if (!server.hasArg("name")) { server.send(400, "text/plain", "Missing name"); return; }

  String path = server.arg("name");
  digitalWrite(PINS.tft_cs, HIGH);
  if (!SD.exists(path)) { server.send(404, "text/plain", "Not found"); return; }

  uploading = true;
  uploadPausedScanWasEnabled = scanningEnabled;
  scanningEnabled = false;

  uploadTotalFiles = 1;
  uploadDoneFiles = 0;
  uploadCurrentFile = path;

  bool ok = uploadFileToWigle(path);

  uploadDoneFiles = 1;

  uploading = false;
  scanningEnabled = uploadPausedScanWasEnabled;
  uploadCurrentFile = "";
  forceStatusFullRedraw();
  if (ok) moveToUploaded(path);

  DynamicJsonDocument doc(384);
  doc["ok"] = ok;
  doc["httpCode"] = wigleLastHttpCode;
  doc["message"] = uploadLastResult;

  String out;
  serializeJson(doc, out);
  server.send(ok ? 200 : 500, "application/json", out);
}

static void startWebServer() {
  Serial.println("[WEB] Starting web server routes...");

  server.on("/", handleRoot);
  server.on("/status.json", handleStatus);
  server.on("/files.json", handleFiles);
  server.on("/download", handleDownload);
  server.on("/delete", HTTP_POST, handleDelete);
  server.on("/start",  HTTP_POST, handleStart);
  server.on("/stop",   HTTP_POST, handleStop);
  server.on("/saveConfig", HTTP_POST, handleSaveConfig);

  server.on("/wigle/test", HTTP_POST, handleWigleTest);
  server.on("/wigle/uploadAll", HTTP_POST, handleWigleUploadAll);
  server.on("/wigle/upload", HTTP_POST, handleWigleUploadOne);

  server.begin();
  Serial.println("[WEB] Server started");
}

// ---------------- Boot WiFi logic ----------------
static void startAP() {
  Serial.println("[WIFI] Starting SoftAP...");
  WiFi.mode(WIFI_AP_STA);

  bool ok = WiFi.softAP(cfg.wardriverSsid.c_str(), cfg.wardriverPsk.c_str());
  apStartMs = millis();
  apClientSeen = false;
  apWindowActive = true;

  Serial.print("[WIFI] AP SSID: ");
  Serial.println(cfg.wardriverSsid);
  Serial.print("[WIFI] AP PSK:  ");
  Serial.println(cfg.wardriverPsk.length() ? "(set)" : "(open)");
  Serial.print("[WIFI] AP start: ");
  Serial.println(ok ? "OK" : "FAIL");
  Serial.print("[WIFI] AP IP: ");
  Serial.println(WiFi.softAPIP());
}

static void forceDnsEspNetif(IPAddress dns1, IPAddress dns2) {
  esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  if (!netif) {
    Serial.println("[NET] esp_netif WIFI_STA_DEF not found");
    return;
  }

  esp_netif_dns_info_t info;

  memset(&info, 0, sizeof(info));
  info.ip.type = ESP_IPADDR_TYPE_V4;
  info.ip.u_addr.ip4.addr = (uint32_t)dns1;
  esp_netif_set_dns_info(netif, ESP_NETIF_DNS_MAIN, &info);

  memset(&info, 0, sizeof(info));
  info.ip.type = ESP_IPADDR_TYPE_V4;
  info.ip.u_addr.ip4.addr = (uint32_t)dns2;
  esp_netif_set_dns_info(netif, ESP_NETIF_DNS_BACKUP, &info);

  Serial.print("[NET] esp_netif forced DNS MAIN="); Serial.println(dns1);
  Serial.print("[NET] esp_netif forced DNS BACKUP="); Serial.println(dns2);
}

static void fixDnsIfNeeded() {
  IPAddress dns = WiFi.dnsIP();
  IPAddress gw  = WiFi.gatewayIP();

  Serial.print("[NET] DHCP DNS: "); Serial.println(dns);
  Serial.print("[NET] Gateway : "); Serial.println(gw);

  bool badDns = (dns == IPAddress(0,0,0,0)) || (dns == gw);

  IPAddress dns1(1,1,1,1);
  IPAddress dns2(8,8,8,8);

  if (badDns) {
    Serial.println("[NET] DNS looks bad (0.0.0.0 or equals gateway). Forcing public DNS...");
    WiFi.config(INADDR_NONE, INADDR_NONE, INADDR_NONE, dns1, dns2);
    delay(100);

    IPAddress after = WiFi.dnsIP();
    bool stillBad = (after == IPAddress(0,0,0,0)) || (after == WiFi.gatewayIP());

    if (stillBad) {
      Serial.println("[NET] WiFi.config DNS did not stick; forcing via esp_netif...");
      forceDnsEspNetif(dns1, dns2);
      delay(100);
    }

    Serial.print("[NET] DNS now: "); Serial.println(WiFi.dnsIP());
  }
}

static bool connectSTA(uint32_t timeoutMs) {
  Serial.println("[WIFI] STA connect attempt...");
  if (cfg.homeSsid.length() == 0) {
    Serial.println("[WIFI] No home SSID in config");
    return false;
  }

  Serial.print("[WIFI] Home SSID: ");
  Serial.println(cfg.homeSsid);

  IPAddress dns1(1,1,1,1);
  IPAddress dns2(8,8,8,8);
  WiFi.config(INADDR_NONE, INADDR_NONE, INADDR_NONE, dns1, dns2);
  delay(50);

  WiFi.begin(cfg.homeSsid.c_str(), cfg.homePsk.c_str());

  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - start) < timeoutMs) {
    delay(200);
    if (WiFi.softAPgetStationNum() > 0) apClientSeen = true;
    Serial.print(".");
  }
  Serial.println();

  bool ok = (WiFi.status() == WL_CONNECTED);
  Serial.print("[WIFI] STA connect: ");
  Serial.println(ok ? "OK" : "FAIL");

  if (ok) {
    Serial.print("[WIFI] STA IP: ");
    Serial.println(WiFi.localIP());
    Serial.print("[WIFI] RSSI: ");
    Serial.println(WiFi.RSSI());
    fixDnsIfNeeded();
  }

  return ok;
}

static void stopAPIfAllowed() {
  if (!apWindowActive) return;

  if ((millis() - apStartMs) > AP_WINDOW_MS) {
    Serial.printf("[WIFI] AP window expired (%lu ms). Stopping AP HARD.\n",
                  (unsigned long)AP_WINDOW_MS);

    WiFi.softAPdisconnect(true);
    apWindowActive = false;

    WiFi.setAutoReconnect(false);
    WiFi.persistent(false);
    WiFi.disconnect(true, true);
    delay(50);

    if (WiFi.status() != WL_CONNECTED) WiFi.mode(WIFI_STA);

    if (!(userScanOverride && !scanningEnabled)) {
      scanningEnabled = true;
      Serial.println("[SCAN] AP timed out -> scanning re-enabled");
    } else {
      Serial.println("[SCAN] AP timed out -> scanning remains OFF (user override)");
    }
  }
}

static void handleStaTransitions() {
  wl_status_t now = WiFi.status();

  // If STA dropped, allow scanning to resume (unless user forced it OFF)
  if (lastStaStatus == WL_CONNECTED && now != WL_CONNECTED) {
    Serial.println("[WIFI] STA disconnected -> scanning can resume");
    if (!(userScanOverride && !scanningEnabled)) {
      scanningEnabled = true;
    }
  }

  // Repaint clean when WiFi state flips
  static wl_status_t prev = WL_IDLE_STATUS;
  if (prev != now) {
    forceStatusFullRedraw();
  }
  prev = now;

  lastStaStatus = now;
}

static bool shouldPauseScanning() {
  if (WiFi.status() == WL_CONNECTED) return true;
  wifi_mode_t m = WiFi.getMode();
  if (m == WIFI_AP || m == WIFI_AP_STA) return true;
  return false;
}

// ---------------- Wardriving scan loop ----------------
static void doScanOnce() {
  static uint32_t lastScanMs = 0;
  uint32_t interval = (cfg.scanMode == "powersaving") ? 12000 : 4500;
  if (millis() - lastScanMs < interval) return;

  Serial.print("[SCAN] Starting WiFi scan (mode=");
  Serial.print(cfg.scanMode);
  Serial.println(")...");

  uint32_t t0 = millis();
  int n = WiFi.scanNetworks(false, true);
  static uint8_t zeroScanCount = 0;

  lastScanMs = millis();

  if (n <= 0) {
    zeroScanCount++;
    Serial.printf("[SCAN] EMPTY (%u) — resetting WiFi if stuck\n", zeroScanCount);
    WiFi.scanDelete();

    if (zeroScanCount >= 3) {
      Serial.println("[SCAN] Performing WiFi radio reset (C6 recovery)");
      WiFi.mode(WIFI_OFF);
      delay(200);
      WiFi.mode(WIFI_STA);
      delay(200);
      zeroScanCount = 0;
    }
    return;
  }

  zeroScanCount = 0;
  uint32_t dt = millis() - t0;
  Serial.printf("[SCAN] scanNetworks() returned %d in %lu ms\n", n, (unsigned long)dt);

  String firstSeen = iso8601NowUTC();

  double lat = 0, lon = 0, altM = 0, accM = 0;
  if (gpsHasFix) {
    lat = gps.location.lat();
    lon = gps.location.lng();
    altM = gps.altitude.meters();
    accM = gps.hdop.hdop();
  }

  uint32_t wrote = 0;

  for (int i = 0; i < n; i++) {
    int ch = WiFi.channel(i);
    bool chUnknown = (ch == 0);
    bool is2g = (ch >= 1 && ch <= 14) || chUnknown;
    if (!is2g) continue; // C6: 2.4GHz only in this project

    String ssid = WiFi.SSID(i);
    String mac  = WiFi.BSSIDstr(i);

    uint64_t key = bssidStrToKey48(mac);
    if (seenCheckOrInsert(key)) continue;

    int rssi = WiFi.RSSI(i);
    wifi_auth_mode_t auth = WiFi.encryptionType(i);
    String authStr = authModeToString(auth);

    networksFound2G++;
    appendWigleRow(mac, ssid, authStr, firstSeen, ch, rssi, lat, lon, altM, accM);
    wrote++;
  }

  WiFi.scanDelete();

  Serial.print("[SCAN] Wrote rows: ");
  Serial.println(wrote);
}

// ---------------- Button ----------------
static void pollButton() {
  static uint32_t lastDebounce = 0;
  static int lastState = HIGH;
  static bool latched = false;

  int s = digitalRead(PINS.btn);

  if (s != lastState) {
    lastDebounce = millis();
    lastState = s;
  }

  if ((millis() - lastDebounce) > 40) {
    if (s == LOW) {
      if (!latched) {
        scanningEnabled = !scanningEnabled;
        userScanOverride = true;
        Serial.print("[BTN] Scanning toggled -> ");
        Serial.println(scanningEnabled ? "ACTIVE" : "PAUSED");
        latched = true;
      }
    } else {
      latched = false;
    }
  }
}

// ---------------- TFT wiring + UI ----------------
//
// NOTE: LovyanGFX + SD share the same Arduino SPI backend.
// We keep TFT deselected (CS HIGH) during SD operations.

// ---------- Pixel grid helpers ----------
static const char* headingToDir8(float deg) {
  // 8-point compass
  // 0=N, 45=NE, 90=E, ...
  static const char* dirs[] = {"N","NE","E","SE","S","SW","W","NW"};
  // normalize
  while (deg < 0) deg += 360.0f;
  while (deg >= 360.0f) deg -= 360.0f;
  int idx = (int)((deg + 22.5f) / 45.0f) & 7;
  return dirs[idx];
}

static void drawGpsIcon(int x, int y, int sats) {
  tft.drawCircle(x, y, 6, WHITE);
  if (sats > 0) tft.fillCircle(x, y, 2, WHITE);
}

static void drawSdPulse(int x, int y) {
  if (uiSdPulse) {
    tft.fillCircle(x, y, 3, WHITE);
    if (millis() - uiLastPulse > 120) uiSdPulse = false;
  } else {
    tft.drawCircle(x, y, 3, WHITE);
  }
}

static void drawDirectionArrow(int cx, int cy, float deg, int sizePx = 10) {
  // TinyGPS course: 0=N,90=E,180=S,270=W
  // Screen coords: +x right, +y down. Make 0=N point UP:
  float rad = (deg - 90.0f) * 0.0174533f;

  int tip = sizePx;
  int base = (int)(sizePx * 0.60f);

  int x1 = cx + (int)(cos(rad) * tip);
  int y1 = cy + (int)(sin(rad) * tip);

  int x2 = cx + (int)(cos(rad + 2.6f) * base);
  int y2 = cy + (int)(sin(rad + 2.6f) * base);

  int x3 = cx + (int)(cos(rad - 2.6f) * base);
  int y3 = cy + (int)(sin(rad - 2.6f) * base);

  tft.fillTriangle(x1, y1, x2, y2, x3, y3, 0xF800); // RED (RGB565)
}

// ---------------- TFT (LovyanGFX / ST7789) ----------------
// tft object declared near top of file

static void tftBacklight(bool on) {
  // LovyanGFX backlight brightness (0-255)
  tft.setBrightness(on ? 255 : 0);
}

static void tftInit() {
  // Keep TFT deselected during init to avoid upsetting SD on shared SPI
  pinMode(PINS.tft_cs, OUTPUT);
  digitalWrite(PINS.tft_cs, HIGH);

  tft.init();
  tft.setRotation(0);

  // Force BLACK background + WHITE text everywhere
  if (uiFirstDraw) {
  tft.fillScreen(BLACK);
  uiFirstDraw = false;
}
  tft.setTextColor(WHITE, BLACK);
  tft.setTextSize(1);
  tft.setCursor(0, 0);
}



static void tftProgressBar(int x, int y, int w, int h, float pct) {
  if (pct < 0) pct = 0;
  if (pct > 1) pct = 1;
  tft.drawRect(x, y, w, h, WHITE);
  int fill = (int)((w - 2) * pct);
  if (fill > 0) tft.fillRect(x + 1, y + 1, fill, h - 2, WHITE);
}

static void updateTFT(float speedValue) {  int W = tft.width();
if (uiFirstDraw) {
  tft.fillScreen(BLACK);
  tft.setTextColor(WHITE, BLACK);
  uiFirstDraw = false;

  // Also reset cursor/text size expectations
  tft.setTextSize(1);
}
  bool allowScan = scanningEnabled && sdOk && (userScanOverride || !autoPaused);

// --- partial redraw background clear ---
// Only clear dynamic regions instead of whole screen


tft.setTextColor(WHITE, BLACK);

  // ---- Header ----
  tft.setTextSize(2);
  tft.setCursor(10, 10);
  tft.print("Piglet");
  // ----- top-right status icons -----
  int right = tft.width() - 10;

  drawGpsIcon(right - 40, 18, uiGpsSats);
  drawSdPulse(right - 60, 18);
  drawDirectionArrow(right - 80, 18, uiHeadingDeg, 10);

  tft.setTextSize(1);

int y = 50;
const int line = 16;

auto repaintLine = [&](bool changed, int yPos, const String& txt) {
  if (!changed) return;
  tft.fillRect(0, yPos, tft.width(), line, BLACK);
  tft.setCursor(10, yPos);
  tft.print(txt);
};

// compute current states
bool staNow = (WiFi.status() == WL_CONNECTED);
String ipNow = staNow ? WiFi.localIP().toString() : "";

// repaint only when changed
repaintLine(prevAllowScan != allowScan, y, String("Scan: ") + (allowScan ? "ACTIVE" : "PAUSED"));
prevAllowScan = allowScan; y += line;

repaintLine(prevSdOk != sdOk, y, String("SD: ") + (sdOk ? "OK" : "FAIL"));
prevSdOk = sdOk; y += line;

repaintLine(prevGpsFix != gpsHasFix, y, String("GPS: ") + (gpsHasFix ? "LOCK" : "NO FIX"));
prevGpsFix = gpsHasFix; y += line;

repaintLine(prevSta != staNow, y, String("STA: ") + (staNow ? "YES" : "NO"));
prevSta = staNow; y += line;

// IP/AP line
if (prevIp != ipNow) {
  tft.fillRect(0, y, tft.width(), line, BLACK);
  tft.setCursor(10, y);

  if (staNow) {
    tft.print("IP: ");
    tft.print(ipNow);
  } else if (apWindowActive) {
    uint32_t remaining = (AP_WINDOW_MS - (millis() - apStartMs) + 999) / 1000;
    tft.print("AP: 192.168.4.1 ");
    tft.print(remaining);
    tft.print("s");
  } else {
    tft.print("AP: OFF");
  }

  prevIp = ipNow;
}

y += line;   // <-- add this so the upload area starts below the IP line

// ---------------- WiGLE upload status area ----------------
if (uploading) {
  if (prevUploadDone != uploadDoneFiles || prevUploadTotal != uploadTotalFiles) {
    tft.fillRect(0, y, tft.width(), 80, BLACK);

    tft.setCursor(10, y);
    tft.print("Uploading ");
    tft.print(uploadDoneFiles);
    tft.print("/");
    tft.print(uploadTotalFiles);

    float pct = (uploadTotalFiles == 0) ? 0.0f : ((float)uploadDoneFiles / (float)uploadTotalFiles);
    tftProgressBar(10, y + 20, 150, 10, pct);

    prevUploadDone = uploadDoneFiles;
    prevUploadTotal = uploadTotalFiles;
  }
} else {
  if (prevUploadMsg != uploadLastResult) {
    tft.fillRect(0, y, tft.width(), 80, BLACK);

    tft.setCursor(10, y);
    tft.print("WiGLE: ");
    if (wigleTokenStatus == 1) tft.print("Valid");
    else if (wigleTokenStatus == -1) tft.print("Invalid");
    else tft.print("Unknown");

    tft.setCursor(10, y + 20);
    tft.print(uploadLastResult.length() ? uploadLastResult : String("-"));

    prevUploadMsg = uploadLastResult;
  }
}
  // ---------------- Bottom area: 2.4G count + navigation arrow ----------------
  // Give the arrow more breathing room by reserving a bit more height
const int bottomArrowH = 94;                       // extra room for speed under arrow
const int bottomY0     = tft.height() - bottomArrowH;

  // Big 2.4G line placed higher + with padding so it never crowds the arrow block
  const int bigLineH = 26;                           // taller clear area = no digit ghosts
  const int bigLinePad = 10;                         // gap between big line and arrow block
  const int bigLineY = bottomY0 - bigLineH - bigLinePad;

  // Redraw ONLY when count changes (or full redraw), and always clear a fixed rectangle first
  if ((prevFound2G != networksFound2G) || uiFirstDraw) {
    tft.fillRect(0, bigLineY, tft.width(), bigLineH, BLACK);
    tft.setTextColor(WHITE, BLACK);
    tft.setTextSize(2);

    // small vertical inset so glyphs don't touch the cleared area's edge
    tft.setCursor(10, bigLineY + 3);
    tft.print("2.4G: ");
    tft.print(networksFound2G);

    tft.setTextSize(1);
    prevFound2G = networksFound2G;
  }

  // Navigation arrow (bigger, red) + direction label underneath
  const int navCx = tft.width() / 2;
  const int navCy = bottomY0 + 24;                   // slightly lower inside the reserved block

  // Only redraw nav if heading/label changed, or on full redraw, OR speed changed meaningfully
  bool speedChanged = (fabs(prevSpeed - speedValue) > 0.1f);

  if (uiFirstDraw || fabs(prevNavHeading - uiHeadingDeg) > 0.5f || prevNavTxt != uiHeadingTxt || speedChanged) {
    // clear arrow block only when needed
    tft.fillRect(0, bottomY0, tft.width(), bottomArrowH, BLACK);

    // arrow
    drawDirectionArrow(navCx, navCy, uiHeadingDeg, 18);

    // direction label
    tft.setTextColor(WHITE, BLACK);
    tft.setTextSize(1);
    int labelY = navCy + 20;
    String label = uiHeadingTxt.length() ? uiHeadingTxt : String("—");
    int tw = tft.textWidth(label);
    int lx = (tft.width() - tw) / 2;
    if (lx < 0) lx = 0;
    tft.setCursor(lx, labelY);
    tft.print(label);

    // speed at very bottom (slightly larger than normal text, but not "big counter" style)
    // Note: font sizes are discrete; size=2 is the "slightly larger" step.
    tft.setTextSize(2);

    String sp = String(speedValue, 1) + (cfg.speedUnits == "mph" ? " mph" : " km/h");
    int sw = tft.textWidth(sp);
    int sx = (tft.width() - sw) / 2;
    if (sx < 0) sx = 0;

    // place near the bottom edge with a little padding
    int speedY = bottomY0 + bottomArrowH - 22; // tuned for size=2 height
    tft.setCursor(sx, speedY);
    tft.print(sp);

    // restore defaults
    tft.setTextSize(1);

    prevNavHeading = uiHeadingDeg;
    prevNavTxt     = uiHeadingTxt;
    prevSpeed      = speedValue;
  }
}
static bool initSD_SharedSPI() {
  Serial.println("[SD] initSD_SharedSPI()...");

  // Make sure both devices are deselected
  pinMode(PINS.sd_cs, OUTPUT);  digitalWrite(PINS.sd_cs, HIGH);
  pinMode(PINS.tft_cs, OUTPUT); digitalWrite(PINS.tft_cs, HIGH);

  // Keep TFT quiet during SD init
  pinMode(PINS.tft_rst, OUTPUT);
  digitalWrite(PINS.tft_rst, LOW);
  pinMode(PINS.tft_dc, OUTPUT);
  digitalWrite(PINS.tft_dc, HIGH);

  // MISO pull-up helps when the card tristates
  pinMode(PINS.sd_miso, INPUT_PULLUP);
  delay(20);

  // (Re)start SPI on the shared pins
  SPI.end();
  delay(10);
  SPI.begin(PINS.sd_sck, PINS.sd_miso, PINS.sd_mosi, PINS.sd_cs);
  delay(20);

  // Clean idle time with both CS high
  digitalWrite(PINS.sd_cs, HIGH);
  digitalWrite(PINS.tft_cs, HIGH);
  delay(100);

  // Try a frequency ladder (start conservative)
  const uint32_t freqs[] = { 100000, 200000, 400000, 1000000, 2000000, 4000000, 8000000, 12000000 };
  for (size_t i = 0; i < sizeof(freqs)/sizeof(freqs[0]); i++) {
    uint32_t f = freqs[i];

    SD.end();               // important between attempts on some builds
    delay(20);

    Serial.printf("[SD] SD.begin(CS=%d) @ %lu Hz ... ", PINS.sd_cs, (unsigned long)f);
    bool ok = SD.begin(PINS.sd_cs, SPI, f);
    Serial.println(ok ? "OK" : "FAIL");

    if (ok) {
      uint8_t type = SD.cardType();
      Serial.printf("[SD] cardType=%u (0=none,1=MMC,2=SDSC,3=SDHC)\n", type);

      if (type == CARD_NONE) {
        Serial.println("[SD] Mounted but CARD_NONE? Treating as fail.");
        SD.end();
        continue;
      }

      uint64_t sizeMB = SD.cardSize() / (1024ULL * 1024ULL);
      Serial.printf("[SD] cardSize=%llu MB\n", sizeMB);

      digitalWrite(PINS.tft_cs, HIGH);
      if (!SD.exists("/logs")) { digitalWrite(PINS.tft_cs, HIGH); SD.mkdir("/logs"); }
      digitalWrite(PINS.tft_cs, HIGH);
      if (!SD.exists("/uploaded")) { digitalWrite(PINS.tft_cs, HIGH); SD.mkdir("/uploaded"); }
      return true;
    }
  }

  Serial.println("[SD] All attempts failed.");
  return false;
}

static void forceStatusFullRedraw() {
  uiFirstDraw = true;

  // Reset the diff-caches so all lines repaint
  prevAllowScan   = !prevAllowScan;
  prevSdOk        = !prevSdOk;
  prevGpsFix      = !prevGpsFix;
  prevSta         = !prevSta;
  prevFound2G     = 0xFFFFFFFF;
  prevSpeed       = -9999.0f;
  prevIp          = "";

  prevUploadMsg   = "";
  prevUploadDone  = 0xFFFFFFFF;
  prevUploadTotal = 0xFFFFFFFF;
}

// ---------------- Splash + Upload UI (TFT) ----------------

// Boot slogans (pick once per boot)
static const char* SPLASH_SLOGANS[] = {
  "Makin' Bacon",
  "Magic Smoke..",
  "Ahhhhhhhhhhhhh",
  "BOSH",
  "Oink Oink Oink",
  "T5577's are Blank",
  "Love your Face",
  "NFC Propaganda",
  "Maker Propaganda",
  "Pigs R Friends",
  "Ham Wuz Here",
};
static const size_t SPLASH_SLOGAN_COUNT = sizeof(SPLASH_SLOGANS) / sizeof(SPLASH_SLOGANS[0]);

static const char* pickSplashSlogan() {
  uint32_t r = (uint32_t)esp_random();
  return SPLASH_SLOGANS[r % SPLASH_SLOGAN_COUNT];
}

static void tftDrawCentered(int y, const char* txt, int textSize) {
  if (!txt) return;
  tft.setTextSize(textSize);
  int W = tft.width();
  int tw = tft.textWidth(txt);
  int x = (W - tw) / 2;
  if (x < 0) x = 0;
  tft.setCursor(x, y);
  tft.print(txt);
}

// Draw the Piglet splash header + bar frame
static void tftSplashBase(const char* slogan, int barX, int barY, int barW, int barH) {
  tft.fillScreen(BLACK);
  tft.setTextColor(WHITE, BLACK);

  // Title + credit
  tftDrawCentered(18, "Piglet", 3);
  tftDrawCentered(62, "By Midwest Gadgets", 1);

  // Bar frame
  tft.drawRect(barX, barY, barW, barH, WHITE);

  // Slogan centered inside bar
  if (slogan && slogan[0]) {
    tft.setTextSize(1);
    int tw = tft.textWidth(slogan);
    int tx = barX + (barW - tw) / 2;
    int ty = barY + (barH - tft.fontHeight()) / 2;
    if (tx < barX + 2) tx = barX + 2;
    tft.setCursor(tx, ty);
    tft.print(slogan);
  }
}

// Indeterminate “scanner” animation (like your OLED one)
static void tftSplashAnimateOnce(const char* slogan, uint32_t animMs, uint32_t frameMs) {
  int W = tft.width();
  // Nice proportions for 172-wide screen
  const int BAR_W = (W < 180) ? 150 : 204;
  const int BAR_H = 20;
  const int BAR_X = (W - BAR_W) / 2;
  const int BAR_Y = 96;

  const int innerPad = 2;
  const int innerW   = BAR_W - innerPad * 2;
  const int blockW   = 26;

  int pos = 0;
  int dir = 1;

  uint32_t start = millis();
  while (millis() - start < animMs) {
    // Redraw base each frame (simple + reliable)
    tftSplashBase(slogan, BAR_X, BAR_Y, BAR_W, BAR_H);

    // Clear interior
    tft.fillRect(BAR_X + innerPad, BAR_Y + innerPad, innerW, BAR_H - innerPad * 2, BLACK);

    // Moving block
    tft.fillRect(BAR_X + innerPad + pos, BAR_Y + innerPad, blockW, BAR_H - innerPad * 2, WHITE);

    // Re-draw slogan on top
    if (slogan && slogan[0]) {
      tft.setTextColor(WHITE, BLACK);
      tft.setTextSize(1);
      int tw = tft.textWidth(slogan);
      int tx = BAR_X + (BAR_W - tw) / 2;
      int ty = BAR_Y + (BAR_H - tft.fontHeight()) / 2;
      if (tx < BAR_X + 2) tx = BAR_X + 2;
      tft.setCursor(tx, ty);
      tft.print(slogan);
    }

    // Bounce
    pos += dir * 5;
    if (pos <= 0) { pos = 0; dir = 1; }
    if (pos >= (innerW - blockW)) { pos = innerW - blockW; dir = -1; }

    delay(frameMs);
  }

  // Final filled look (like your OLED “100%” frame)
  tftSplashBase(slogan, BAR_X, BAR_Y, BAR_W, BAR_H);
  tft.fillRect(BAR_X + 2, BAR_Y + 2, BAR_W - 4, BAR_H - 4, WHITE);

  // Slogan inside filled bar (invert text for readability)
  if (slogan && slogan[0]) {
    tft.setTextColor(BLACK, WHITE);
    tft.setTextSize(1);
    int tw = tft.textWidth(slogan);
    int tx = BAR_X + (BAR_W - tw) / 2;
    int ty = BAR_Y + (BAR_H - tft.fontHeight()) / 2;
    if (tx < BAR_X + 2) tx = BAR_X + 2;
    tft.setCursor(tx, ty);
    tft.print(slogan);
    tft.setTextColor(WHITE, BLACK);
  }

  delay(250);
}

// Determinate WiGLE upload screen w/ progress bar + counts
static void tftWigleUploadScreen(uint32_t done, uint32_t total, const String& filename) {
  int W = tft.width();

  const int BAR_W = (W < 180) ? 150 : 204;
  const int BAR_H = 18;
  const int BAR_X = (W - BAR_W) / 2;
  const int BAR_Y = 140;

  float pct = (total == 0) ? 0.0f : (float)done / (float)total;
  if (pct < 0) pct = 0;
  if (pct > 1) pct = 1;

  tft.fillScreen(BLACK);
  tft.setTextColor(WHITE, BLACK);

  tftDrawCentered(18, "Piglet", 3);
  tftDrawCentered(62, "WiGLE Upload", 2);

  // done/total
  {
    char buf[40];
    snprintf(buf, sizeof(buf), "%lu / %lu CSV",
             (unsigned long)done, (unsigned long)total);
    tftDrawCentered(96, buf, 1);
  }

  // progress bar
  tft.drawRect(BAR_X, BAR_Y, BAR_W, BAR_H, WHITE);
  int fillW = (int)((BAR_W - 4) * pct);
  if (fillW > 0) tft.fillRect(BAR_X + 2, BAR_Y + 2, fillW, BAR_H - 4, WHITE);

  // file name (trim)
  String name = filename;
  if (name.length() > 26) name = name.substring(0, 26) + "...";
  tftDrawCentered(BAR_Y + BAR_H + 14, name.c_str(), 1);
  uiFirstDraw = true;   // next status frame must redraw everything
}

// ---------------- Setup / Loop ----------------
void setup() {
  // ESP32-C6 often uses USB CDC; give it time to enumerate
  Serial.begin(115200);
  delay(1200);

  // If the core supports it, wait briefly for Serial to be ready (won't hang forever)
  uint32_t t0 = millis();
  while (!Serial && (millis() - t0) < 2000) { delay(10); }

  Serial.println();
  Serial.println("[BOOT] Serial is alive");
  Serial.printf("[BOOT] Reset reason: %d\n", (int)esp_reset_reason());
  resetSeenTable();
  networksFound2G = 0;
  networksFound5G = 0;

  Serial.println("=== Piglet Wardriver Boot (Waveshare ESP32-C6 LCD 1.47) ===");

  // Button
  pinMode(PINS.btn, INPUT_PULLUP);
  Serial.printf("[BTN] Init OK (GPIO %d, INPUT_PULLUP)\n", PINS.btn);

  // TFT init early (so you see failures fast)
// Bring up shared SPI bus (LCD + SD share SCK/MOSI)
// HARD deselect TFT + SD first
pinMode(PINS.tft_cs, OUTPUT); digitalWrite(PINS.tft_cs, HIGH);
pinMode(PINS.sd_cs,  OUTPUT); digitalWrite(PINS.sd_cs,  HIGH);

// Hold TFT in reset during SD init (prevents any accidental bus activity)
pinMode(PINS.tft_rst, OUTPUT);
digitalWrite(PINS.tft_rst, LOW);

// DC stable
pinMode(PINS.tft_dc, OUTPUT); digitalWrite(PINS.tft_dc, HIGH);
delay(50);

// Start SPI (shared pins)
SPI.begin(PINS.sd_sck, PINS.sd_miso, PINS.sd_mosi);
delay(20);

// ---------------- SD init FIRST ----------------
Serial.println("[SD] Initializing SD on shared SPI (SD-first)...");
Serial.printf("[SD] Pins SCK=%d MISO=%d MOSI=%d CS=%d\n",
              PINS.sd_sck, PINS.sd_miso, PINS.sd_mosi, PINS.sd_cs);

sdOk = initSD_SharedSPI();
if (sdOk && SD.cardType() == CARD_NONE) sdOk = false;
Serial.print("[SD] sdOk="); Serial.println(sdOk ? "true" : "false");

// Load cfg from SD BEFORE using cfg for GPS/WiFi
if (sdOk) {
  loadConfigFromSD();
}

// ---------------- TFT init AFTER SD ----------------
// Release TFT reset now
digitalWrite(PINS.tft_rst, HIGH);
delay(120);

Serial.println("[TFT] Initializing TFT...");
tftBacklight(true);
tftInit();

// Pick slogan ONCE per boot and run the classic splash animation
bootSlogan = pickSplashSlogan();
// ----- fade-in splash -----
for (int b = 0; b <= 255; b += 25) {
  tft.setBrightness(b);
  delay(15);
}
tftSplashAnimateOnce(bootSlogan);
uiFirstDraw = true;   // force full status redraw after splash
digitalWrite(PINS.tft_cs, HIGH);
digitalWrite(PINS.sd_cs,  HIGH);
forceStatusFullRedraw();
updateTFT(0);
if (sdOk) {
  File test = SD.open("/logs");
  if (test) {
    test.close();
    Serial.println("[SD] SD still OK after TFT init (directory open OK)");
  } else {
    Serial.println("[SD] SD open failed after TFT init");
    sdOk = false;
  }
}

if (sdOk && SD.cardType() == CARD_NONE) sdOk = false;
Serial.print("[SD] sdOk(after TFT)="); Serial.println(sdOk ? "true" : "false");


  // GPS
  Serial.println("[GPS] Initializing UART...");
  Serial.printf("[GPS] Pins RX=%d TX=%d Baud=%lu\n", PINS.gps_rx, PINS.gps_tx, (unsigned long)cfg.gpsBaud);
  GPSSerial.begin(cfg.gpsBaud, SERIAL_8N1, PINS.gps_rx, PINS.gps_tx);

  // WiFi: try STA first
  WiFi.mode(WIFI_STA);
  bool staOk = connectSTA(12000);

  if (!staOk) {
    WiFi.setAutoReconnect(false);
    WiFi.persistent(false);
    WiFi.disconnect(true, true);
    delay(100);
    startAP();
  }

  startWebServer();
  lastStaStatus = WiFi.status();

  if (staOk) {
    Serial.print("[WEB] STA IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.print("[WEB] AP IP: ");
    Serial.println(WiFi.softAPIP());
    Serial.println("[WEB] AP UI: http://192.168.4.1/");
  }

  if (staOk && sdOk && cfg.wigleBasicToken.length() > 0) {
    Serial.println("[WiGLE] STA connected and token set. Uploading previous CSVs...");
    uploadAllCsvsToWigle();
  } else {
    Serial.println("[WiGLE] Upload not attempted (STA/token/SD not ready).");
  }

  // Create fresh log for this run
  if (sdOk) {
    bool lfOk = openLogFile();
    Serial.print("[SD] Log file create: ");
    Serial.println(lfOk ? "OK" : "FAIL");
  } else {
    Serial.println("[SD] Log file create skipped (SD FAIL).");
  }

  updateTFT(0);
  Serial.println("=== Boot complete ===");
}

void loop() {
  server.handleClient();

  // Track AP client presence and enforce 60s AP window
  if (apWindowActive && WiFi.getMode() == WIFI_AP_STA) {
    if (WiFi.softAPgetStationNum() > 0) {
      if (!apClientSeen) Serial.println("[WIFI] AP client connected");
      apClientSeen = true;
    }
  }
  stopAPIfAllowed();

  // GPS parse
  while (GPSSerial.available()) gps.encode(GPSSerial.read());

  bool prevFix = gpsHasFix;
  gpsHasFix = gps.location.isValid() && gps.location.age() < 2000;
  if (gpsHasFix != prevFix) {
    Serial.print("[GPS] Fix state changed -> ");
    Serial.println(gpsHasFix ? "LOCKED" : "NO FIX");
    if (gpsHasFix) {
      Serial.print("[GPS] Lat="); Serial.print(gps.location.lat(), 6);
      Serial.print(" Lon=");      Serial.println(gps.location.lng(), 6);
    }
  }
uiGpsSats = gps.satellites.isValid() ? gps.satellites.value() : 0;

  float speedKmph = gps.speed.isValid() ? gps.speed.kmph() : 0.0f;
  float speedDisplay = speedKmph;
  if (cfg.speedUnits == "mph") speedDisplay = speedKmph * 0.621371f;

  // Keep last heading + label if course becomes invalid/stale
  if (gps.course.isValid() && gps.course.age() < 2000) {
    uiHeadingDeg = gps.course.deg();
    uiHeadingTxt = headingToDir8(uiHeadingDeg);
  }
  
  static bool timeSet = false;
  if (!timeSet &&
      gps.date.isValid() && gps.time.isValid() &&
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
    Serial.println("[TIME] System time set from GPS (UTC)");
  }

  // Button
  pollButton();

  // TFT refresh
  static uint32_t lastTft = 0;
  if (millis() - lastTft > 250) {
    lastTft = millis();
    updateTFT(speedDisplay);
  }

  handleStaTransitions();

  // Scanning
  autoPaused = shouldPauseScanning();

  wifi_mode_t m = WiFi.getMode();
  bool apActive = (m == WIFI_AP || m == WIFI_AP_STA);

  bool allowScan = scanningEnabled && sdOk && !apActive && (userScanOverride || !autoPaused);
  if (allowScan) doScanOnce();

  delay(10);
}