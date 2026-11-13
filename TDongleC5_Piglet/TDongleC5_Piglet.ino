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
#include <functional>
#include <time.h>
#include <math.h>
#include <esp_now.h>
#include "esp_wifi.h"
#include "driver/gpio.h"
#include "soc/gpio_sig_map.h"
#include "esp32-hal-matrix.h"
#include "YieldSched.h"
#include "OtaProto.h"
#include <Update.h>
#include "mbedtls/sha256.h"
#include "esp_heap_caps.h"
#include <stdarg.h>

// ---- Core-side OTA engine types ----
// Defined here, at the very top of the file, rather than alongside the
// coreOta* functions further down: the Arduino .ino preprocessor auto-
// generates function prototypes near the top of the file (before most of
// the sketch has been parsed), so any top-level function whose signature
// uses a type defined later in the file fails to compile (see
// biscuitSetChannels()'s comment below for the same issue). Defining these
// types up front avoids that for coreOtaLoadOne()/coreOtaClose()/coreOtaStatus().
enum class CoreOtaPhase : uint8_t { Idle, Starting, Sending, Done, Failed };
struct CoreOtaStatus {
  CoreOtaPhase phase;
  int8_t       nodeIdx;      // roster index being updated, -1 if none
  uint32_t     sentBytes;    // bytes acknowledged by the node so far
  uint32_t     totalBytes;
  char         msg[28];      // short human-readable result / failure reason
  char         loadMsg[28];  // per-board-type summary of what's staged on SD
};
// Per-board-type image, preloaded from the Core's SD card at boot.
struct CoreOtaImage {
  uint8_t     boardType;    // PigletBoardType this file is for
  const char* path;         // /Firmware/<Board>.ota.bin
  const char* label;        // short tag for status messages
  uint8_t*    image;        // whole image in PSRAM, null if not loaded
  uint32_t    size;
  uint16_t    chunks;
  uint8_t     sha[32];
  char        failMsg[28];  // why this image is not loaded
};

// On-device "Update Nodes through Core?" prompt status, read by
// drawPageMeshNode() and pollButton(). Defined here (not alongside the
// coreUpdate* functions further down) for the same top-of-file-prototype
// reason as CoreOtaStatus above.
struct CoreUpdatePromptStatus {
  bool     promptActive;       // true while the 30s confirm window is open
  uint32_t promptRemainingMs;  // time left in the confirm window, valid when promptActive
  uint8_t  pendingCount;       // nodes queued for update (including the one currently pushing)
  bool     pushActive;         // a coreOta transfer for this queue is in flight
  uint8_t  pushNodeIdx;        // coreNodes[] index being pushed, valid when pushActive
};

// Per-file, per-service upload status (see UploadService/FileUploadState
// usage further down) -- defined here for the same top-of-file-prototype
// reason as CoreOtaStatus above: loadUploadState()/markServiceDone()/
// allConfiguredServicesDone() use these types in their signatures, and
// Arduino's auto-generated prototypes are inserted above this point.
enum class UploadService { Wigle, Wdgw, Wardrift };
struct FileUploadState {
  bool wigleDone    = false;
  bool wdgwDone     = false;
  bool wardriftDone = false;
};

// Firmware version
#define FIRMWARE_VERSION "v3.03"

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
  // Wardrift API key (wdk_...) from wardrift.net, sent as X-API-Key. The CSV
  // file is uploaded as-is (raw body, Content-Type: text/csv, X-File-Name
  // header) to POST /v1/ingest/signals. This endpoint caps files at 10 MiB;
  // larger CSVs are skipped for Wardrift but still attempted on
  // WDGoWars/WiGLE, which have higher limits. Runs 2nd in the upload chain:
  // after WDGoWars, before WiGLE.
  String wardriftApiKey;
  // Boot auto-upload limit: -1=all, 0=disabled, 1+=capped
  int maxBootUploads   = 25;
  // Optional device name shown in WiGLE header and filename
  String deviceName;
  // Auto-start mesh role on boot: "core" | "node" | "none"
  String meshModeOnBoot = "none";
  // Rotate TFT screen 180° (true = upside-down mount). Requires reboot.
  bool rotateScreen180 = false;
  // Ceiling for SD-over-SPI clock negotiation (Hz). Boot tries a descending
  // ladder of speeds up to this cap and uses the fastest one that mounts
  // successfully. Lower this if you see SD write errors/corruption on
  // marginal wiring; default is a commonly-safe ceiling for SD-over-SPI.
  uint32_t sdMaxSpiHz = 20000000;
  // Up to 10 SSIDs that will never be logged (exact, case-sensitive match).
  // Empty slots never match, so blank/hidden SSIDs are not accidentally filtered.
  String ssidWhitelist[10];
  // When true (default): if the configured home SSID is seen in a scan
  // result while wardriving, the device stops scanning, connects to it, and
  // uploads pending CSVs automatically.
  bool uploadOnNetworkSeen = true;
  // Wardrive Go / WarBLE Go BLE bridge. When true, the Mesh Node/Core page
  // (and only that page) advertises a RocketGod "iosESP" BLE bridge so the
  // Wardrive Go / WarBLE Go phone app can pair and plot APs live: Node pushes
  // its own scan finds + GPS, Core pushes every node-forwarded AP + its own
  // GPS. Default off.
  bool wardriveGoBridge = false;
  // When true: a headless PigletNode (PIGLET_BOARD_NODE, no display/buttons
  // of its own) that connects to this Core running older firmware is
  // automatically pushed the staged /Firmware/PigletNode.ota.bin image over
  // ESP-NOW, with no on-device confirmation (there's no screen to confirm
  // on). XIAO-based nodes (Piglet/TDongleC5_Piglet) always get the on-screen
  // "Update Nodes through Core?" confirm prompt instead. Use an SD card
  // rated at least 16GB for sustained-write reliability. Default off.
  bool autoUpdateHeadlessNodes = false;
  // When true (default): CSVs are gzip-compressed before upload to WiGLE
  // (one combined/merged archive per batch) and WDGoWars (each file
  // individually compressed, since it cannot accept a multi-file archive).
  // Wardrift uploads are ALWAYS sent uncompressed regardless of this
  // setting -- its /v1/ingest/signals endpoint does not honor
  // Content-Encoding: gzip (confirmed in the field: a gzip body comes back
  // HTTP 400 "file did not contain supported signal rows"). Escape hatch in
  // case WiGLE/WDGoWars ever reject the gzip framing too -- flip off to
  // fall back to plain uncompressed uploads for those without a rebuild.
  bool gzipUploads = true;
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

// Set when the configured home SSID (cfg.homeSsid) is seen in a scan result
// while wardriving. Consumed by checkHomeNetworkReturn(), which connects to
// it and runs the upload flow, then clears the flag.
static bool homeNetworkSeen = false;

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

// ---------------- Firmware Update State ----------------
// Declared here (well before the Firmware Update implementation section
// further down) because updateTFT()/pollButton() need to reference these to
// drive the on-device "Press button to download/install" flow, and -- unlike
// function signatures -- Arduino's auto-prototype generator does not forward-
// declare plain global variables, so they must physically precede any use.
static bool     fwUpdateChecked = false;
static bool     fwUpdateAvailable = false;
static String   fwAvailableVersion = "";
static String   fwReleaseNotes = "";
static String   fwCheckError = "";

static bool     fwDownloading = false;
static bool     fwDownloaded = false;
static String   fwDownloadStage = "";
static uint32_t fwDownloadedBytes = 0;
static uint32_t fwDownloadTotalBytes = 0;
static String   fwDownloadError = "";

static bool     fwInstalling = false;
static String   fwInstallError = "";

static const char* WDGWARS_HOST = "wdgwars.pl";
static const uint16_t WDGWARS_PORT = 443;

static const char* WARDRIFT_HOST = "wardrift.net";
static const uint16_t WARDRIFT_PORT = 443;

// Logs free/largest-contiguous MALLOC_CAP_DMA heap -- the narrower pool the
// hardware AES/DMA driver allocates its (small) per-operation descriptors
// from, separate from general MALLOC_CAP_INTERNAL free heap. A failed
// client.connect() with plenty of MALLOC_CAP_INTERNAL free but a tight or
// zero MALLOC_CAP_DMA pool is the "esp-aes: Failed to allocate memory"
// signature; called on every failed TLS connect attempt below so the next
// occurrence has concrete numbers instead of a guess.
static void logDmaHeap(const char* tag) {
  Serial.printf("[HEAP] %s: dmaFree=%u dmaLargest=%u\n", tag,
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
}

// A 202 response only means the CSV was received and queued for import —
// it does NOT mean the server has finished processing it. Blocking the
// upload loop for up to 45 s per file waiting on that made both WDGoWars
// and WiGLE uploads (which run right after it, for the same file) look
// hung even though the file had already reached the server. Instead, the
// job is queued here and checked opportunistically from loop() via
// wdgwarsServicePendingJobs(), without blocking anything.
struct WdgwarsPendingJob {
  int      jobId;
  String   filename;
  uint32_t queuedAtMs;
  uint32_t lastCheckMs;
  // Exponential backoff per job -- starts at the base interval and doubles
  // (capped) each inconclusive check, instead of hammering a fresh TLS
  // connection every 4s for up to 5 minutes straight. That steady
  // background TLS churn was a prime suspect for the AES/DMA-pool
  // exhaustion behind repeated, sustained (not just momentary) "esp-aes:
  // Failed to allocate memory" failures on foreground Test Token/Key TLS
  // attempts during the same window.
  uint32_t nextDelayMs;
};
static std::vector<WdgwarsPendingJob> wdgwarsPendingJobs;
static const uint32_t WDGWARS_JOB_CHECK_INTERVAL_MS = 4000;            // base/min gap between checks
static const uint32_t WDGWARS_JOB_CHECK_MAX_MS      = 60000;           // backoff ceiling per job
static const uint32_t WDGWARS_JOB_MAX_AGE_MS        = 5UL * 60UL * 1000UL; // give up after 5 min

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

// Routes ArduinoJson's JsonDocument allocations to PSRAM (MALLOC_CAP_SPIRAM)
// instead of the library's default malloc()-backed internal heap. On this
// chip, plain malloc() (and therefore every un-annotated JsonDocument used
// across the Web UI's status/files/mesh/update JSON handlers) pulls from the
// same general internal RAM pool that MALLOC_CAP_DMA is carved from -- the
// narrow region the WiFi/crypto hardware needs for TLS handshakes. Every one
// of those JSON buffers competed with and fragmented that pool, even though
// none of them actually need DMA capability. Moving them to PSRAM frees that
// headroom for TLS instead. Defined here, near the top of the file, since
// Arduino's auto-prototype generator needs the class fully defined before
// any of the many handlers further down that use it.
class PsramJsonAllocator : public ArduinoJson::Allocator {
 public:
  void* allocate(size_t size) override {
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
  }
  void deallocate(void* ptr) override {
    heap_caps_free(ptr);
  }
  void* reallocate(void* ptr, size_t new_size) override {
    return heap_caps_realloc(ptr, new_size, MALLOC_CAP_SPIRAM);
  }
};
static ArduinoJson::Allocator* psramJsonAllocator() {
  static PsramJsonAllocator instance;
  return &instance;
}

static String pathBasename(const String& p) {
  int slash = p.lastIndexOf('/');
  if (slash < 0) return p;
  return p.substring(slash + 1);
}

// True for dot-prefixed entries (the .gzscratch gzip scratch directory, the
// .upload_state.cfg per-file upload ledger) -- internal housekeeping, never
// user wardriving data. Used by both the upload path (collectEligibleCsvs())
// and "Delete All" (handleDeleteAll()) so neither ever reads, uploads, or
// deletes these, regardless of what extension (if any) a future
// housekeeping file ends up using.
static bool isHiddenSdEntry(const String& basename) {
  return basename.length() > 0 && basename[0] == '.';
}

// Rejects exact Null Island (0,0 -- a real location in the Gulf of Guinea,
// virtually certain to be a GPS-less sentinel rather than a genuine fix for
// a wardriving device), any coordinate outside the physically possible
// range for latitude/longitude, and non-finite values from corrupted GPS
// data. Used everywhere a position is about to be cached or logged so a
// bad value can never end up in a CSV row.
static bool gpsCoordIsValid(double lat, double lon) {
  if (!isfinite(lat) || !isfinite(lon)) return false;
  if (lat < -90.0 || lat > 90.0) return false;
  if (lon < -180.0 || lon > 180.0) return false;
  if (lat == 0.0 && lon == 0.0) return false;  // Null Island / no-fix sentinel
  return true;
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
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
             gps.date.year(), gps.date.month(), gps.date.day(),
             gps.time.hour(), gps.time.minute(), gps.time.second());
    return String(buf);
  }
  uint32_t s = millis() / 1000;
  char buf[32];
  snprintf(buf, sizeof(buf), "1970-01-01 00:%02lu:%02lu",
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
  else if (k == "wardriftApiKey")  cfg.wardriftApiKey = v;
  else if (k == "maxBootUploads") { int n = v.toInt(); if (n >= -1) cfg.maxBootUploads = n; }
  else if (k == "deviceName")      cfg.deviceName = v;
  else if (k == "meshModeOnBoot") { String vv = v; vv.toLowerCase(); if (vv == "core" || vv == "node" || vv == "none") cfg.meshModeOnBoot = vv; }
  else if (k == "rotateScreen180") { String vv = v; vv.toLowerCase(); cfg.rotateScreen180 = (vv == "true" || vv == "1"); }
  else if (k == "sdMaxSpiHz") {
    long hz = v.toInt();
    if (hz >= 400000 && hz <= 40000000) cfg.sdMaxSpiHz = (uint32_t)hz;  // sanity-clamp to a plausible SPI range
  }
  else if (k.startsWith("ssidWhitelist")) {
    int idx = k.substring(13).toInt();
    if (idx >= 1 && idx <= 10) cfg.ssidWhitelist[idx - 1] = v;  // assign even if empty, to allow clearing
  }
  else if (k == "uploadOnNetworkSeen") {
    String vv = v; vv.toLowerCase();
    cfg.uploadOnNetworkSeen = (vv == "true" || vv == "1");
  }
  else if (k == "wardriveGoBridge") {
    String vv = v; vv.toLowerCase();
    cfg.wardriveGoBridge = (vv == "true" || vv == "1");
  }
  else if (k == "autoUpdateHeadlessNodes") {
    String vv = v; vv.toLowerCase();
    cfg.autoUpdateHeadlessNodes = (vv == "true" || vv == "1");
  }
  else if (k == "gzipUploads") {
    String vv = v; vv.toLowerCase();
    cfg.gzipUploads = (vv == "true" || vv == "1");
  }
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
  f.println("# Wardrift API key (wdk_...) from wardrift.net, sent as X-API-Key.");
  f.print("wardriftApiKey=");  f.println(cfg.wardriftApiKey);
  f.println("# Boot auto-upload limit: -1=all, 0=disabled, 1+=capped");
  f.print("maxBootUploads=");  f.println(cfg.maxBootUploads);
  f.println("# Device name: shown in WiGLE header and filename (optional)");
  f.print("deviceName=");      f.println(cfg.deviceName);
  f.println("# Mesh mode on boot: core | node | none");
  f.print("meshModeOnBoot=");  f.println(cfg.meshModeOnBoot);
  f.println("# Rotate screen 180 degrees (true = upside-down mount). Requires reboot.");
  f.print("rotateScreen180="); f.println(cfg.rotateScreen180 ? "true" : "false");
  f.println("# SD-over-SPI clock ceiling in Hz (default 20000000 = 20 MHz). Lower if you");
  f.println("# see SD write errors/corruption on marginal wiring.");
  f.print("sdMaxSpiHz="); f.println(cfg.sdMaxSpiHz);
  f.println("# SSIDs that will never be logged (up to 10, exact case-sensitive match).");
  for (int i = 0; i < 10; i++) {
    f.print("ssidWhitelist"); f.print(i + 1); f.print("="); f.println(cfg.ssidWhitelist[i]);
  }
  f.println("# Connect + upload automatically when the home SSID is seen while wardriving.");
  f.print("uploadOnNetworkSeen="); f.println(cfg.uploadOnNetworkSeen ? "true" : "false");

  f.println("# Wardrive Go / WarBLE Go BLE bridge -- active only on the Mesh Node/Core page.");
  f.print("wardriveGoBridge="); f.println(cfg.wardriveGoBridge ? "true" : "false");

  f.println("# Auto-push firmware to out-of-date headless PigletNode nodes (no on-screen");
  f.println("# confirm -- they have no display). XIAO nodes always use the on-device");
  f.println("# confirm prompt instead. Use an SD card rated at least 16GB.");
  f.print("autoUpdateHeadlessNodes="); f.println(cfg.autoUpdateHeadlessNodes ? "true" : "false");

  f.println("# Gzip-compress CSVs before upload (WiGLE: one merged archive per batch;");
  f.println("# WDGoWars/Wardrift: each file individually). Default true.");
  f.print("gzipUploads="); f.println(cfg.gzipUploads ? "true" : "false");

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

// Row limit per CSV -- a loose secondary safeguard; CSV_MAX_BYTES below is
// the real, byte-accurate cap that rotation actually relies on.
static const uint32_t CSV_MAX_ROWS = 100000;
static uint32_t       csvRowCount  = 0;

// Byte limit per CSV file: rotates well before the Wardrift API's hard
// 10 MiB (10,485,760-byte) cap on POST /v1/ingest/signals, with roughly 1 MiB
// of margin to absorb header overhead and the one extra row that can land
// before rotation is detected on the next call. Also comfortably under the
// WDGoWars 15 MB cap. Tracked from the actual bytes logFile.println() reports
// writing, not estimated from row count, so this is an exact guarantee.
static const uint32_t CSV_MAX_BYTES = 9UL * 1024UL * 1024UL;  // 9 MiB
static uint32_t       csvByteCount  = 0;

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
  logFile.println(",display=TFT-ST7735-80x160,board=LilyGo-T-Dongle-C5,brand=Piglet,star=Sol,body=3,subBody=0");
  logFile.println("MAC,SSID,AuthMode,FirstSeen,Channel,Frequency,RSSI,CurrentLatitude,CurrentLongitude,AltitudeMeters,AccuracyMeters,RCOIs,MfgrId,Type");
  logFile.flush();
  csvRowCount = 0;
  csvByteCount = 0;  // header bytes are negligible against the multi-MB cap
  return true;
}

// Copy `in` into fixed buffer `out` (size outSize), doubling any embedded
// double-quote characters per CSV escaping rules (RFC4180-style). Truncates
// safely rather than overflowing if the (unexpectedly long/malformed) input
// wouldn't fit.
static void csvEscapeQuotes(const String& in, char* out, size_t outSize) {
  size_t o = 0;
  size_t n = in.length();
  for (size_t i = 0; i < n; i++) {
    char c = in[i];
    if (c == '"') {
      if (o + 2 >= outSize) break;  // no room left for the doubled quote
      out[o++] = '"';
      out[o++] = '"';
    } else {
      if (o + 1 >= outSize) break;
      out[o++] = c;
    }
  }
  out[o] = '\0';
}

// Returns true if the row was actually written to the log file, false if it
// was skipped for any reason (SD unavailable, whitelist match, encoding error).
static bool appendWigleRow(const String& mac, const String& ssid, const String& auth,
                           const String& firstSeen, int channel, int rssi,
                           double lat, double lon, double altM, double accM) {
  if (!sdOk || !logFile) return false;

  // Final backstop: never persist Null Island (0,0), an out-of-range
  // coordinate, or a non-finite value from corrupted GPS data -- regardless
  // of what any caller already thinks it validated. See gpsCoordIsValid()
  // above. processScanResults() is expected to already skip calling this at
  // all when it has no trustworthy position, so this firing is itself a
  // signal something upstream needs a look.
  if (!gpsCoordIsValid(lat, lon)) {
    Serial.printf("[SD] appendWigleRow: rejected invalid coordinate (%.6f, %.6f) -- row not written\n", lat, lon);
    return false;
  }

  // Skip logging for any SSID present in the whitelist (exact, case-sensitive match)
  for (int i = 0; i < 10; i++) {
    if (cfg.ssidWhitelist[i].length() > 0 && cfg.ssidWhitelist[i] == ssid) return false;
  }

  // Rotate before the file grows past the byte cap (the real Wardrift-size
  // guarantee) or the loose row-count safeguard, whichever comes first.
  if (csvByteCount >= CSV_MAX_BYTES || csvRowCount >= CSV_MAX_ROWS) {
    Serial.printf("[SD] CSV %s limit reached (%u bytes, %u rows), rotating log file\n",
                  (csvByteCount >= CSV_MAX_BYTES) ? "byte" : "row",
                  (unsigned)csvByteCount, (unsigned)csvRowCount);
    closeLogFile();
    if (!openLogFile()) return false;
  }

  // Frequency in MHz (WiGLE 1.6)
  uint32_t freq = 0;
  if      (channel >= 1  && channel <= 13) freq = 2407u + (uint32_t)channel * 5;
  else if (channel == 14)                  freq = 2484u;
  else if (channel >= 32)                  freq = 5000u + (uint32_t)channel * 5;

  // Build the row into fixed stack buffers instead of chaining Arduino
  // String concatenation (each `+=` above used to allocate/copy/free on the
  // heap). This runs once per scan result and can fire hundreds of times
  // per batch in dense areas, so per-row heap churn adds up fast.
  char safeSsid[70];  // SSID is at most 32 bytes; worst case (all quotes) doubles to 64
  csvEscapeQuotes(ssid, safeSsid, sizeof(safeSsid));

  char line[256];
  int len = snprintf(line, sizeof(line),
                      "%s,\"%s\",%s,%s,%d,%u,%d,%.6f,%.6f,%.1f,%.1f,,,WIFI",
                      mac.c_str(), safeSsid, auth.c_str(), firstSeen.c_str(),
                      channel, (unsigned)freq, rssi, lat, lon, altM, accM);
  if (len < 0) {
    Serial.println("[SD] appendWigleRow: encoding error, row skipped");
    return false;
  }
  if ((size_t)len >= sizeof(line)) {
    Serial.printf("[SD] appendWigleRow: row truncated (needed %d bytes, buffer %u) — check SSID\n",
                  len, (unsigned)sizeof(line));
  }

  digitalWrite(PINS.tft_cs, HIGH);
  size_t written = logFile.println(line);
  csvRowCount++;
  csvByteCount += (uint32_t)written;  // 0 on a failed write, so no false accumulation

  // Detect silent write failure — if println() returns 0 for a non-empty line,
  // the SD card or file handle is broken. Attempt to reopen the log file once;
  // if that also fails, mark SD as unusable until next boot.
  if (written == 0 && len > 0) {
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
    return false;
  }

  // Flush less often to avoid stalls (SD writes can block hard). Threshold
  // tightened from 25 to 16 lines so large batches get broken into smaller
  // chunks; yield() after each flush gives WiFi/GPS/web-server/watchdog a
  // chance to run between chunks instead of only after the whole batch.
  static uint32_t lastFlushMs = 0;
  static uint32_t linesSinceFlush = 0;
  linesSinceFlush++;

  if (linesSinceFlush >= 16 || (millis() - lastFlushMs) >= 2000) {
    digitalWrite(PINS.tft_cs, HIGH);
    logFile.flush();
    yield();
    lastFlushMs = millis();
    linesSinceFlush = 0;
  }
  return true;
}

// Forward declarations needed by WDGoWars and WiGLE batch functions
static void tftWigleUploadScreen(uint32_t done, uint32_t total, const String& filename);
static void forceStatusFullRedraw();
static void wdgwarsServicePendingJobs();
static void wdgwarsDrainPendingJobs(uint32_t budgetMs);
static bool initSD_SharedSPI();
static bool sdTryRecover();

// ================================================================
//  Real gzip compression (RFC 1952) via the ESP32 ROM's built-in miniz
//  DEFLATE compressor (tdefl_compress/tdefl_init) -- these symbols live in
//  the chip's boot ROM (confirmed present at fixed addresses in the linked
//  build) and cost zero flash/IRAM; the only overhead is a one-time ~160KB
//  tdefl_compressor allocation from PSRAM while compressing. Used to shrink
//  CSVs before they go out over the air to WiGLE/WDGoWars/Wardrift. This is
//  real compression, unlike the zero-ratio "stored block" gzip framing used
//  by the /downloadAll-style browser-download helpers on other boards.
//
//  Arduino-ESP32 doesn't bundle ESP-IDF's esp_rom/include/miniz.h, so the
//  exact struct layout/prototypes below are ported from that header
//  (trimmed to just the low-level tdefl_init()/tdefl_compress() pieces
//  actually used here) -- this layout MUST match the ROM's compiled
//  version exactly, since we're calling directly into ROM code that
//  reads/writes these fields.
// ================================================================
extern "C" {

typedef unsigned char  gz_mz_uint8;
typedef unsigned short gz_mz_uint16;
typedef unsigned int   gz_mz_uint32;
typedef unsigned int   gz_mz_uint;
typedef int            gz_mz_bool;

enum {
  GZ_TDEFL_LZ_DICT_SIZE = 32768, GZ_TDEFL_MAX_MATCH_LEN = 258,
  GZ_TDEFL_MAX_HUFF_TABLES = 3, GZ_TDEFL_MAX_HUFF_SYMBOLS = 288,
  GZ_TDEFL_LZ_CODE_BUF_SIZE = 24 * 1024,
  GZ_TDEFL_OUT_BUF_SIZE = (GZ_TDEFL_LZ_CODE_BUF_SIZE * 13) / 10,
  GZ_TDEFL_LZ_HASH_BITS = 12, GZ_TDEFL_LZ_HASH_SIZE = 1 << GZ_TDEFL_LZ_HASH_BITS
};

typedef gz_mz_bool (*gz_tdefl_put_buf_func_ptr)(const void* pBuf, int len, void* pUser);

typedef enum {
  GZ_TDEFL_STATUS_BAD_PARAM = -2, GZ_TDEFL_STATUS_PUT_BUF_FAILED = -1,
  GZ_TDEFL_STATUS_OKAY = 0, GZ_TDEFL_STATUS_DONE = 1
} gz_tdefl_status;

typedef enum {
  GZ_TDEFL_NO_FLUSH = 0, GZ_TDEFL_SYNC_FLUSH = 2, GZ_TDEFL_FULL_FLUSH = 3, GZ_TDEFL_FINISH = 4
} gz_tdefl_flush;

typedef struct {
  gz_tdefl_put_buf_func_ptr m_pPut_buf_func;
  void* m_pPut_buf_user;
  gz_mz_uint m_flags, m_max_probes[2];
  int m_greedy_parsing;
  gz_mz_uint m_adler32, m_lookahead_pos, m_lookahead_size, m_dict_size;
  gz_mz_uint8 *m_pLZ_code_buf, *m_pLZ_flags, *m_pOutput_buf, *m_pOutput_buf_end;
  gz_mz_uint m_num_flags_left, m_total_lz_bytes, m_lz_code_buf_dict_pos, m_bits_in, m_bit_buffer;
  gz_mz_uint m_saved_match_dist, m_saved_match_len, m_saved_lit, m_output_flush_ofs, m_output_flush_remaining,
             m_finished, m_block_index, m_wants_to_finish;
  gz_tdefl_status m_prev_return_status;
  const void* m_pIn_buf;
  void* m_pOut_buf;
  size_t *m_pIn_buf_size, *m_pOut_buf_size;
  gz_tdefl_flush m_flush;
  const gz_mz_uint8* m_pSrc;
  size_t m_src_buf_left, m_out_buf_ofs;
  gz_mz_uint8 m_dict[GZ_TDEFL_LZ_DICT_SIZE + GZ_TDEFL_MAX_MATCH_LEN - 1];
  gz_mz_uint16 m_huff_count[GZ_TDEFL_MAX_HUFF_TABLES][GZ_TDEFL_MAX_HUFF_SYMBOLS];
  gz_mz_uint16 m_huff_codes[GZ_TDEFL_MAX_HUFF_TABLES][GZ_TDEFL_MAX_HUFF_SYMBOLS];
  gz_mz_uint8 m_huff_code_sizes[GZ_TDEFL_MAX_HUFF_TABLES][GZ_TDEFL_MAX_HUFF_SYMBOLS];
  gz_mz_uint8 m_lz_code_buf[GZ_TDEFL_LZ_CODE_BUF_SIZE];
  gz_mz_uint16 m_next[GZ_TDEFL_LZ_DICT_SIZE];
  gz_mz_uint16 m_hash[GZ_TDEFL_LZ_HASH_SIZE];
  gz_mz_uint8 m_output_buf[GZ_TDEFL_OUT_BUF_SIZE];
} gz_tdefl_compressor;

// Low 12 bits = max dictionary-search probes. No zlib-header flag -- gzip
// wraps a RAW deflate stream with its own header/trailer, not a zlib one.
static const int GZ_TDEFL_DEFAULT_MAX_PROBES = 128;

gz_tdefl_status tdefl_init(gz_tdefl_compressor* d, gz_tdefl_put_buf_func_ptr pPut_buf_func, void* pPut_buf_user, int flags);
gz_tdefl_status tdefl_compress(gz_tdefl_compressor* d, const void* pIn_buf, size_t* pIn_buf_size,
                                void* pOut_buf, size_t* pOut_buf_size, gz_tdefl_flush flush);

}  // extern "C"

// Shared CRC32 (gzip/PNG polynomial, reflected).
static uint32_t gzipCrc32Update(uint32_t crc, const uint8_t* data, size_t len) {
  static const uint32_t T[16] = {
    0x00000000, 0x1DB71064, 0x3B6E20C8, 0x26D930AC,
    0x76DC4190, 0x6B6B51F4, 0x4DB26158, 0x5005713C,
    0xEDB88320, 0xF00F9344, 0xD6D6A3E8, 0xCB61B38C,
    0x9B64C2B0, 0x86D3D2D4, 0xA00AE278, 0xBDBDF21C
  };
  crc = ~crc;
  for (size_t i = 0; i < len; i++) {
    crc = (crc >> 4) ^ T[(crc ^ data[i]) & 0xF];
    crc = (crc >> 4) ^ T[(crc ^ (data[i] >> 4)) & 0xF];
  }
  return ~crc;
}

namespace {

struct GzipSink {
  File out;
  uint32_t crc = 0;
  uint32_t isize = 0;

  bool writeHeader() {
    static const uint8_t hdr[10] = { 0x1f, 0x8b, 0x08, 0x00, 0, 0, 0, 0, 0, 0xff };
    return out.write(hdr, 10) == 10;
  }
  bool writeCompressed(const uint8_t* buf, size_t n) {
    if (!n) return true;
    return out.write(buf, n) == n;
  }
  void updateCrc(const uint8_t* raw, size_t n) {
    crc = gzipCrc32Update(crc, raw, n);
    isize += (uint32_t)n;
  }
  bool writeTrailer() {
    uint8_t t[8] = {
      (uint8_t)crc, (uint8_t)(crc >> 8), (uint8_t)(crc >> 16), (uint8_t)(crc >> 24),
      (uint8_t)isize, (uint8_t)(isize >> 8), (uint8_t)(isize >> 16), (uint8_t)(isize >> 24)
    };
    return out.write(t, 8) == 8;
  }
};

// Drives the ROM compressor: repeatedly calls readChunk(buf, cap) to pull
// raw bytes (returns 0 when done), feeds them through tdefl_compress(), and
// writes compressed output + CRC32/ISIZE via sink. Allocates the (~160KB)
// gz_tdefl_compressor from PSRAM for the duration of the call.
bool gzCompressStream(GzipSink& sink, const std::function<int(uint8_t*, size_t)>& readChunk) {
  gz_tdefl_compressor* comp = (gz_tdefl_compressor*)heap_caps_malloc(sizeof(gz_tdefl_compressor), MALLOC_CAP_SPIRAM);
  if (!comp) {
    Serial.println("[GZIP] Failed to allocate tdefl_compressor from PSRAM");
    return false;
  }

  // inBuf/outBuf used to be plain stack locals (2048 + 4096 = 6KB). This
  // function runs deep inside the boot-time call chain (setup() ->
  // uploadAllPending() -> gzip*ToFile() -> here), and the cumulative stack
  // usage already built up by that point, plus these 6KB, plus whatever the
  // ROM tdefl_compress() itself uses internally, was enough to overflow the
  // default 8KB loopTask stack -- observed in the field as a RISC-V "Stack
  // protection fault" (stack pointer ended up ~5KB below the task's
  // allocated stack region). Heap/PSRAM-allocating them instead, right next
  // to the compressor, keeps this function's own stack frame tiny.
  uint8_t* inBuf  = (uint8_t*)heap_caps_malloc(2048, MALLOC_CAP_SPIRAM);
  uint8_t* outBuf = (uint8_t*)heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
  if (!inBuf || !outBuf) {
    Serial.println("[GZIP] Failed to allocate scratch buffers from PSRAM");
    if (inBuf)  heap_caps_free(inBuf);
    if (outBuf) heap_caps_free(outBuf);
    heap_caps_free(comp);
    return false;
  }

  if (tdefl_init(comp, nullptr, nullptr, GZ_TDEFL_DEFAULT_MAX_PROBES) != GZ_TDEFL_STATUS_OKAY) {
    Serial.println("[GZIP] tdefl_init failed");
    heap_caps_free(inBuf);
    heap_caps_free(outBuf);
    heap_caps_free(comp);
    return false;
  }
  if (!sink.writeHeader()) {
    heap_caps_free(inBuf);
    heap_caps_free(outBuf);
    heap_caps_free(comp);
    return false;
  }

  bool ok = true;
  int n;
  while (ok && (n = readChunk(inBuf, 2048)) > 0) {
    sink.updateCrc(inBuf, (size_t)n);
    const uint8_t* inPtr = inBuf;
    size_t inLeft = (size_t)n;
    while (ok && inLeft > 0) {
      size_t inSz = inLeft;
      size_t outSz = 4096;
      gz_tdefl_status st = tdefl_compress(comp, inPtr, &inSz, outBuf, &outSz, GZ_TDEFL_NO_FLUSH);
      if (st != GZ_TDEFL_STATUS_OKAY) { ok = false; break; }
      if (outSz > 0 && !sink.writeCompressed(outBuf, outSz)) { ok = false; break; }
      inPtr += inSz;
      inLeft -= inSz;
      yield();
    }
  }

  if (ok) {
    gz_tdefl_status st;
    do {
      size_t inSz = 0;
      size_t outSz = 4096;
      st = tdefl_compress(comp, nullptr, &inSz, outBuf, &outSz, GZ_TDEFL_FINISH);
      if (outSz > 0 && !sink.writeCompressed(outBuf, outSz)) { ok = false; break; }
      yield();
    } while (st == GZ_TDEFL_STATUS_OKAY);
    if (ok && st != GZ_TDEFL_STATUS_DONE) ok = false;
  }

  heap_caps_free(inBuf);
  heap_caps_free(outBuf);
  heap_caps_free(comp);
  if (!ok) return false;
  return sink.writeTrailer();
}

}  // namespace

// Compresses srcPath into a valid gzip file at dstPath. Returns false on
// any allocation/SD/compression failure; dstPath is removed in that case.
static bool gzipCompressFileToFile(const String& srcPath, const String& dstPath) {
  File in = SD.open(srcPath, FILE_READ);
  if (!in) { Serial.printf("[GZIP] Cannot open source %s\n", srcPath.c_str()); return false; }
  if (SD.exists(dstPath)) SD.remove(dstPath);
  File out = SD.open(dstPath, FILE_WRITE);
  if (!out) { Serial.printf("[GZIP] Cannot create %s\n", dstPath.c_str()); in.close(); return false; }

  GzipSink sink;
  sink.out = out;
  bool ok = gzCompressStream(sink, [&](uint8_t* buf, size_t cap) -> int { return in.read(buf, cap); });
  in.close();
  out.close();
  if (!ok) SD.remove(dstPath);
  return ok;
}

// Merges multiple WiGLE CSVs into one gzip-compressed archive: the first
// successfully-opened file's two WiGLE header lines are kept verbatim, the
// same two lines are stripped from every subsequent file. Files that fail
// to open are skipped.
static bool gzipCompressMergedFilesToFile(const std::vector<String>& srcPaths, const String& dstPath) {
  if (srcPaths.empty()) return false;
  if (SD.exists(dstPath)) SD.remove(dstPath);
  File out = SD.open(dstPath, FILE_WRITE);
  if (!out) { Serial.printf("[GZIP] Cannot create %s\n", dstPath.c_str()); return false; }

  GzipSink sink;
  sink.out = out;

  size_t nextIdx = 0;
  File cur;
  bool headerWritten = false;  // set once the merged output has its first byte

  bool ok = gzCompressStream(sink, [&](uint8_t* buf, size_t cap) -> int {
    for (;;) {
      if (cur) {
        int n = cur.read(buf, cap);
        if (n > 0) { headerWritten = true; return n; }
        cur.close();
      }
      if (nextIdx >= srcPaths.size()) return 0;
      cur = SD.open(srcPaths[nextIdx], FILE_READ);
      nextIdx++;
      if (!cur) continue;
      if (headerWritten) {
        if (cur.available()) cur.readStringUntil('\n');
        if (cur.available()) cur.readStringUntil('\n');
      }
    }
  });

  if (cur) cur.close();
  out.close();
  if (!ok) SD.remove(dstPath);
  return ok;
}

// ================================================================
//  Per-file, per-service upload status ledger
//  Tracks which upload services each CSV has already succeeded against, so
//  a failure against one service (e.g. WDGoWars) never blocks or
//  re-triggers the others (WiGLE/Wardrift) for that same file. Persisted
//  in a small plain-text ledger at /logs/.upload_state.cfg (one line per
//  filename: "name|wigle|wdgw|wardrift", each field 1=done or 0=pending).
//  UploadService/FileUploadState themselves are defined near the top of
//  the file -- see the comment there for why.
// ================================================================
namespace {

struct UploadLedgerEntry {
  String filename;
  bool   wigle;
  bool   wdgw;
  bool   wardrift;
};

const char* UPLOAD_STATE_PATH = "/logs/.upload_state.cfg";

bool readUploadLedger(std::vector<UploadLedgerEntry>& out) {
  if (!sdOk || !SD.exists(UPLOAD_STATE_PATH)) return false;
  File f = SD.open(UPLOAD_STATE_PATH, FILE_READ);
  if (!f) return false;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;
    int p1 = line.indexOf('|'); if (p1 < 0) continue;
    int p2 = line.indexOf('|', p1 + 1); if (p2 < 0) continue;
    int p3 = line.indexOf('|', p2 + 1); if (p3 < 0) continue;
    UploadLedgerEntry e;
    e.filename = line.substring(0, p1);
    e.wigle    = line.substring(p1 + 1, p2) == "1";
    e.wdgw     = line.substring(p2 + 1, p3) == "1";
    e.wardrift = line.substring(p3 + 1) == "1";
    out.push_back(e);
  }
  f.close();
  return true;
}

bool writeUploadLedger(const std::vector<UploadLedgerEntry>& entries) {
  if (!sdOk) return false;
  if (SD.exists(UPLOAD_STATE_PATH)) SD.remove(UPLOAD_STATE_PATH);
  if (entries.empty()) return true;
  File f = SD.open(UPLOAD_STATE_PATH, FILE_WRITE);
  if (!f) { Serial.println("[UPLOADSTATE] Failed to open ledger for write"); return false; }
  for (const auto& e : entries) {
    f.print(e.filename); f.print('|');
    f.print(e.wigle ? '1' : '0'); f.print('|');
    f.print(e.wdgw ? '1' : '0'); f.print('|');
    f.println(e.wardrift ? '1' : '0');
  }
  f.flush(); f.close();
  return true;
}

}  // namespace

static FileUploadState loadUploadState(const String& filename) {
  FileUploadState st;
  st.wigleDone    = cfg.wigleBasicToken.length() < 8;
  st.wdgwDone     = cfg.wdgwarsApiKey.length()   < 8;
  st.wardriftDone = cfg.wardriftApiKey.length()  < 8;

  std::vector<UploadLedgerEntry> entries;
  if (!readUploadLedger(entries)) return st;
  for (const auto& e : entries) {
    if (e.filename != filename) continue;
    if (e.wigle)    st.wigleDone    = true;
    if (e.wdgw)     st.wdgwDone     = true;
    if (e.wardrift) st.wardriftDone = true;
    break;
  }
  return st;
}

static void markServiceDone(const String& filename, UploadService svc) {
  std::vector<UploadLedgerEntry> entries;
  readUploadLedger(entries);
  bool found = false;
  for (auto& e : entries) {
    if (e.filename != filename) continue;
    found = true;
    if (svc == UploadService::Wigle)    e.wigle    = true;
    if (svc == UploadService::Wdgw)     e.wdgw     = true;
    if (svc == UploadService::Wardrift) e.wardrift = true;
    break;
  }
  if (!found) {
    UploadLedgerEntry e;
    e.filename = filename;
    e.wigle    = (svc == UploadService::Wigle);
    e.wdgw     = (svc == UploadService::Wdgw);
    e.wardrift = (svc == UploadService::Wardrift);
    entries.push_back(e);
  }
  writeUploadLedger(entries);
}

static void clearUploadState(const String& filename) {
  std::vector<UploadLedgerEntry> entries;
  if (!readUploadLedger(entries)) return;
  for (size_t i = 0; i < entries.size(); i++) {
    if (entries[i].filename == filename) {
      entries.erase(entries.begin() + i);
      writeUploadLedger(entries);
      return;
    }
  }
}

static bool allConfiguredServicesDone(const FileUploadState& st) {
  return st.wigleDone && st.wdgwDone && st.wardriftDone;
}

static void pruneUploadState() {
  std::vector<UploadLedgerEntry> entries;
  if (!readUploadLedger(entries)) return;
  std::vector<UploadLedgerEntry> kept;
  kept.reserve(entries.size());
  for (const auto& e : entries) {
    if (SD.exists(String("/logs/") + e.filename)) kept.push_back(e);
  }
  if (kept.size() != entries.size()) writeUploadLedger(kept);
}

// Scratch directory for gzip-compressed uploads.
static const char* GZ_SCRATCH_DIR = "/logs/.gzscratch";
static void ensureGzScratchDir() {
  if (!SD.exists(GZ_SCRATCH_DIR)) SD.mkdir(GZ_SCRATCH_DIR);
}
static String gzScratchPathFor(const String& basename) {
  return String(GZ_SCRATCH_DIR) + "/" + basename + ".gz";
}

// ---- WDGoWars API key test — GET /api/me ----
static bool wdgwarsTestKey() {
  uploadLastResult = "";
  if (WiFi.status() != WL_CONNECTED) { uploadLastResult = "No STA WiFi"; return false; }
  if (cfg.wdgwarsApiKey.length() < 8) { uploadLastResult = "No API key set"; return false; }

  WiFiClientSecure client;
  client.setInsecure(); client.setTimeout(15000);
  // Retry TLS connect -- transient esp-aes/DMA-descriptor allocation
  // failures (logged as "esp-aes: Failed to allocate memory") can make a
  // single connect() attempt fail even though overall free heap looks
  // fine; the AES driver needs a small allocation from the separate
  // MALLOC_CAP_DMA pool, which a short retry gives a chance to free up.
  bool wdgConnected = false;
  for (int attempt = 1; attempt <= 3; attempt++) {
    if (client.connect(WDGWARS_HOST, WDGWARS_PORT)) { wdgConnected = true; break; }
    char errBuf[100];
    int sslErr = client.lastError(errBuf, sizeof(errBuf));
    Serial.printf("[WDGWars] Test key TLS connect failed (attempt %d/3): error %d: %s\n", attempt, sslErr, errBuf);
    logDmaHeap("WDGWars test key connect fail");
    client.stop(); delay(500); yield();
  }
  if (!wdgConnected) { uploadLastResult = "TLS connect fail"; return false; }

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
  if (!SD.exists(path)) {
    Serial.println("[WDGWars] File does not exist -- attempting SD recovery");
    if (!sdTryRecover() || !SD.exists(path)) {
      Serial.println("[WDGWars] File does not exist");
      return false;
    }
    Serial.println("[WDGWars] SD recovered -- file found");
  }

  File f = SD.open(path, FILE_READ);
  if (!f) {
    Serial.println("[WDGWars] Failed to open file -- attempting SD recovery");
    if (sdTryRecover()) f = SD.open(path, FILE_READ);
  }
  if (!f) {
    Serial.println("[WDGWars] Failed to open file");
    return false;
  }

  String boundary = "----Piglet-WDGWARS-BOUNDARY";
  String filename = pathBasename(path);
  String pre = "--"+boundary+"\r\nContent-Disposition: form-data; name=\"file\"; filename=\""+filename+"\"\r\nContent-Type: text/csv\r\n\r\n";
  String post = "\r\n--"+boundary+"--\r\n";
  uint32_t fileSize = f.size();
  uint32_t contentLen = (uint32_t)pre.length()+fileSize+(uint32_t)post.length();
  Serial.printf("[WDGWars] File size: %.2f MB (%u bytes)\n", fileSize / (1024.0 * 1024.0), (unsigned)fileSize);

  WiFiClientSecure client;
  client.setInsecure(); client.setTimeout(25000);
  bool connected = false;
  for (int att=1; att<=3; att++) {
    Serial.printf("[WDGWars] Free heap before connect (attempt %d/3): %u bytes\n",
                  att, (unsigned)ESP.getFreeHeap());
    if (client.connect(WDGWARS_HOST, WDGWARS_PORT)) { connected=true; break; }
    Serial.printf("[WDGWars] TLS connect failed (attempt %d/3)\n", att);
    client.stop(); delay(500); yield();
  }
  if (!connected) { uploadLastResult = "WDGW: TLS connect fail"; Serial.println("[WDGWars] TLS connect failed after 3 attempts"); f.close(); return false; }

  // A `.gz`-suffixed path means this is a scratch file already
  // gzip-compressed above -- signal that with the standard Content-Encoding
  // header (the multipart part's own Content-Type stays text/csv).
  bool isGzip = path.endsWith(".gz");
  client.print("POST /api/v2/upload-csv HTTP/1.0\r\n");
  client.print(String("Host: ")+WDGWARS_HOST+"\r\n");
  client.print(String("X-API-Key: ")+cfg.wdgwarsApiKey+"\r\n");
  client.print(String("Content-Type: multipart/form-data; boundary=")+boundary+"\r\n");
  if (isGzip) client.print("Content-Encoding: gzip\r\n");
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
  // V2 API: HTTP 202 Accepted — file received and queued for processing.
  // Report success immediately instead of blocking here for up to 45 s;
  // the job result is checked later in the background (see
  // wdgwarsServicePendingJobs() / wdgwarsDrainPendingJobs() below).
  if (code == 202) {
    int jobId = 0;
    int ji = body.indexOf("\"job_id\":");
    if (ji >= 0) jobId = body.substring(ji + 9).toInt();
    if (jobId <= 0) { uploadLastResult = "WDGW: no job_id"; return false; }

    uint32_t nowMs = millis();
    wdgwarsPendingJobs.push_back({ jobId, filename, nowMs, nowMs, WDGWARS_JOB_CHECK_INTERVAL_MS });
    uploadLastResult = "WDGW: queued (job " + String(jobId) + ")";
    Serial.printf("[WDGWars] Job %d submitted for %s — result will be checked in background\n",
                  jobId, filename.c_str());
    return true;
  }
  uploadLastResult = "WDGW fail (" + String(code) + ")";
  Serial.printf("[WDGWars] Upload FAILED HTTP %d: %s\n", code, body.c_str());
  return false;
}

// ---- Wardrift API key test — POSTs a minimal, 0-data-row CSV ----
// No dedicated key-check endpoint exists, so this doubles as a safe live
// test. Per the Wardrift API docs, "a CSV file with no supported
// observations fails" -- so a valid key with this empty payload is expected
// to come back as either 200 (accepted, 0 rows) or 400 (rejected for having
// no observations, which only happens after auth succeeds). An invalid key
// 401s/403s before the body is ever parsed.
static bool wardriftTestKey() {
  uploadLastResult = "";
  if (WiFi.status() != WL_CONNECTED) { uploadLastResult = "No STA WiFi"; return false; }
  if (cfg.wardriftApiKey.length() < 8) { uploadLastResult = "No API key set"; return false; }

  String body =
    String("WigleWifi-1.6,appRelease=") + FIRMWARE_VERSION +
    ",model=Piglet-Test,release=1,device=Piglet-Test,display=SSD1306-128x64,"
    "board=Piglet-Test,brand=Piglet,star=Sol,body=3,subBody=0\r\n"
    "MAC,SSID,AuthMode,FirstSeen,Channel,Frequency,RSSI,CurrentLatitude,"
    "CurrentLongitude,AltitudeMeters,AccuracyMeters,RCOIs,MfgrId,Type\r\n";

  WiFiClientSecure client;
  client.setInsecure(); client.setTimeout(15000);
  // Retry TLS connect -- see the matching comment in wdgwarsTestKey() for
  // why a single connect() attempt here can fail transiently.
  bool wrConnected = false;
  for (int attempt = 1; attempt <= 3; attempt++) {
    if (client.connect(WARDRIFT_HOST, WARDRIFT_PORT)) { wrConnected = true; break; }
    char errBuf[100];
    int sslErr = client.lastError(errBuf, sizeof(errBuf));
    Serial.printf("[Wardrift] Test key TLS connect failed (attempt %d/3): error %d: %s\n", attempt, sslErr, errBuf);
    logDmaHeap("Wardrift test key connect fail");
    client.stop(); delay(500); yield();
  }
  if (!wrConnected) { uploadLastResult = "TLS connect fail"; return false; }

  client.print("POST /v1/ingest/signals HTTP/1.0\r\n");
  client.print(String("Host: ") + WARDRIFT_HOST + "\r\n");
  client.print(String("X-API-Key: ") + cfg.wardriftApiKey + "\r\n");
  client.print("Content-Type: text/csv\r\n");
  client.print("X-File-Name: test.wiglecsv\r\n");
  client.print(String("Content-Length: ") + String(body.length()) + "\r\n");
  client.print("Connection: close\r\n\r\n");
  client.print(body);

  uint32_t ws = millis();
  while (!client.available() && client.connected() && (millis()-ws)<15000) { delay(10); yield(); }

  String status = client.readStringUntil('\n'); status.trim();
  int code = 0;
  if (status.startsWith("HTTP/")) {
    int s1 = status.indexOf(' ');
    if (s1>0) { int s2=status.indexOf(' ',s1+1); if(s2>s1) code=status.substring(s1+1,s2).toInt(); }
  }

  String respBody = "";
  bool inBody = false;
  while (client.connected() || client.available()) {
    String line = client.readStringUntil('\n'); line.trim();
    if (!inBody) { if (line.length()==0) inBody=true; }
    else { respBody+=line; if(respBody.length()>512) break; }
  }
  client.stop();

  Serial.printf("[Wardrift] /v1/ingest/signals (test) HTTP %d  body: %s\n", code, respBody.substring(0,160).c_str());

  if (code == 200) { uploadLastResult = "API key valid (200)"; return true; }
  // A 400 here means the server parsed far enough to reject the payload for
  // having no supported observations -- which only happens once auth passes.
  if (code == 400) { uploadLastResult = "API key valid (400 -- empty test payload rejected, as expected)"; return true; }
  if (code==401||code==403) { uploadLastResult="API key invalid ("+String(code)+")"; }
  else if (code==0) { uploadLastResult="No response from server"; }
  else { uploadLastResult="Test inconclusive ("+String(code)+")"; }
  return false;
}

// ---- Wardrift single-file upload — POST /v1/ingest/signals ----
// Auth: X-API-Key header. The CSV file is uploaded as-is (raw body,
// Content-Type: text/csv, plus an X-File-Name header) -- no multipart
// wrapper and no per-row parsing needed. This endpoint caps
// uploaded/decompressed files at 10 MiB (10,485,760 bytes); a duplicate
// file comes back as HTTP 200 with "duplicate":true in the body, not a
// 409 -- a 409 here means a real idempotency/sequence conflict and must
// NOT be treated as success.
static bool uploadFileToWardrift(const String& path) {
  if (WiFi.status() != WL_CONNECTED) { uploadLastResult = "No STA WiFi"; return false; }
  if (cfg.wardriftApiKey.length() < 8) { uploadLastResult = "No API key set"; return false; }
  digitalWrite(PINS.tft_cs, HIGH);
  if (!SD.exists(path)) {
    Serial.println("[Wardrift] File does not exist -- attempting SD recovery");
    if (!sdTryRecover() || !SD.exists(path)) {
      Serial.println("[Wardrift] File does not exist");
      uploadLastResult = "File missing";
      return false;
    }
    Serial.println("[Wardrift] SD recovered -- file found");
  }

  File f = SD.open(path, FILE_READ);
  if (!f) {
    Serial.println("[Wardrift] Failed to open file -- attempting SD recovery");
    if (sdTryRecover()) f = SD.open(path, FILE_READ);
  }
  if (!f) { Serial.println("[Wardrift] Failed to open file"); uploadLastResult = "Open fail"; return false; }

  String filename = pathBasename(path);
  uint32_t fileSize = f.size();
  Serial.printf("[Wardrift] File size: %.2f MB (%u bytes)\n", fileSize / (1024.0 * 1024.0), (unsigned)fileSize);

  // Wardrift caps this endpoint's uploaded/decompressed file size at 10 MiB
  // (10,485,760 bytes). Fail fast with a clear reason instead of wasting a
  // TLS round-trip on a guaranteed 413 -- other services (WiGLE/WDGoWars)
  // have higher limits and can still take this file.
  static const uint32_t WARDRIFT_MAX_FILE_BYTES = 10485760UL;
  if (fileSize > WARDRIFT_MAX_FILE_BYTES) {
    f.close();
    uploadLastResult = "File too large for Wardrift (>10 MiB)";
    Serial.printf("[Wardrift] File exceeds 10 MiB limit (%u bytes) -- skipping\n", (unsigned)fileSize);
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure(); client.setTimeout(25000);
  bool connected = false;
  for (int att=1; att<=3; att++) {
    Serial.printf("[Wardrift] Free heap before connect (attempt %d/3): %u bytes\n",
                  att, (unsigned)ESP.getFreeHeap());
    if (client.connect(WARDRIFT_HOST, WARDRIFT_PORT)) { connected=true; break; }
    Serial.printf("[Wardrift] TLS connect failed (attempt %d/3)\n", att);
    client.stop(); delay(500); yield();
  }
  if (!connected) { uploadLastResult = "TLS connect fail"; Serial.println("[Wardrift] TLS connect failed after 3 attempts"); f.close(); return false; }

  // A `.gz`-suffixed path means this is a scratch file already
  // gzip-compressed above -- signal that with the standard Content-Encoding
  // header, same rationale as uploadFileToWdgwars() above.
  bool isGzip = path.endsWith(".gz");
  client.print("POST /v1/ingest/signals HTTP/1.0\r\n");
  client.print(String("Host: ")+WARDRIFT_HOST+"\r\n");
  client.print(String("X-API-Key: ")+cfg.wardriftApiKey+"\r\n");
  client.print("Content-Type: text/csv\r\n");
  if (isGzip) client.print("Content-Encoding: gzip\r\n");
  client.print(String("X-File-Name: ")+filename+"\r\n");
  client.print(String("Content-Length: ")+String(fileSize)+"\r\nConnection: close\r\n\r\n");

  // Stream the raw file bytes directly as the body -- no multipart wrapper.
  uint32_t fileSent = 0; bool wErr = false;
  uint8_t buf[512];
  while (!wErr) {
    int n = f.read(buf, sizeof(buf)); if (n <= 0) break;
    int off = 0;
    while (off < n) { int w = client.write(buf+off, n-off); if (w<=0){wErr=true;break;} off+=w; fileSent+=w; }
    yield();
  }
  f.close();
  client.flush();
  Serial.printf("[Wardrift] ContentLen=%u sent=%u (%s)\n", fileSize, fileSent,
                (fileSent==fileSize)?"MATCH":"*** MISMATCH ***");
  if (wErr || fileSent != fileSize) {
    uploadLastResult = "Write error (body short)"; client.stop();
    Serial.println("[Wardrift] Body write incomplete — aborting");
    return false;
  }

  uint32_t ws = millis();
  while (!client.available() && client.connected() && (millis()-ws)<60000) { delay(100); yield(); }
  uint32_t elapsed = millis()-ws;
  if (!client.available()) {
    bool cc = !client.connected();
    uploadLastResult = cc ? "Conn closed ("+String(elapsed)+"ms)" : "Timeout (60s)";
    Serial.printf("[Wardrift] No data after %lums (%s)\n", (unsigned long)elapsed, cc?"conn closed":"timeout");
    client.stop(); return false;
  }

  String status = client.readStringUntil('\n'); status.trim();
  int code = 0;
  if (status.startsWith("HTTP/")) {
    int s1=status.indexOf(' '); if(s1>0){int s2=status.indexOf(' ',s1+1);if(s2>s1) code=status.substring(s1+1,s2).toInt();}
  }
  String body=""; bool inBody=false;
  while (client.connected()||client.available()) {
    String line=client.readStringUntil('\n'); line.trim();
    if (!inBody){if(line.length()==0) inBody=true;} else{body+=line;if(body.length()>512) break;}
  }
  client.stop();
  Serial.printf("[Wardrift] HTTP %d  body: %s\n", code, body.substring(0,160).c_str());

  if (code == 200) {
    // Synchronous responses use "item_count" (not "accepted_signals", which
    // only appears in the async queued-upload /v1/wardrive/logs response).
    bool isDuplicate = body.indexOf("\"duplicate\":true") >= 0;
    int icIdx = body.indexOf("\"item_count\":");
    String countStr = "";
    if (icIdx >= 0) {
      int start = icIdx + 13, end = start;
      while (end < (int)body.length() && isDigit(body[end])) end++;
      countStr = body.substring(start, end);
    }
    if (isDuplicate) {
      uploadLastResult = "Already uploaded (duplicate)";
    } else {
      uploadLastResult = countStr.length() ? "Uploaded OK (" + countStr + " signals)" : "Uploaded OK (200)";
    }
    return true;
  }

  String msg = "";
  int mIdx = body.indexOf("\"message\":");
  if (mIdx >= 0) { int q1=body.indexOf('"',mIdx+11); int q2=(q1>=0)?body.indexOf('"',q1+1):-1; if(q1>=0&&q2>q1) msg=body.substring(q1+1,q2); }
  uploadLastResult = msg.length() ? ("Upload failed (" + String(code) + "): " + msg) : ("Upload failed (" + String(code) + ")");
  Serial.printf("[Wardrift] Upload FAILED HTTP %d: %s\n", code, body.c_str());
  return false;
}

// ---- WDGoWars background job status check ----
// Does the actual work of checking a single pending job: one short-lived
// HTTPS GET, no retries. Removes the job from the queue once it resolves
// (done/failed) or once it has been pending longer than
// WDGWARS_JOB_MAX_AGE_MS. Returns true if it made a network attempt (or
// gave up on an aged-out job), so callers can pace successive checks.
static bool wdgwarsCheckOnePendingJob() {
  if (wdgwarsPendingJobs.empty()) return false;
  if (WiFi.status() != WL_CONNECTED) return false;

  uint32_t nowMs = millis();

  // Find the least-recently-checked job that's due for a check, using each
  // job's own (growing) backoff interval rather than a fixed one.
  int idx = -1;
  for (size_t i = 0; i < wdgwarsPendingJobs.size(); i++) {
    if (nowMs - wdgwarsPendingJobs[i].lastCheckMs >= wdgwarsPendingJobs[i].nextDelayMs) {
      idx = (int)i;
      break;
    }
  }
  if (idx < 0) return false;

  WdgwarsPendingJob job = wdgwarsPendingJobs[idx];

  if (nowMs - job.queuedAtMs > WDGWARS_JOB_MAX_AGE_MS) {
    Serial.printf("[WDGWars] Job %d (%s): giving up after %lus with no result\n",
                  job.jobId, job.filename.c_str(),
                  (unsigned long)(WDGWARS_JOB_MAX_AGE_MS / 1000));
    wdgwarsPendingJobs.erase(wdgwarsPendingJobs.begin() + idx);
    return true;
  }

  wdgwarsPendingJobs[idx].lastCheckMs = nowMs;
  // Double the wait before the next check on this job (capped), whether
  // this attempt connects or not -- a resolving check erases the entry
  // anyway, so only the "still processing"/connect-fail paths ever see the
  // larger gap take effect.
  wdgwarsPendingJobs[idx].nextDelayMs =
    (job.nextDelayMs > WDGWARS_JOB_CHECK_MAX_MS / 2) ? WDGWARS_JOB_CHECK_MAX_MS : job.nextDelayMs * 2;

  WiFiClientSecure pc;
  pc.setInsecure();
  pc.setTimeout(8000);
  if (!pc.connect(WDGWARS_HOST, WDGWARS_PORT)) {
    logDmaHeap("WDGWars job-poll connect fail");
    return true;
  }

  String jobPath = String("/api/v2/upload-job/") + String(job.jobId);
  pc.print("GET " + jobPath + " HTTP/1.0\r\n");
  pc.print(String("Host: ") + WDGWARS_HOST + "\r\n");
  pc.print(String("X-API-Key: ") + cfg.wdgwarsApiKey + "\r\n");
  pc.print("Connection: close\r\n\r\n");

  uint32_t pw = millis();
  while (!pc.available() && pc.connected() && (millis() - pw) < 6000) { delay(50); yield(); }
  if (!pc.available()) { pc.stop(); return true; }

  pc.readStringUntil('\n');  // skip HTTP status line
  String pb = ""; bool pInBody = false;
  while (pc.connected() || pc.available()) {
    String ln = pc.readStringUntil('\n'); ln.trim();
    if (!pInBody) { if (ln.length() == 0) pInBody = true; }
    else { pb += ln; if (pb.length() > 512) break; }
  }
  pc.stop();

  if (pb.indexOf("\"done\"") >= 0) {
    int imp = 0;
    int ii = pb.indexOf("\"imported\":");
    if (ii >= 0) imp = pb.substring(ii + 11).toInt();
    Serial.printf("[WDGWars] Job %d (%s) done — imported: %d\n",
                  job.jobId, job.filename.c_str(), imp);
    wdgwarsPendingJobs.erase(wdgwarsPendingJobs.begin() + idx);
  } else if (pb.indexOf("\"failed\"") >= 0) {
    Serial.printf("[WDGWars] Job %d (%s) FAILED server-side\n",
                  job.jobId, job.filename.c_str());
    wdgwarsPendingJobs.erase(wdgwarsPendingJobs.begin() + idx);
  }
  // else still "processing" — leave queued, checked again later

  return true;
}

// Call periodically from loop(). Internally rate-limited so it never does
// more than one short network round-trip per call.
static void wdgwarsServicePendingJobs() {
  static uint32_t lastRunMs = 0;
  uint32_t nowMs = millis();
  if (nowMs - lastRunMs < WDGWARS_JOB_CHECK_INTERVAL_MS) return;
  lastRunMs = nowMs;
  wdgwarsCheckOnePendingJob();
}

// Best-effort: keep checking pending jobs back-to-back for up to budgetMs.
// Intended to be called right before intentionally dropping the STA
// connection (e.g. entering mesh Core mode) so quick jobs still get
// confirmed instead of being silently abandoned.
static void wdgwarsDrainPendingJobs(uint32_t budgetMs) {
  if (wdgwarsPendingJobs.empty()) return;

  for (auto& j : wdgwarsPendingJobs) j.lastCheckMs = 0;

  uint32_t start = millis();
  while (!wdgwarsPendingJobs.empty() && (millis() - start) < budgetMs) {
    if (!wdgwarsCheckOnePendingJob()) break;  // WiFi down or nothing due yet
    delay(200);
    yield();
  }

  if (!wdgwarsPendingJobs.empty()) {
    Serial.printf("[WDGWars] %u job(s) still pending — will not be confirmed (STA disconnecting)\n",
                  (unsigned)wdgwarsPendingJobs.size());
  }
}

// ---- SD bus recovery ----
// After sustained heavy SD I/O (large batch uploads at high SPI clock), the
// SD/SPI bus can wedge, causing SD.exists()/SD.open() to fail even though
// the underlying file is intact on the card. Re-running the full boot-time
// init dance (initSD_SharedSPI) usually clears it, since it already handles
// this board's shared TFT/SD SPI bus quirks. Rate-limited so a genuinely
// dead card doesn't get hammered with a remount attempt on every file.
static uint32_t sdLastRecoverMs = 0;
static bool sdTryRecover() {
  uint32_t now = millis();
  if (now - sdLastRecoverMs < 4000) return false;
  sdLastRecoverMs = now;

  Serial.println("[SD] Attempting SD bus recovery (full re-init)...");
  bool ok = initSD_SharedSPI();
  sdOk = ok;
  Serial.printf("[SD] Recovery %s\n", ok ? "OK" : "FAILED");
  return ok;
}

// ---- Empty-file guard: true if file has any data beyond the 2 header lines ----
// `outSize` (optional) receives the file's byte size for diagnostic logging.
//
// IMPORTANT: if the file can't even be opened, this returns true ("assume it
// has data") rather than false. Callers only delete a file when this returns
// false, so a transient SD/SPI error must never look identical to "verified
// empty" -- that conflation previously caused real, non-empty files to be
// deleted en masse whenever the SD card hit a rough patch mid-session.
static bool csvHasDataRows(const String& path, uint32_t* outSize = nullptr) {
  if (outSize) *outSize = 0;
  File f = SD.open(path, FILE_READ);
  if (!f) {
    Serial.printf("[SD] csvHasDataRows: could not open %s -- attempting SD recovery\n",
                  pathBasename(path).c_str());
    if (sdTryRecover()) f = SD.open(path, FILE_READ);
  }
  if (!f) {
    Serial.printf("[SD] csvHasDataRows: could not open %s -- assuming it HAS data (not deleting)\n",
                  pathBasename(path).c_str());
    return true;
  }
  if (outSize) *outSize = f.size();
  for (int i=0; i<2; i++) { if(!f.available()){f.close();return false;} f.readStringUntil('\n'); }
  bool hasData = f.available()>0;
  f.close();
  return hasData;
}

// ---- No-GPS-fix guard ----
// Returns true only if the file has at least one data row AND every one of
// those rows has lat=lon=0.000000 -- "null island", the exact placeholder
// appendWigleRow()-equivalent logging writes when GPS has never had a fix
// this session. A file like this is either a bench/indoor test capture or a
// session where GPS never locked, and is exactly the shape Wardrift's "no
// supported signal rows" validator rejects (it treats 0,0 as no-fix, not a
// real location) -- confirmed against real uploads where every row sent was
// rejected this way. Scans only as far as the first non-zero coordinate
// before stopping, so a large real-data file is cheap to clear.
//
// Relies on the row format this file's log writer uses: field 0 is the
// unquoted MAC, field 1 is the double-quoted SSID (the only quoted field in
// a WiGLE data row, per RFC4180), and everything after it --
// AuthMode,FirstSeen,Channel,Frequency,RSSI,Lat,Lon,... -- is plain,
// unquoted CSV. Lat/Lon are always formatted with exactly 6 decimal places
// ("%.6f"), so a genuine no-fix row is always the literal string
// "0.000000", not just numerically close to zero.
static bool csvAllRowsZeroGps(const String& path) {
  File f = SD.open(path, FILE_READ);
  if (!f) return false;  // can't verify -- never delete on an unknown

  for (int i = 0; i < 2; i++) {
    if (!f.available()) { f.close(); return false; }  // no data rows -- csvHasDataRows() handles this
    f.readStringUntil('\n');
  }

  bool sawAnyRow = false;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;

    int c1 = line.indexOf(',');
    if (c1 < 0) continue;
    int i = c1 + 1;
    int n = line.length();
    if (i >= n || line[i] != '"') continue;

    i++;
    while (i < n) {
      if (line[i] == '"') {
        if (i + 1 < n && line[i + 1] == '"') { i += 2; continue; }
        i++;
        break;
      }
      i++;
    }
    if (i >= n || line[i] != ',') continue;

    String rest = line.substring(i + 1);
    int pos = 0, field = 0;
    String lat, lon;
    while (field <= 6) {
      int comma = rest.indexOf(',', pos);
      String val = (comma < 0) ? rest.substring(pos) : rest.substring(pos, comma);
      if (field == 5) lat = val;
      if (field == 6) lon = val;
      if (comma < 0) break;
      pos = comma + 1;
      field++;
    }

    sawAnyRow = true;
    if (lat != "0.000000" || lon != "0.000000") {
      f.close();
      return false;
    }
  }
  f.close();

  return sawAnyRow;
}

// ---- Shared file collection for batch uploads ----
// Enumerates /logs/*.csv (excluding the active log file), deleting
// header-only files along the way. Returns the remaining eligible paths,
// capped to maxFiles if positive.
static std::vector<String> collectEligibleCsvs(int maxFiles) {
  std::vector<String> all;
  digitalWrite(PINS.tft_cs, HIGH);
  File root = SD.open("/logs");
  if (root) {
    File f = root.openNextFile();
    while (f) {
      f.setBufferSize(64);
      String name = normalizeSdPath("/logs", f.name());
      // Explicitly skip dot-prefixed housekeeping entries (the .gzscratch
      // scratch directory, the .upload_state.cfg ledger) -- redundant with
      // the .csv suffix check below today, but makes the exclusion explicit
      // and keeps it safe if either ever changes shape.
      bool isHidden = isHiddenSdEntry(pathBasename(name));
      bool isCsv = name.endsWith(".csv");
      bool isCurrent = (currentCsvPath.length() && name == currentCsvPath);
      f.close();
      if (isCsv && !isHidden && !isCurrent) all.push_back(name);
      f = root.openNextFile();
    }
    root.close();
  }

  std::vector<String> eligible;
  eligible.reserve(all.size());
  for (const String& p : all) {
    uint32_t emptySz = 0;
    if (!csvHasDataRows(p, &emptySz)) {
      Serial.printf("[UPLOAD] Empty CSV (%u bytes), deleting: %s\n", (unsigned)emptySz, pathBasename(p).c_str());
      if (!SD.remove(p)) Serial.printf("[UPLOAD] WARNING: SD.remove failed for %s -- file left in place\n", pathBasename(p).c_str());
      clearUploadState(pathBasename(p));
      continue;
    }
    if (csvAllRowsZeroGps(p)) {
      Serial.printf("[UPLOAD] No GPS fix in any row -- deleting bench-test/no-lock capture: %s\n", pathBasename(p).c_str());
      if (!SD.remove(p)) Serial.printf("[UPLOAD] WARNING: SD.remove failed for %s -- file left in place\n", pathBasename(p).c_str());
      clearUploadState(pathBasename(p));
      continue;
    }
    eligible.push_back(p);
  }

  if (maxFiles > 0 && (uint32_t)maxFiles < eligible.size()) eligible.resize((size_t)maxFiles);
  return eligible;
}

// ---- WDGoWars-only batch upload (web UI button) ----
// Uploads every file still pending for WDGoWars (gzip per file), then
// moves any file that now has every *configured* service done -- not just
// WDGoWars -- to /uploaded/.
static uint32_t uploadAllCsvsToWdgwars() {
  if (!sdOk) { uploadLastResult="SD not OK"; return 0; }
  if (cfg.wdgwarsApiKey.length()<8) { uploadLastResult="No API key"; return 0; }

  uploadPausedScanWasEnabled=scanningEnabled; scanningEnabled=false;
  uploading=true; uploadTargetName="WDGW UL";
  uploadDoneFiles=0; uploadFailedFiles=0; uploadTotalFiles=0; uploadCurrentFile="";
  ledBlue();

  std::vector<String> paths = collectEligibleCsvs(-1);
  uploadTotalFiles = (uint32_t)paths.size();
  if (paths.empty()) { uploading=false; uploadTargetName=""; scanningEnabled=uploadPausedScanWasEnabled; uploadLastResult="No CSVs to upload"; ledOff(); return 0; }

  ensureGzScratchDir();

  uint32_t okCount=0;
  for(size_t i=0;i<paths.size();i++){
    uploadCurrentFile=paths[i];
    tftWigleUploadScreen(uploadDoneFiles,uploadTotalFiles,pathBasename(uploadCurrentFile));

    String basename = pathBasename(paths[i]);
    FileUploadState st = loadUploadState(basename);
    if (!st.wdgwDone) {
      Serial.printf("[UPLOAD] File %u/%u: %s (heap free: %u bytes)\n",
                    (unsigned)(i + 1), (unsigned)paths.size(), basename.c_str(), (unsigned)ESP.getFreeHeap());
      bool ok;
      if (cfg.gzipUploads) {
        String scratch = gzScratchPathFor(basename);
        ok = gzipCompressFileToFile(paths[i], scratch) && uploadFileToWdgwars(scratch);
        SD.remove(scratch);
      } else {
        ok = uploadFileToWdgwars(paths[i]);
      }
      if (ok) { markServiceDone(basename, UploadService::Wdgw); st.wdgwDone = true; }
      else uploadFailedFiles++;
    }

    if (allConfiguredServicesDone(st)) {
      okCount++;
      // See the matching comment in uploadAllPending()'s finalize loop --
      // only clear the ledger once the move actually succeeds, so a
      // transient SD hiccup can't make an already-uploaded file look unsent
      // again on the next run.
      if (moveToUploaded(paths[i])) clearUploadState(basename);
      else Serial.printf("[UPLOAD] WARNING: moveToUploaded failed for %s -- file stays in /logs, ledger kept\n", basename.c_str());
    }
    uploadDoneFiles++;
    tftWigleUploadScreen(uploadDoneFiles,uploadTotalFiles,pathBasename(uploadCurrentFile));
    if(i<paths.size()-1) delay(1500);
  }
  uploading=false; uploadTargetName=""; scanningEnabled=uploadPausedScanWasEnabled;
  uploadCurrentFile=""; uploadLastResult="WDGWars: "+String(okCount)+"/"+String(uploadTotalFiles);
  ledOff(); forceStatusFullRedraw();
  return okCount;
}

// ---- Wardrift-only batch upload (web UI button) ----
// Same shape as uploadAllCsvsToWdgwars() above, for Wardrift.
static uint32_t uploadAllCsvsToWardrift() {
  if (!sdOk) { uploadLastResult="SD not OK"; return 0; }
  if (cfg.wardriftApiKey.length()<8) { uploadLastResult="No token set"; return 0; }

  uploadPausedScanWasEnabled=scanningEnabled; scanningEnabled=false;
  uploading=true; uploadTargetName="Wardrift UL";
  uploadDoneFiles=0; uploadFailedFiles=0; uploadTotalFiles=0; uploadCurrentFile="";
  ledBlue();

  std::vector<String> paths = collectEligibleCsvs(-1);
  uploadTotalFiles = (uint32_t)paths.size();
  if (paths.empty()) { uploading=false; uploadTargetName=""; scanningEnabled=uploadPausedScanWasEnabled; uploadLastResult="No CSVs to upload"; ledOff(); return 0; }

  ensureGzScratchDir();

  uint32_t okCount=0;
  for(size_t i=0;i<paths.size();i++){
    uploadCurrentFile=paths[i];
    tftWigleUploadScreen(uploadDoneFiles,uploadTotalFiles,pathBasename(uploadCurrentFile));

    String basename = pathBasename(paths[i]);
    FileUploadState st = loadUploadState(basename);
    if (!st.wardriftDone) {
      Serial.printf("[UPLOAD] File %u/%u: %s (heap free: %u bytes)\n",
                    (unsigned)(i + 1), (unsigned)paths.size(), basename.c_str(), (unsigned)ESP.getFreeHeap());
      // Always sent uncompressed -- see the matching comment in
      // uploadAllPending()'s Phase 3 for why (Wardrift doesn't honor
      // Content-Encoding: gzip on this endpoint).
      bool ok = uploadFileToWardrift(paths[i]);
      if (ok) { markServiceDone(basename, UploadService::Wardrift); st.wardriftDone = true; }
      else uploadFailedFiles++;
    }

    if (allConfiguredServicesDone(st)) {
      okCount++;
      // See the matching comment in uploadAllPending()'s finalize loop --
      // only clear the ledger once the move actually succeeds.
      if (moveToUploaded(paths[i])) clearUploadState(basename);
      else Serial.printf("[UPLOAD] WARNING: moveToUploaded failed for %s -- file stays in /logs, ledger kept\n", basename.c_str());
    }
    uploadDoneFiles++;
    tftWigleUploadScreen(uploadDoneFiles,uploadTotalFiles,pathBasename(uploadCurrentFile));
    if(i<paths.size()-1) delay(1500);
  }
  uploading=false; uploadTargetName=""; scanningEnabled=uploadPausedScanWasEnabled;
  uploadCurrentFile=""; uploadLastResult="Wardrift: "+String(okCount)+"/"+String(uploadTotalFiles);
  ledOff(); forceStatusFullRedraw();
  return okCount;
}

// ---------------- WiGLE Upload ----------------
static bool moveToUploaded(const String& srcPath) {
  if (!sdOk) return false;
  digitalWrite(PINS.tft_cs, HIGH);
  if (!SD.exists(srcPath)) {
    Serial.println("[SD] moveToUploaded: source missing, attempting recovery");
    if (!sdTryRecover() || !SD.exists(srcPath)) {
      Serial.println("[SD] moveToUploaded: source missing");
      return false;
    }
    Serial.println("[SD] moveToUploaded: SD recovered, source found");
  }
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
    // Retry TLS connect -- see the matching comment in wdgwarsTestKey() for
    // why a single connect() attempt here can fail transiently.
    bool wigleConnected = false;
    for (int attempt = 1; attempt <= 3; attempt++) {
      if (client.connect(WIGLE_HOST, WIGLE_PORT)) { wigleConnected = true; break; }
      char errBuf[100];
      int sslErr = client.lastError(errBuf, sizeof(errBuf));
      Serial.printf("[WiGLE] Test token TLS connect failed (attempt %d/3): error %d: %s\n", attempt, sslErr, errBuf);
      logDmaHeap("WiGLE test token connect fail");
      client.stop(); delay(500); yield();
    }
    if (!wigleConnected) { uploadLastResult = "TLS connect fail"; wigleTokenStatus = -1; return false; }
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
  if (!SD.exists(path)) {
    Serial.println("[WiGLE] File missing -- attempting SD recovery");
    if (!sdTryRecover() || !SD.exists(path)) {
      uploadLastResult = "File missing";
      return false;
    }
    Serial.println("[WiGLE] SD recovered -- file found");
  }

  File f = SD.open(path, FILE_READ);
  if (!f) {
    Serial.println("[WiGLE] Failed to open file -- attempting SD recovery");
    if (sdTryRecover()) f = SD.open(path, FILE_READ);
  }
  if (!f) { uploadLastResult = "Open fail"; return false; }

  String boundary = "----Piglet-WARDRIVE-BOUNDARY";
  String filename = pathBasename(path);
  String pre = "--" + boundary + "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"" + filename + "\"\r\nContent-Type: text/csv\r\n\r\n";
  String post = "\r\n--" + boundary + "--\r\n";
  uint32_t fileSize = f.size();
  uint32_t contentLen = (uint32_t)pre.length() + fileSize + (uint32_t)post.length();
  Serial.printf("[WiGLE] File size: %.2f MB (%u bytes)\n", fileSize / (1024.0 * 1024.0), (unsigned)fileSize);

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(25000);
  Serial.printf("[WiGLE] Free heap before connect: %u bytes\n", (unsigned)ESP.getFreeHeap());
  if (!client.connect(WIGLE_HOST, WIGLE_PORT)) { uploadLastResult = "TLS connect fail"; f.close(); return false; }

  client.print(String("POST /api/v2/file/upload HTTP/1.0\r\nHost: ") + WIGLE_HOST +
               "\r\nAuthorization: Basic " + cfg.wigleBasicToken +
               "\r\nContent-Type: multipart/form-data; boundary=" + boundary +
               "\r\nContent-Length: " + String(contentLen) + "\r\nConnection: close\r\n\r\n");
  client.print(pre);

  // Stream the file, tracking actual bytes sent so a short/failed SD read
  // (e.g. SD card under stress) is detected and aborted cleanly instead of
  // silently sending a body shorter than the Content-Length already sent.
  uint32_t fileSent = 0;
  uint8_t buf[1024];
  while (true) {
    int n = f.read(buf, sizeof(buf));
    if (n <= 0) break;
    size_t w = client.write(buf, n);
    fileSent += (uint32_t)w;
    if (w != (size_t)n) break;
    delay(0);
  }
  f.close();

  if (fileSent != fileSize) {
    client.stop();
    uploadLastResult = "Read/write error (body short)";
    Serial.printf("[WiGLE] Body incomplete: sent %u of %u bytes -- aborting\n",
                  (unsigned)fileSent, (unsigned)fileSize);
    return false;
  }

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
  Serial.printf("[WiGLE] Upload FAILED HTTP %d\n", code);
  return false;
}

// ---- Combined batch upload (service-major, with per-file/per-service retry tracking) ----
// Uploads every pending file to WiGLE first (as one merged, gzip-compressed
// archive when more than one file is pending), then every file still
// pending to WDGoWars (gzip per file), then every file still pending to
// Wardrift (gzip per file). A file only moves to /uploaded/ once every
// *configured* service has succeeded for it; a failure against one service
// never blocks or re-triggers the others for that file.
static uint32_t uploadAllPending(int maxFiles = -1) {
  if (!sdOk) { uploadLastResult = "SD not OK"; return 0; }

  uploadPausedScanWasEnabled = scanningEnabled;
  scanningEnabled = false;
  uploading = true;
  uploadDoneFiles = 0;
  uploadFailedFiles = 0;
  uploadTotalFiles = 0;
  uploadCurrentFile = "";
  ledBlue();

  std::vector<String> paths = collectEligibleCsvs(maxFiles);
  if (paths.empty()) {
    uploading = false; scanningEnabled = uploadPausedScanWasEnabled;
    uploadLastResult = "No CSVs to upload"; ledOff(); return 0;
  }

  ensureGzScratchDir();

  std::vector<FileUploadState> states(paths.size());
  for (size_t i = 0; i < paths.size(); i++) {
    states[i] = loadUploadState(pathBasename(paths[i]));
  }

  // Progress (TFT/web UI) tracks individual upload ATTEMPTS across all three
  // service phases, not "every configured service done for this file" -- a
  // file can succeed WiGLE but still be mid-flight on WDGoWars/Wardrift, and
  // the operator should see the bar/counter visibly tick forward on every
  // attempt (success or fail), matching what the serial log shows, instead
  // of sitting still until a file clears every service.
  for (size_t i = 0; i < paths.size(); i++) {
    if (!states[i].wigleDone)    uploadTotalFiles++;
    if (!states[i].wdgwDone)     uploadTotalFiles++;
    if (!states[i].wardriftDone) uploadTotalFiles++;
  }
  auto touchFile = [&](size_t /*idx*/) { uploadDoneFiles++; };

  // ---- Phase 1: WiGLE ----
  if (cfg.wigleBasicToken.length() >= 8) {
    std::vector<size_t> pendingIdx;
    for (size_t i = 0; i < paths.size(); i++) if (!states[i].wigleDone) pendingIdx.push_back(i);

    if (!pendingIdx.empty()) {
      uploadTargetName = "WiGLE UL";
      if (cfg.gzipUploads && pendingIdx.size() > 1) {
        uploadCurrentFile = "(" + String(pendingIdx.size()) + " files)";
        tftWigleUploadScreen(uploadDoneFiles, uploadTotalFiles, uploadCurrentFile);
        std::vector<String> srcPaths;
        for (size_t idx : pendingIdx) srcPaths.push_back(paths[idx]);
        String scratch = gzScratchPathFor("wigle_batch");
        bool ok = gzipCompressMergedFilesToFile(srcPaths, scratch) && uploadFileToWigle(scratch);
        SD.remove(scratch);
        if (ok) Serial.printf("[WiGLE] Merged archive OK -- %u file(s) marked done\n", (unsigned)pendingIdx.size());
        else { Serial.printf("[WiGLE] Merged archive FAILED -- %u file(s) remain pending\n", (unsigned)pendingIdx.size()); uploadFailedFiles++; }
        for (size_t idx : pendingIdx) {
          if (ok) { markServiceDone(pathBasename(paths[idx]), UploadService::Wigle); states[idx].wigleDone = true; }
          touchFile(idx);
        }
      } else {
        for (size_t n = 0; n < pendingIdx.size(); n++) {
          size_t idx = pendingIdx[n];
          uploadCurrentFile = paths[idx];
          tftWigleUploadScreen(uploadDoneFiles, uploadTotalFiles, pathBasename(uploadCurrentFile));
          bool ok;
          if (cfg.gzipUploads) {
            String scratch = gzScratchPathFor(pathBasename(paths[idx]));
            ok = gzipCompressFileToFile(paths[idx], scratch) && uploadFileToWigle(scratch);
            SD.remove(scratch);
          } else {
            ok = uploadFileToWigle(paths[idx]);
          }
          if (ok) { markServiceDone(pathBasename(paths[idx]), UploadService::Wigle); states[idx].wigleDone = true; }
          else uploadFailedFiles++;
          touchFile(idx);
          if (n + 1 < pendingIdx.size()) delay(1500);
        }
      }
    }
  }

  // ---- Phase 2: WDGoWars (per file) ----
  if (cfg.wdgwarsApiKey.length() >= 8) {
    for (size_t i = 0; i < paths.size(); i++) {
      if (states[i].wdgwDone) continue;
      uploadTargetName = "WDGW UL";
      uploadCurrentFile = paths[i];
      tftWigleUploadScreen(uploadDoneFiles, uploadTotalFiles, pathBasename(uploadCurrentFile));
      Serial.printf("[WDGWars] %s (heap free: %u bytes)\n", pathBasename(paths[i]).c_str(), (unsigned)ESP.getFreeHeap());

      bool ok;
      if (cfg.gzipUploads) {
        String scratch = gzScratchPathFor(pathBasename(paths[i]));
        ok = gzipCompressFileToFile(paths[i], scratch) && uploadFileToWdgwars(scratch);
        SD.remove(scratch);
      } else {
        ok = uploadFileToWdgwars(paths[i]);
      }
      if (ok) { markServiceDone(pathBasename(paths[i]), UploadService::Wdgw); states[i].wdgwDone = true; }
      else uploadFailedFiles++;
      touchFile(i);
      if (i + 1 < paths.size()) delay(1500);
    }
  }

  // ---- Phase 3: Wardrift (per file) ----
  if (cfg.wardriftApiKey.length() >= 8) {
    for (size_t i = 0; i < paths.size(); i++) {
      if (states[i].wardriftDone) continue;
      uploadTargetName = "Wardrift UL";
      uploadCurrentFile = paths[i];
      tftWigleUploadScreen(uploadDoneFiles, uploadTotalFiles, pathBasename(uploadCurrentFile));
      Serial.printf("[Wardrift] %s (heap free: %u bytes)\n", pathBasename(paths[i]).c_str(), (unsigned)ESP.getFreeHeap());

      // Always sent uncompressed -- Wardrift's /v1/ingest/signals endpoint
      // does not honor Content-Encoding: gzip (confirmed in the field: every
      // gzip-compressed upload came back HTTP 400 "file did not contain
      // supported signal rows" -- it was trying to parse the raw gzip bytes
      // as CSV text -- even though the identical file succeeded against
      // WDGoWars, which does support gzip). Ignore cfg.gzipUploads here.
      bool ok = uploadFileToWardrift(paths[i]);
      if (ok) { markServiceDone(pathBasename(paths[i]), UploadService::Wardrift); states[i].wardriftDone = true; }
      else uploadFailedFiles++;
      touchFile(i);
      if (i + 1 < paths.size()) delay(1500);
    }
  }

  // ---- Finalize: move fully-done files to /uploaded/ ----
  uint32_t okCount = 0;
  for (size_t i = 0; i < paths.size(); i++) {
    if (allConfiguredServicesDone(states[i])) {
      okCount++;
      // Only clear the ledger entry once the move actually succeeds. Every
      // configured service already succeeded for this file at this point --
      // clearing the ledger unconditionally here (even when moveToUploaded()
      // itself failed, e.g. a transient SD hiccup) made that look identical
      // to "never uploaded" on the next run, which re-sent it to every
      // service all over again. Leaving the entry intact just retries the
      // move next time, with no re-upload.
      if (moveToUploaded(paths[i])) {
        clearUploadState(pathBasename(paths[i]));
      } else {
        Serial.printf("[UPLOAD] WARNING: moveToUploaded failed for %s -- file stays in /logs, ledger kept so it won't be re-uploaded\n",
                      pathBasename(paths[i]).c_str());
      }
    }
  }

  uploading = false;
  uploadTargetName = "";
  scanningEnabled = uploadPausedScanWasEnabled;
  uploadCurrentFile = "";
  uploadDoneFiles = uploadTotalFiles;
  // uploadTotalFiles is the attempts-based progress denominator above, not
  // the distinct file count -- use paths.size() here so this summary reads
  // naturally ("Uploaded 23/25" rather than counting every per-service try).
  uploadLastResult = "Uploaded " + String(okCount) + "/" + String(paths.size());
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

  // ---- On-device firmware update flow -- full-screen takeover, same idiom
  // as the OLED Piglet's drawPageStatus(). Entirely TFT/button driven: no
  // browser needs to be open for any of this, which matters here since the
  // download's TLS session is already fighting for a very narrow slice of
  // DMA-capable heap -- Web UI polling traffic would otherwise compete for
  // the same shared WiFi buffer pool at exactly the wrong moment.
  if (fwDownloading || fwInstalling || fwDownloaded || fwUpdateAvailable) {
    tft.fillScreen(BLACK);
    tft.setTextColor(WHITE, BLACK);
    tftDrawCentered(10, fwInstalling ? "Installing" : (fwDownloading ? "Updating" : "Update!"), 2);

    int y = 40;
    if (fwDownloading || fwInstalling) {
      String stage = fwDownloadStage;
      if (stage.length() > 13) stage = stage.substring(0, 13);
      tftDrawCentered(y, stage.c_str(), 1);
      y += 16;
      if (fwDownloadTotalBytes > 0) {
        float pct = (float)fwDownloadedBytes / (float)fwDownloadTotalBytes;
        int barW = W - 16;
        tft.drawRect(8, y, barW, 10, WHITE);
        int fill = (int)((barW - 2) * pct);
        if (fill > 0) tft.fillRect(9, y + 1, fill, 8, WHITE);
        y += 16;
        char pctBuf[8];
        snprintf(pctBuf, sizeof(pctBuf), "%d%%", (int)(pct * 100));
        tftDrawCentered(y, pctBuf, 1);
      }
    } else if (fwDownloaded) {
      tftDrawCentered(y, "Download", 1); y += 12;
      tftDrawCentered(y, "complete!", 1); y += 20;
      tftDrawCentered(y, "Press button to", 1); y += 12;
      tftDrawCentered(y, "restart & install", 1);
    } else {  // fwUpdateAvailable
      tftDrawCentered(y, fwAvailableVersion.c_str(), 1); y += 20;
      tftDrawCentered(y, "Press button to", 1); y += 12;
      tftDrawCentered(y, "download", 1);
    }

    uiFirstDraw = true;  // force a clean full redraw once this state clears
    return;
  }

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
static const uint32_t NODE_ADMIN_WIN_MS   = 500;   // ch-6 window after each scan cycle
#define JCMK_TEXT_MAX 200

// ---- Piglet-to-Piglet identification + transmit-slot scheduling ----
// A short marker carried in the otherwise-always-empty text/len fields of the
// existing CORE_REQUEST/CORE_REPLY/HEARTBEAT messages. Neither real JCMK
// hardware nor Biscuit nodes look at this payload, so it's fully backward
// compatible; it only ever activates the new scheduling path when BOTH sides
// of a pairing are confirmed to be genuine Piglet devices.
static const char*   PIGLET_MARKER     = "PIGLET1";
static const uint8_t PIGLET_MARKER_LEN = 7;
// Board-type byte (PigletBoardType, OtaProto.h) immediately follows the
// marker text, letting a Core pick the right /Firmware/<Board>.ota.bin
// image for a connected node -- every Piglet variant is ESP32-C5, so the
// image's own chip_id can't disambiguate boards the way it could for a
// mixed-silicon fleet. See coreOtaStart() below.
static const uint8_t PIGLET_MARKER_TOTAL_LEN = PIGLET_MARKER_LEN + 1;
#define PIGLET_THIS_BOARD_TYPE PIGLET_BOARD_TDONGLE
// Fixed-width, null-padded copy of FIRMWARE_VERSION immediately follows the
// board-type byte, letting a Core detect a connected Piglet/TDongleC5_Piglet
// node running different firmware than itself (see the on-device "Update
// Nodes through Core?" prompt in coreModeTick()/drawPageMeshNode()). A node
// still on the shorter pre-version marker reports an empty version string --
// treated as "unknown", never flagged as a mismatch.
static const uint8_t PIGLET_VERSION_LEN = 8;
static const uint8_t PIGLET_MARKER_TOTAL_LEN_V2 = PIGLET_MARKER_TOTAL_LEN + PIGLET_VERSION_LEN;

// Deterministic transmit-slot width for Piglet-to-Piglet nodes. Each
// confirmed-Piglet node gets an exclusive [slotIndex*SLOT_MS, +SLOT_MS) window
// within a repeating cycle (cycle length = SLOT_MS * active-Piglet-node-count),
// so scan results are sent without colliding with other nodes' transmissions.
static const uint32_t JCMK_SLOT_MS = 200;

// If a Node hasn't heard anything from its Core in this long, assume it's gone
// and return to searching (mirrors Biscuit's own CORE_TIMEOUT_MS behavior).
static const uint32_t JCMK_CORE_TIMEOUT_MS = 30000;

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
static uint32_t jcmkSendFailCount = 0;
static uint32_t jcmkHbCounter   = 0;
// Separate, always-incrementing counter for TEXT (scan-result) messages --
// see the comment in the main Piglet firmware's MeshNode.cpp for why reusing
// jcmkHbCounter here caused every TEXT message in a cycle to share an
// identical, stale counter value (which can look like a duplicate to a Core
// that dedupes by (sender, counter), silently dropping all of them).
static uint32_t jcmkTextCounter = 0;
static uint32_t jcmkLastHbMs    = 0;
static uint32_t jcmkLastReqMs   = 0;
static uint32_t jcmkReqInterval = JCMK_REQ_INIT_MS;

// Per-channel async scan state
static bool     nodeScanActive   = false;
static uint8_t  nodeScanChOffset = 0;
static bool     nodeScanAdminWin = false;
static uint32_t nodeScanAdminMs  = 0;

// Channel list the Wardrive Go phone app has explicitly narrowed scanning to (via
// `-ch`/`ch`), refreshed each nodeDoScanTick() call. Empty unless the bridge is active
// AND the app asked for fewer than every supported channel -- see wardriveGoChannelOverride().
static uint8_t  wgOverrideChannels[48];
static uint8_t  wgOverrideCount = 0;

// Pending core-found event — set from ESP-Now callback, consumed in loop
static volatile bool  jcmkCoreFoundPending    = false;
static uint8_t        jcmkCoreMacPending[6]   = {0};
static volatile bool  jcmkCoreIsPigletPending = false;
static volatile bool  biscuitPlainReplyPending = false;
static uint8_t biscuitChannels[JCMK_NUM_CHANNELS + 2];
static volatile uint8_t biscuitChannelCount = 0;

// Piglet-to-Piglet transmit-slot state (only meaningful when jcmkCoreIsPiglet)
static bool     jcmkCoreIsPiglet   = false;
static uint8_t  jcmkSlotIndex      = 0;
static uint8_t  jcmkSlotCount      = 1;
static uint32_t jcmkCycleEpochMs   = 0;
static uint32_t jcmkCoreLastSeenMs = 0;  // for JCMK_CORE_TIMEOUT_MS detection

// Ring buffer of scan-result lines awaiting this node's transmit slot.
// Scanning (nodeDoScanTick) keeps running at full pace regardless of buffer
// state; only sending already-found results is deferred to the slot window.
#define JCMK_PENDING_MAX 64
struct JcmkPendingLine { char text[96]; };
static JcmkPendingLine jcmkPendingBuf[JCMK_PENDING_MAX];
static uint8_t         jcmkPendingHead = 0, jcmkPendingTail = 0;

static bool jcmkPendingEmpty() { return jcmkPendingHead == jcmkPendingTail; }

static bool jcmkPendingPush(const String& line) {
  uint8_t next = (jcmkPendingTail + 1) % JCMK_PENDING_MAX;
  if (next == jcmkPendingHead) return false;  // full — drop rather than block scanning
  size_t n = line.length();
  if (n >= sizeof(jcmkPendingBuf[0].text)) n = sizeof(jcmkPendingBuf[0].text) - 1;
  memcpy(jcmkPendingBuf[jcmkPendingTail].text, line.c_str(), n);
  jcmkPendingBuf[jcmkPendingTail].text[n] = '\0';
  jcmkPendingTail = next;
  return true;
}

static bool jcmkPendingPop(String& out) {
  if (jcmkPendingEmpty()) return false;
  out = jcmkPendingBuf[jcmkPendingHead].text;
  jcmkPendingHead = (jcmkPendingHead + 1) % JCMK_PENDING_MAX;
  return true;
}

// ---- Core mode state ----
#define CORE_MAX_NODES 12
struct CoreNodeInfo {
  bool     active;
  uint8_t  mac[6];
  uint8_t  startIdx, endIdx;
  uint32_t lastHbMs;
  uint32_t recordsRx;
  bool     isBiscuit;  // true = Biscuit Node protocol (requires full-size 212-byte packets)
  bool     isPiglet;   // true = confirmed genuine Piglet node (via marker handshake)
  uint8_t  slotIndex;  // this node's transmit slot among other Piglet nodes (valid when isPiglet)
  uint8_t  boardType;  // PigletBoardType (PIGLET_BOARD_UNKNOWN unless isPiglet) -- which
                        // /Firmware/<Board>.ota.bin image this node can be pushed
  char     version[9]; // node's reported FIRMWARE_VERSION, empty if unknown (pre-version node)
  bool     updateDeclined;      // user let the on-device update prompt time out for this node
  bool     autoUpdateAttempted; // headless-node auto-push already tried this session (no retry loop)
};
static bool         meshCoreActive  = false;
static uint32_t     coreRecordsRx   = 0;
static uint8_t      coreNodeCount   = 0;
static CoreNodeInfo coreNodes[CORE_MAX_NODES] = {};
static uint8_t      coreAssignVer   = 0;
static uint32_t     coreLastHbMs    = 0;
static uint32_t     coreHbCounter   = 0;
static uint8_t      corePigletSlotCount = 0;  // active Piglet-confirmed node count; recomputed in coreReassignChannels()

// Per-5GHz-channel yield tracking (see YieldSched.h), fed from every TEXT
// record the Core receives regardless of which node reported it. Folded into
// an EWMA every ~4s and used only to ORDER the channel list a Biscuit node is
// given -- the channel SET/COUNT a node is assigned is unaffected, so this is
// a no-op for real-JCMK/Piglet-to-Piglet nodes (which never see this list;
// they keep the packed binary ADMIN message, unchanged).
#define CORE_5G_COUNT 26  // JCMK_NUM_CHANNELS(40) - 14 2.4GHz entries
static ChanStat  core5gStat[CORE_5G_COUNT];
static uint16_t  core5gHits[CORE_5G_COUNT];
static uint32_t  coreYieldLastMs = 0;
static SchedCfg  coreSchedCfg;
static const uint32_t CORE_HB_MS         = 5000;
static const uint32_t CORE_NODE_TIMEOUT  = 90000;  // 90 s — generous for many-node ESP-Now collisions

#define CORE_REQ_QUEUE  16
#define CORE_TEXT_QUEUE 192  // sized for burst from 12 nodes x ~15 networks each
struct CorReqSlot  { uint8_t mac[6]; bool isBiscuit; bool isPiglet; uint8_t boardType; char version[9]; };
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

// ESP-Now send-status callback -- the only way to learn whether esp_now_send()
// actually made it onto the air. esp_now_send() itself only reports whether the
// driver *accepted* the request, not whether the frame was transmitted; a
// counter that only tracks calls made (like jcmkSentCount) can look perfectly
// healthy locally while every frame is silently failing at the radio layer.
static void jcmkOnSent(const esp_now_send_info_t* txInfo, esp_now_send_status_t status) {
  if (status != ESP_NOW_SEND_SUCCESS) {
    jcmkSendFailCount++;
    if (txInfo && txInfo->des_addr) {
      const uint8_t* d = txInfo->des_addr;
      Serial.printf("[MESH] Send FAILED (no ACK) to %02X:%02X:%02X:%02X:%02X:%02X\n",
        d[0], d[1], d[2], d[3], d[4], d[5]);
    }
  }
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

// Writes the marker + board-type byte + null-padded version string starting
// at `text[0]` and returns the total marker length (PIGLET_MARKER_TOTAL_LEN_V2).
// Shared by every marker-sending function below so the version encoding
// stays in sync. Takes a raw char* (not jcmk_text_msg_t&) because the
// Arduino .ino preprocessor auto-generates function prototypes near the top
// of the file, before the jcmk_text_msg_t typedef is defined further down --
// any top-level function taking that type directly fails to compile (same
// issue noted on biscuitSetChannels() below).
static uint16_t fillPigletMarker(char* text) {
  memcpy(text, PIGLET_MARKER, PIGLET_MARKER_LEN);
  text[PIGLET_MARKER_LEN] = (char)PIGLET_THIS_BOARD_TYPE;
  size_t vlen = strlen(FIRMWARE_VERSION);
  if (vlen > PIGLET_VERSION_LEN) vlen = PIGLET_VERSION_LEN;
  memset(text + PIGLET_MARKER_TOTAL_LEN, 0, PIGLET_VERSION_LEN);
  memcpy(text + PIGLET_MARKER_TOTAL_LEN, FIRMWARE_VERSION, vlen);
  text[PIGLET_MARKER_TOTAL_LEN_V2] = '\0';
  return PIGLET_MARKER_TOTAL_LEN_V2;
}

static void jcmkSendCoreRequest() {
  jcmk_text_msg_t msg = {};
  memcpy(msg.magic, JCMK_MAGIC, 4);
  msg.type    = JCMK_MSG_CORE_REQUEST;
  msg.counter = 0;
  // Marker + board-type byte + version identify this as a genuine Piglet
  // node (which board, which firmware) to a Piglet Core; real JCMK hardware
  // and Biscuit Cores simply ignore this payload.
  msg.len = fillPigletMarker(msg.text);
  esp_now_send(JCMK_BCAST, (uint8_t*)&msg, sizeof(msg));
}

static void jcmkSendHeartbeat() {
  jcmk_text_msg_t msg = {};
  memcpy(msg.magic, JCMK_MAGIC, 4);
  msg.type    = JCMK_MSG_HEARTBEAT;
  msg.counter = ++jcmkHbCounter;
  // Carry the marker here too so a Core can learn isPiglet/version even if
  // it first (re)discovers this node via a heartbeat rather than a fresh
  // CORE_REQUEST.
  msg.len = fillPigletMarker(msg.text);
  // Unicast only for a confirmed Piglet-to-Piglet pairing, where we control
  // peer registration on both ends and gain real link-layer retry/ACK.
  // Broadcast for everything else (real JCMK hardware / Biscuit): field
  // testing against real Biscuit hardware showed unicast frames getting a
  // 100% "no ACK" failure rate even though esp_now_send() itself always
  // accepted them -- i.e. the Biscuit's ESP-Now stack doesn't ack unicast
  // from a sender it hasn't itself registered as a peer. Broadcast frames
  // don't require the receiver to have the sender peered at all, which is
  // exactly why the discovery handshake (also broadcast) works fine.
  const uint8_t* dest = jcmkCoreIsPiglet ? jcmkCoreMac : JCMK_BCAST;
  esp_err_t err = esp_now_send(dest, (uint8_t*)&msg, sizeof(msg));
  if (err != ESP_OK) {
    uint8_t pri; wifi_second_chan_t sec; esp_wifi_get_channel(&pri, &sec);
    Serial.printf("[MESH] HEARTBEAT send() rejected: err=%d ch=%d dest=%02X:%02X:%02X:%02X:%02X:%02X\n",
      (int)err, pri, dest[0], dest[1], dest[2], dest[3], dest[4], dest[5]);
  }
}

static void jcmkSendText(const String& s) {
  jcmk_text_msg_t msg = {};
  memcpy(msg.magic, JCMK_MAGIC, 4);
  msg.type    = JCMK_MSG_TEXT;
  msg.counter = ++jcmkTextCounter;
  uint16_t slen = (uint16_t)((s.length() < JCMK_TEXT_MAX) ? s.length() : JCMK_TEXT_MAX);
  msg.len = slen;
  memcpy(msg.text, s.c_str(), slen);
  msg.text[slen] = '\0';
  // Always send full struct size — Biscuit Pro drops variable-length packets < 212 bytes.
  // Unicast only for confirmed Piglet-to-Piglet pairings (see jcmkSendHeartbeat
  // comment above for why broadcast is required for real JCMK/Biscuit hardware).
  const uint8_t* dest = jcmkCoreIsPiglet ? jcmkCoreMac : JCMK_BCAST;
  esp_err_t err = esp_now_send(dest, (uint8_t*)&msg, sizeof(msg));
  if (err != ESP_OK) {
    uint8_t pri; wifi_second_chan_t sec; esp_wifi_get_channel(&pri, &sec);
    Serial.printf("[MESH] TEXT send() rejected: err=%d ch=%d dest=%02X:%02X:%02X:%02X:%02X:%02X\n",
      (int)err, pri, dest[0], dest[1], dest[2], dest[3], dest[4], dest[5]);
  }
}

// Generic Biscuit protocol-control reply, always unicast to the Core. Used
// for the type=15 (admin ack) and type=11 (config ack) handshake messages a
// plain-reply Biscuit Core expects during node registration.
static void biscuitSendControl(uint8_t type, uint32_t counter = 0, const char* text = nullptr) {
  jcmk_text_msg_t msg = {};
  memcpy(msg.magic, JCMK_MAGIC, 4);
  msg.type = type;
  msg.counter = counter;
  if (text) {
    msg.len = strlen(text);
    memcpy(msg.text, text, msg.len);
  }
  esp_now_send(jcmkCoreMac, (uint8_t*)&msg, sizeof(msg));
}

// True if a received jcmk_text_msg_t carries the Piglet identification marker.
// `len` is the raw ESP-Now payload length (as delivered to the recv callback).
static bool jcmkHasPigletMarker(const void* data, int len) {
  if (len < (int)sizeof(jcmk_text_msg_t)) return false;
  const jcmk_text_msg_t* tm = (const jcmk_text_msg_t*)data;
  if (tm->len < PIGLET_MARKER_LEN) return false;
  return memcmp(tm->text, PIGLET_MARKER, PIGLET_MARKER_LEN) == 0;
}

// Returns the sender's declared PigletBoardType if the marker AND the
// board-type byte that follows it are both present, else PIGLET_BOARD_UNKNOWN
// (a pre-v3.0 Piglet node that only sends the bare 7-byte marker, or
// non-Piglet hardware -- jcmkHasPigletMarker() already gates isPiglet itself).
static uint8_t jcmkGetPigletBoardType(const void* data, int len) {
  if (len < (int)sizeof(jcmk_text_msg_t)) return PIGLET_BOARD_UNKNOWN;
  const jcmk_text_msg_t* tm = (const jcmk_text_msg_t*)data;
  if (tm->len < PIGLET_MARKER_TOTAL_LEN) return PIGLET_BOARD_UNKNOWN;
  if (memcmp(tm->text, PIGLET_MARKER, PIGLET_MARKER_LEN) != 0) return PIGLET_BOARD_UNKNOWN;
  return (uint8_t)tm->text[PIGLET_MARKER_LEN];
}

// Writes the sender's declared firmware version into `out` (size >= 9) if the
// marker AND the version bytes that follow the board-type byte are both
// present, else writes an empty string -- a pre-version Piglet node (or
// non-Piglet hardware) simply reports "unknown", which never triggers the
// on-device "Update Nodes through Core?" mismatch prompt.
static void jcmkGetPigletVersion(const void* data, int len, char* out) {
  out[0] = '\0';
  if (len < (int)sizeof(jcmk_text_msg_t)) return;
  const jcmk_text_msg_t* tm = (const jcmk_text_msg_t*)data;
  if (tm->len < PIGLET_MARKER_TOTAL_LEN_V2) return;
  if (memcmp(tm->text, PIGLET_MARKER, PIGLET_MARKER_LEN) != 0) return;
  memcpy(out, tm->text + PIGLET_MARKER_TOTAL_LEN, PIGLET_VERSION_LEN);
  out[PIGLET_VERSION_LEN] = '\0';
}

// True if a received CORE_REPLY indicates a Biscuit Core running in
// unencrypted-reply mode (carries "encrypt=0" somewhere in its text field).
static bool biscuitHasPlainReply(const void* data, int len) {
  if (len < (int)sizeof(jcmk_text_msg_t)) return false;
  const jcmk_text_msg_t* msg = (const jcmk_text_msg_t*)data;
  size_t n = msg->len < JCMK_TEXT_MAX ? msg->len : JCMK_TEXT_MAX;
  for (size_t i = 0; i + 9 <= n; i++) {
    if (memcmp(msg->text + i, "encrypt=0", 9) == 0) return true;
  }
  return false;
}

// Parses a Biscuit "channels=1,2,...;dwell=N" config message into
// biscuitChannels[]/biscuitChannelCount, restarting the scan cycle from the
// new list. Only used for plain-reply Biscuit pairings.
// Takes the raw ESP-Now payload (like jcmkHasPigletMarker/biscuitHasPlainReply
// above) rather than a jcmk_text_msg_t* parameter -- the Arduino .ino sketch
// preprocessor auto-generates function prototypes near the top of the file,
// before the jcmk_text_msg_t typedef is defined further down, which breaks
// compilation for any top-level function taking that type directly.
static void biscuitSetChannels(const void* data, int len) {
  if (len < (int)sizeof(jcmk_text_msg_t)) { biscuitChannelCount = 0; return; }
  const jcmk_text_msg_t* msg = (const jcmk_text_msg_t*)data;
  if (msg->len < 9 || msg->len > JCMK_TEXT_MAX || memcmp(msg->text, "channels=", 9)) {
    biscuitChannelCount = 0;
    return;
  }
  char text[JCMK_TEXT_MAX + 1];
  memcpy(text, msg->text, msg->len);
  text[msg->len] = '\0';
  char* end = strchr(text, ';');
  if (end) *end = '\0';
  uint8_t channels[JCMK_NUM_CHANNELS + 2];
  uint8_t count = 0;
  for (char* token = text + 9; *token && count < JCMK_NUM_CHANNELS + 2;) {
    char* next;
    long channel = strtol(token, &next, 10);
    if (next == token || channel < 1 || channel > 255 || (*next && *next != ',')) {
      count = 0;
      break;
    }
    if (channel == 104 || channel == 108) {
      channels[count++] = (uint8_t)channel;
    } else {
      for (uint8_t i = 0; i < JCMK_NUM_CHANNELS; i++) {
        if (JCMK_CHANNELS[i] == channel) { channels[count++] = (uint8_t)channel; break; }
      }
    }
    if (!*next) break;
    token = next + 1;
  }
  memcpy(biscuitChannels, channels, count);
  biscuitChannelCount = count;
  nodeScanChOffset = 0;
}

// ================================================================
//  Node-side OTA receiver (PIG_MSG_OTA_BEGIN / _DATA / _ACK)
//  Ported from olosnoks/warhog's WarHogNode otaTick()/otaStart()/otaFinish().
//  The recv callback (further down, jcmkOnRecv) only enqueues chunks; all
//  flash writes, hashing and acks happen here from nodeOtaTick(), called
//  every nodeModeTick(). Scanning is paused for the duration.
// ================================================================
#define NODE_OTA_QDEPTH 8
struct NodeOtaSlot { uint8_t len; uint8_t data[PIG_OTA_CHUNK]; };
static NodeOtaSlot        nodeOtaQ[NODE_OTA_QDEPTH];
static volatile uint8_t   nodeOtaQHead = 0, nodeOtaQTail = 0;   // callback produces, loop consumes
static volatile bool      nodeOtaActive       = false;
static volatile uint8_t   nodeOtaSession      = 0;
static volatile uint16_t  nodeOtaQNext        = 0;              // next seq the callback will accept
static volatile bool      nodeOtaBeginPending = false;
static volatile bool      nodeOtaReack        = false;          // stale/gap chunk seen -> re-send our ack
static pig_ota_begin_t    nodeOtaBeginMsg;
static uint16_t           nodeOtaWritten = 0, nodeOtaChunks = 0;
static uint32_t           nodeOtaSize = 0, nodeOtaBytes = 0, nodeOtaLastRxMs = 0;
static uint8_t            nodeOtaSha[32];
static mbedtls_sha256_context nodeOtaShaCtx;
static const uint32_t NODE_OTA_IDLE_ABORT_MS = 15000;  // no chunk for this long -> abandon

static void nodeOtaAck(uint8_t status) {
  pig_ota_ack_t a = {};
  memcpy(a.magic, JCMK_MAGIC, 4);
  a.type    = PIG_MSG_OTA_ACK;
  a.session = nodeOtaSession;
  a.next    = nodeOtaWritten;
  a.status  = status;
  esp_now_send(jcmkCoreMac, (uint8_t*)&a, sizeof(a));
}

static void nodeOtaFail(uint8_t status, const char* why) {
  Serial.printf("[OTA] FAILED: %s\n", why);
  Update.abort();
  mbedtls_sha256_free(&nodeOtaShaCtx);
  nodeOtaActive = false;
  nodeOtaAck(status);
}

static void nodeOtaStart() {
  nodeOtaBeginPending = false;
  const pig_ota_begin_t& b = nodeOtaBeginMsg;
  if (nodeOtaActive) {
    // Core re-sent BEGIN because our READY was lost: just repeat it.
    if (b.session == nodeOtaSession) { if (nodeOtaWritten == 0) nodeOtaAck(PIG_OTA_ST_READY); return; }
    Update.abort();
    mbedtls_sha256_free(&nodeOtaShaCtx);
    nodeOtaActive = false;
  }
  nodeOtaSession = b.session;
  nodeOtaWritten = 0;
  uint32_t size = b.size;
  if (size == 0 || ((size + PIG_OTA_CHUNK - 1) / PIG_OTA_CHUNK) != b.chunks) {
    nodeOtaAck(PIG_OTA_ST_ERR_ARGS);
    return;
  }
  // Stop any scan in flight and park on the home channel.
  WiFi.scanDelete();
  nodeScanActive   = false;
  nodeScanAdminWin = false;
  jcmkSetChannel(JCMK_ESPNOW_CH);

  if (!Update.begin(size, U_FLASH)) {
    Serial.printf("[OTA] Update.begin(%lu) failed: %s\n", (unsigned long)size, Update.errorString());
    nodeOtaAck(PIG_OTA_ST_ERR_SPACE);
    return;
  }
  mbedtls_sha256_init(&nodeOtaShaCtx);
  mbedtls_sha256_starts(&nodeOtaShaCtx, 0);
  memcpy(nodeOtaSha, b.sha256, 32);
  nodeOtaChunks   = b.chunks;
  nodeOtaSize     = size;
  nodeOtaBytes    = 0;
  nodeOtaQHead    = nodeOtaQTail = 0;
  nodeOtaQNext    = 0;
  nodeOtaReack    = false;
  nodeOtaLastRxMs = millis();
  nodeOtaActive   = true;
  Serial.printf("[OTA] Start: %lu bytes, %u chunks\n", (unsigned long)size, (unsigned)b.chunks);
  nodeOtaAck(PIG_OTA_ST_READY);
}

static void nodeOtaFinish() {
  uint8_t h[32];
  mbedtls_sha256_finish(&nodeOtaShaCtx, h);
  mbedtls_sha256_free(&nodeOtaShaCtx);
  if (nodeOtaBytes != nodeOtaSize) { nodeOtaFail(PIG_OTA_ST_ERR_ARGS, "size mismatch"); return; }
  if (memcmp(h, nodeOtaSha, 32) != 0) {
    Update.abort();
    nodeOtaActive = false;
    Serial.println("[OTA] FAILED: SHA-256 mismatch — update discarded");
    nodeOtaAck(PIG_OTA_ST_ERR_SHA);
    return;
  }
  if (!Update.end(true)) { nodeOtaFail(PIG_OTA_ST_ERR_WRITE, Update.errorString()); return; }
  nodeOtaActive = false;
  Serial.println("[OTA] Verified — rebooting into the new image");
  for (uint8_t i = 0; i < 3; i++) { nodeOtaAck(PIG_OTA_ST_DONE); delay(30); }
  delay(200);
  ESP.restart();
}

// Called every nodeModeTick(). No-op unless a transfer is pending/active.
static void nodeOtaTick() {
  if (nodeOtaBeginPending) nodeOtaStart();
  if (!nodeOtaActive) return;
  uint32_t now = millis();
  bool progressed = false;
  while (nodeOtaQTail != nodeOtaQHead) {
    NodeOtaSlot& sl = nodeOtaQ[nodeOtaQTail];
    if (Update.write(sl.data, sl.len) != sl.len) { nodeOtaFail(PIG_OTA_ST_ERR_WRITE, "flash write"); return; }
    mbedtls_sha256_update(&nodeOtaShaCtx, sl.data, sl.len);
    nodeOtaBytes += sl.len;
    nodeOtaWritten++;
    nodeOtaQTail = (nodeOtaQTail + 1) % NODE_OTA_QDEPTH;
    progressed = true;
  }
  if (progressed || nodeOtaReack) {
    nodeOtaReack = false;
    nodeOtaAck(PIG_OTA_ST_OK);
  }
  if (progressed) {
    nodeOtaLastRxMs = now;
    jcmkCoreLastSeenMs = now;   // we are talking to the Core: don't time out
  }
  if (nodeOtaWritten == nodeOtaChunks) { nodeOtaFinish(); return; }
  if (now - nodeOtaLastRxMs > NODE_OTA_IDLE_ABORT_MS) {
    Serial.println("[OTA] Timed out waiting for chunks — abandoning update");
    Update.abort();
    mbedtls_sha256_free(&nodeOtaShaCtx);
    nodeOtaActive = false;      // scanning resumes; Core can retry
  }
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
  // Marker + board-type byte + version tell a Piglet node that this Core is
  // also a genuine Piglet (and its firmware version), so it can safely
  // switch to slot-scheduled transmit; ignored by third-party nodes.
  msg.len = fillPigletMarker(msg.text);
  esp_now_send(mac, (uint8_t*)&msg, sizeof(msg));
}

// Maps a 5GHz channel number to its index in core5gStat[]/core5gHits[]
// (JCMK_CHANNELS[14..39]), or -1 if `ch` isn't a known 5GHz channel.
static int core5gIndexOf(uint8_t ch) {
  for (uint8_t i = 14; i < JCMK_NUM_CHANNELS; i++)
    if (JCMK_CHANNELS[i] == ch) return (int)(i - 14);
  return -1;
}

// Folds accumulated per-channel hit counts into the yield EWMA once per 4s
// window. Called every coreModeTick(); a no-op between windows.
static void coreYieldUpdate(uint32_t now) {
  if (coreYieldLastMs == 0) { coreYieldLastMs = now; return; }
  if (now - coreYieldLastMs < 4000) return;
  float dt = (float)(now - coreYieldLastMs) / 1000.0f;
  for (uint8_t i = 0; i < CORE_5G_COUNT; i++) {
    uint8_t h = core5gHits[i] > 255 ? 255 : (uint8_t)core5gHits[i];
    sched_on_yield(core5gStat, i, h, h, dt, coreSchedCfg);
    core5gHits[i] = 0;
  }
  coreYieldLastMs = now;
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

  // Step 2: channel config — "channels=1,2,...;dwell=80". Biscuit's wire
  // format allows the list in any order; the SET of channels in [startIdx,
  // endIdx] is exactly what today's plain contiguous split assigned this
  // node -- unchanged -- but the 5GHz ones among them are reordered by
  // descending measured yield (core5gStat / YieldSched.h) so the most
  // productive channels this node has access to are listed first. 2.4GHz
  // channels (always worth scanning) are kept in natural order first.
  uint8_t end = (endIdx < JCMK_NUM_CHANNELS) ? endIdx : (JCMK_NUM_CHANNELS - 1);
  uint8_t lo[JCMK_NUM_CHANNELS]; uint8_t loN = 0;
  uint8_t hi[JCMK_NUM_CHANNELS]; uint8_t hiN = 0;
  ChanStat hiStat[JCMK_NUM_CHANNELS];
  for (uint8_t j = startIdx; j <= end; j++) {
    uint8_t ch = JCMK_CHANNELS[j];
    if (ch <= 14) {
      lo[loN++] = ch;
    } else {
      int gi = core5gIndexOf(ch);
      hi[hiN] = ch;
      hiStat[hiN] = (gi >= 0) ? core5gStat[gi] : ChanStat{};
      hiN++;
    }
  }
  uint8_t order[JCMK_NUM_CHANNELS];
  if (hiN > 0) sched_rank(hiStat, hi, hiN, millis(), coreSchedCfg, order);

  jcmk_text_msg_t cfgMsg = {};
  memcpy(cfgMsg.magic, JCMK_MAGIC, 4);
  cfgMsg.type    = 10;  // MSG_CONFIG_UPDATE
  cfgMsg.counter = 0;

  String chList = "channels=";
  bool first = true;
  for (uint8_t j = 0; j < loN; j++) {
    if (!first) chList += ",";
    chList += String(lo[j]);
    first = false;
  }
  for (uint8_t j = 0; j < hiN; j++) {
    if (!first) chList += ",";
    chList += String(hi[order[j]]);
    first = false;
  }
  chList += ";dwell=";
  chList += String((uint32_t)NODE_SCAN_DWELL_MS);

  uint16_t slen = (chList.length() < JCMK_TEXT_MAX)
                ? (uint16_t)chList.length() : (uint16_t)JCMK_TEXT_MAX;
  cfgMsg.len = slen;
  memcpy(cfgMsg.text, chList.c_str(), slen);
  cfgMsg.text[slen] = '\0';
  esp_now_send(mac, (uint8_t*)&cfgMsg, sizeof(cfgMsg));

  Serial.printf("[CORE] Biscuit role+config sent (ch %d-%d, dwell %dms, yield-ranked)\n",
    JCMK_CHANNELS[startIdx],
    JCMK_CHANNELS[end],
    (int)NODE_SCAN_DWELL_MS);
}

// ================================================================
//  Core-side OTA engine (PIG_MSG_OTA_BEGIN / _DATA / _ACK)
//  Ported from olosnoks/warhog's coreOta* functions. Pushes a connected
//  node's app image over the existing ESP-NOW link -- no USB. Images are
//  staged once per boot from the Core's own SD card (/Firmware/<Board>.ota.bin,
//  one per PigletBoardType) into PSRAM, since mid-session SD reads are not
//  reliable once Wi-Fi/BLE are both running. coreOtaStart() just arms a
//  transfer; coreOtaTick() (called from coreModeTick()) drives it.
// ================================================================
static const uint32_t CORE_OTA_BEGIN_RETRY_MS   = 400;      // resend OTA_BEGIN this often
static const uint32_t CORE_OTA_START_TIMEOUT_MS = 20000;    // node never answered
static const uint32_t CORE_OTA_ACK_TIMEOUT_MS   = 250;      // no ack progress -> resend window
static const uint32_t CORE_OTA_STALL_MS         = 10000;    // no ack progress at all -> give up
static const uint32_t CORE_OTA_RESULT_SHOW_MS   = 15000;    // keep Done/Failed visible this long
static const uint32_t CORE_OTA_MAX_IMAGE        = 0x330000; // size of an ota_0/ota_1 slot in default_8MB
static const uint16_t CORE_OTA_CHIP_ESP32C5     = 23;       // esp_image_header_t.chip_id for the C5

static CoreOtaImage coreOtaImages[] = {
  { PIGLET_BOARD_NODE,    "/Firmware/PigletNode.ota.bin", "Node", nullptr, 0, 0, {0}, {0} },
  { PIGLET_BOARD_XIAO,    "/Firmware/Piglet.ota.bin",     "XIAO", nullptr, 0, 0, {0}, {0} },
  { PIGLET_BOARD_TDONGLE, "/Firmware/TDongleC5_Piglet.ota.bin", "TDongle", nullptr, 0, 0, {0}, {0} },
};
static const int CORE_OTA_IMAGE_COUNT = (int)(sizeof(coreOtaImages) / sizeof(coreOtaImages[0]));

static struct {
  CoreOtaPhase phase   = CoreOtaPhase::Idle;
  int8_t       node    = -1;
  uint8_t      mac[6]  = {0};
  uint8_t      session = 0;
  uint8_t*     image   = nullptr;   // whole node image, in PSRAM (null = none loaded)
  uint32_t     size    = 0;
  uint16_t     chunks  = 0;
  uint8_t      sha[32] = {0};
  char         loadMsg[28] = {0};
  uint16_t     base    = 0;         // chunks the node has acknowledged
  uint16_t     nextSend = 0;
  uint32_t     startMs = 0, lastBeginMs = 0, ackMs = 0, progressMs = 0, endMs = 0;
  char         msg[28] = {0};
} coreOta;

// Written by the ESP-NOW callback, consumed by coreOtaTick().
static volatile bool     coreOtaGotReady  = false;
static volatile uint16_t coreOtaAckNext   = 0;
static volatile uint8_t  coreOtaFinalStat = 0xFF;   // PIG_OTA_ST_DONE / PIG_OTA_ST_ERR_*; 0xFF = none yet

static bool coreOtaBusy() {
  return coreOta.phase == CoreOtaPhase::Starting || coreOta.phase == CoreOtaPhase::Sending;
}

// True if a loaded (PSRAM-resident) image is available for pushing to a node
// of the given board type. Used by the update-prompt/auto-push logic below
// to avoid offering/attempting a push with nothing to actually send.
static bool coreOtaHasImage(uint8_t boardType) {
  for (int i = 0; i < CORE_OTA_IMAGE_COUNT; i++)
    if (coreOtaImages[i].boardType == boardType) return coreOtaImages[i].image != nullptr;
  return false;
}

static void coreOtaSetMsg(const char* m) { snprintf(coreOta.msg, sizeof(coreOta.msg), "%s", m); }
static void coreOtaLoadFail(const char* m) {
  snprintf(coreOta.loadMsg, sizeof(coreOta.loadMsg), "%s", m);
  Serial.printf("[OTA] no image loaded: %s\n", m);
}

// Load one board's image file from SD into PSRAM, validating its ESP image
// magic + chip_id. A missing file is not an error here (that board may simply
// not be part of this mesh) -- it only matters once coreOtaStart() wants it.
static void coreOtaLoadOne(CoreOtaImage& im) {
  if (im.image) { heap_caps_free(im.image); im.image = nullptr; }
  im.size = 0; im.chunks = 0; im.failMsg[0] = 0;
  if (!SD.exists(im.path)) { snprintf(im.failMsg, sizeof(im.failMsg), "NO IMAGE ON SD"); return; }
  File f = SD.open(im.path, FILE_READ);
  if (!f) { snprintf(im.failMsg, sizeof(im.failMsg), "CANNOT OPEN IMAGE"); return; }
  uint32_t size = (uint32_t)f.size();
  if (size < 1024 || size > CORE_OTA_MAX_IMAGE) {
    f.close(); snprintf(im.failMsg, sizeof(im.failMsg), "BAD IMAGE SIZE"); return;
  }
  uint8_t* img = (uint8_t*)heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!img) { f.close(); snprintf(im.failMsg, sizeof(im.failMsg), "OUT OF MEMORY"); return; }
  uint32_t got = 0;
  uint8_t buf[4096];
  while (got < size) {
    uint32_t want = size - got; if (want > sizeof(buf)) want = (uint32_t)sizeof(buf);
    int n = f.read(buf, want);
    if (n <= 0) break;
    memcpy(img + got, buf, (size_t)n);
    got += (uint32_t)n;
  }
  f.close();
  if (got != size) { heap_caps_free(img); snprintf(im.failMsg, sizeof(im.failMsg), "SD READ ERROR"); return; }
  if (img[0] != 0xE9) { heap_caps_free(img); snprintf(im.failMsg, sizeof(im.failMsg), "NOT AN APP IMAGE"); return; }
  uint16_t chip = (uint16_t)(img[12] | (img[13] << 8));
  if (chip != CORE_OTA_CHIP_ESP32C5) {
    heap_caps_free(img); snprintf(im.failMsg, sizeof(im.failMsg), "NOT A C5 IMAGE"); return;
  }
  mbedtls_sha256(img, size, im.sha, 0);
  im.image  = img;
  im.size   = size;
  im.chunks = (uint16_t)((size + PIG_OTA_CHUNK - 1) / PIG_OTA_CHUNK);
  Serial.printf("[OTA] %s image loaded: %lu bytes, %u chunks\n", im.label, (unsigned long)size, (unsigned)im.chunks);
}

// Call once from setup(), right after the SD card mounts. Loads every board's
// image present on SD and leaves a short per-board summary for the Web UI.
static void coreOtaPreload() {
  coreOta.loadMsg[0] = 0;
  if (!sdOk) { coreOtaLoadFail("NO SD CARD"); return; }
  if (!SD.exists("/Firmware")) SD.mkdir("/Firmware");
  int loaded = 0;
  String summary;
  for (int i = 0; i < CORE_OTA_IMAGE_COUNT; i++) {
    coreOtaLoadOne(coreOtaImages[i]);
    if (coreOtaImages[i].image) loaded++;
    if (summary.length()) summary += " ";
    summary += coreOtaImages[i].label;
    summary += ":";
    summary += coreOtaImages[i].image ? "ok" : "-";
  }
  if (loaded == 0) coreOtaLoadFail("NO FIRMWARE IMAGES ON SD");
  else snprintf(coreOta.loadMsg, sizeof(coreOta.loadMsg), "%s", summary.c_str());
}

static void coreOtaClose(CoreOtaPhase endPhase, const char* msg) {
  coreOta.phase = endPhase;
  coreOta.endMs = millis();
  coreOtaSetMsg(msg);
}

// ESP-NOW callback context: record only, no sends.
static void coreOtaOnAck(const uint8_t* mac, const uint8_t* data, int len) {
  if (len < (int)sizeof(pig_ota_ack_t)) return;
  if (coreOta.phase != CoreOtaPhase::Starting && coreOta.phase != CoreOtaPhase::Sending) return;
  if (memcmp(mac, coreOta.mac, 6) != 0) return;
  const pig_ota_ack_t* a = (const pig_ota_ack_t*)data;
  if (a->session != coreOta.session) return;
  if (coreOta.node >= 0) coreNodes[coreOta.node].lastHbMs = millis();   // an ack proves the node is alive
  if (a->status == PIG_OTA_ST_READY) coreOtaGotReady = true;
  else if (a->status >= PIG_OTA_ST_DONE) coreOtaFinalStat = a->status;
  if (a->next > coreOtaAckNext) coreOtaAckNext = a->next;
}

static bool coreOtaStart(uint8_t nodeIdx) {
  if (coreOtaBusy()) return false;
  coreOta.node  = (int8_t)nodeIdx;
  coreOta.phase = CoreOtaPhase::Failed;      // until proven otherwise; keeps msg visible
  coreOta.endMs = millis();
  if (!meshCoreActive || nodeIdx >= CORE_MAX_NODES || !coreNodes[nodeIdx].active) {
    coreOtaSetMsg("NODE NOT CONNECTED"); return false;
  }
  uint8_t bt = coreNodes[nodeIdx].boardType;
  if (bt == PIGLET_BOARD_UNKNOWN) { coreOtaSetMsg("BOARD TYPE UNKNOWN"); return false; }
  CoreOtaImage* im = nullptr;
  for (int i = 0; i < CORE_OTA_IMAGE_COUNT; i++)
    if (coreOtaImages[i].boardType == bt) { im = &coreOtaImages[i]; break; }
  if (!im || !im->image) {
    coreOtaSetMsg(im && im->failMsg[0] ? im->failMsg : "NO IMAGE FOR NODE");
    return false;
  }
  coreOta.image  = im->image;
  coreOta.size   = im->size;
  coreOta.chunks = im->chunks;
  memcpy(coreOta.sha, im->sha, 32);

  memcpy(coreOta.mac, coreNodes[nodeIdx].mac, 6);
  coreOta.session  = (uint8_t)(esp_random() & 0xFF);
  coreOta.base     = coreOta.nextSend = 0;
  coreOtaGotReady  = false;
  coreOtaAckNext   = 0;
  coreOtaFinalStat = 0xFF;
  coreOtaSetMsg("");
  coreOta.startMs     = millis();
  coreOta.lastBeginMs = 0;
  coreOta.phase       = CoreOtaPhase::Starting;
  Serial.printf("[OTA] node %u: %lu bytes, %u chunks, session %u\n", (unsigned)nodeIdx,
                (unsigned long)coreOta.size, (unsigned)coreOta.chunks, (unsigned)coreOta.session);
  return true;
}

static void coreOtaAbort() {
  if (!coreOtaBusy()) return;
  coreOtaClose(CoreOtaPhase::Failed, "CANCELLED");   // the node abandons an idle update after 15 s
}

static CoreOtaStatus coreOtaStatus() {
  CoreOtaStatus st = {};
  st.phase      = coreOta.phase;
  st.nodeIdx    = coreOta.node;
  st.totalBytes = coreOta.size;
  uint32_t b    = (uint32_t)coreOta.base * PIG_OTA_CHUNK;
  st.sentBytes  = b > coreOta.size ? coreOta.size : b;
  snprintf(st.msg, sizeof(st.msg), "%s", coreOta.msg);
  snprintf(st.loadMsg, sizeof(st.loadMsg), "%s", coreOta.loadMsg);
  return st;
}

static bool coreOtaSendBegin() {
  pig_ota_begin_t b = {};
  memcpy(b.magic, JCMK_MAGIC, 4);
  b.type    = PIG_MSG_OTA_BEGIN;
  b.session = coreOta.session;
  b.size    = coreOta.size;
  b.chunks  = coreOta.chunks;
  memcpy(b.sha256, coreOta.sha, 32);
  return esp_now_send(coreOta.mac, (uint8_t*)&b, sizeof(b)) == ESP_OK;
}

// Returns false if the chunk could not be sent right now (retry next tick).
static bool coreOtaSendChunk(uint16_t seq) {
  pig_ota_data_t d;
  memcpy(d.magic, JCMK_MAGIC, 4);
  d.type    = PIG_MSG_OTA_DATA;
  d.session = coreOta.session;
  d.seq     = seq;
  uint32_t off = (uint32_t)seq * PIG_OTA_CHUNK;
  uint32_t n   = coreOta.size - off;
  if (n > PIG_OTA_CHUNK) n = PIG_OTA_CHUNK;
  memcpy(d.data, coreOta.image + off, n);
  d.len = (uint8_t)n;
  return esp_now_send(coreOta.mac, (uint8_t*)&d, offsetof(pig_ota_data_t, data) + n) == ESP_OK;
}

static const char* coreOtaErrText(uint8_t st) {
  switch (st) {
    case PIG_OTA_ST_ERR_SPACE: return "NODE HAS NO OTA SLOT";   // old flash layout: needs one reflash
    case PIG_OTA_ST_ERR_WRITE: return "NODE FLASH WRITE ERR";
    case PIG_OTA_ST_ERR_SHA:   return "NODE: SHA MISMATCH";
    default:                   return "NODE REJECTED UPDATE";
  }
}

// Called every coreModeTick(). No-op unless a transfer is pending/active.
static void coreOtaTick() {
  uint32_t now = millis();
  if (!coreOtaBusy()) {
    if ((coreOta.phase == CoreOtaPhase::Done || coreOta.phase == CoreOtaPhase::Failed) &&
        now - coreOta.endMs > CORE_OTA_RESULT_SHOW_MS) {
      coreOta.phase = CoreOtaPhase::Idle;
      coreOta.node  = -1;
    }
    return;
  }
  if (coreOta.node < 0 || !coreNodes[coreOta.node].active) {
    coreOtaClose(CoreOtaPhase::Failed, "NODE LOST");
    return;
  }

  switch (coreOta.phase) {
    case CoreOtaPhase::Starting:
      if (coreOtaFinalStat != 0xFF && coreOtaFinalStat != PIG_OTA_ST_DONE) {
        coreOtaClose(CoreOtaPhase::Failed, coreOtaErrText(coreOtaFinalStat));
      } else if (coreOtaGotReady) {
        coreOta.phase = CoreOtaPhase::Sending;
        coreOta.base = coreOta.nextSend = 0;
        coreOta.ackMs = coreOta.progressMs = now;
        Serial.println("[OTA] node ready — sending image");
      } else if (now - coreOta.startMs > CORE_OTA_START_TIMEOUT_MS) {
        coreOtaClose(CoreOtaPhase::Failed, "NODE DID NOT REPLY");
      } else if (coreOta.lastBeginMs == 0 || now - coreOta.lastBeginMs >= CORE_OTA_BEGIN_RETRY_MS) {
        coreOta.lastBeginMs = now;
        coreOtaSendBegin();
      }
      break;

    case CoreOtaPhase::Sending: {
      uint8_t fin = coreOtaFinalStat;
      if (fin == PIG_OTA_ST_DONE) { coreOta.base = coreOta.chunks; coreOtaClose(CoreOtaPhase::Done, "UPDATED - REBOOTING"); return; }
      if (fin != 0xFF)            { coreOtaClose(CoreOtaPhase::Failed, coreOtaErrText(fin)); return; }

      uint16_t acked = coreOtaAckNext;
      if (acked > coreOta.chunks) acked = coreOta.chunks;
      if (acked > coreOta.base) { coreOta.base = acked; coreOta.ackMs = coreOta.progressMs = now; }
      if (coreOta.nextSend < coreOta.base) coreOta.nextSend = coreOta.base;

      if (now - coreOta.progressMs > CORE_OTA_STALL_MS) { coreOtaClose(CoreOtaPhase::Failed, "TRANSFER STALLED"); return; }
      // Lost frame or ack: go back to the first unacknowledged chunk.
      if (coreOta.nextSend > coreOta.base && now - coreOta.ackMs > CORE_OTA_ACK_TIMEOUT_MS) {
        coreOta.nextSend = coreOta.base;
        coreOta.ackMs    = now;
      }
      while (coreOta.nextSend < coreOta.chunks && coreOta.nextSend < (uint16_t)(coreOta.base + PIG_OTA_WINDOW)) {
        if (!coreOtaSendChunk(coreOta.nextSend)) break;   // busy radio -> retry next tick
        coreOta.nextSend++;
      }
      break;
    }
    default: break;
  }
}

// ================================================================
//  On-device "Update Nodes through Core?" prompt (XIAO nodes) and
//  automatic push to out-of-date headless PigletNode nodes
//  (cfg.autoUpdateHeadlessNodes). Both share the single-transfer-at-a-time
//  coreOta engine above; neither ever starts a push while coreOtaBusy().
//  Rendered by drawPageMeshNode() below; the mesh-page button handler in
//  pollButton() calls coreUpdatePromptConfirm() when a press should be
//  treated as "confirm" (gated on coreUpdatePromptStatus().promptActive).
// ================================================================
static const uint32_t CORE_UPDATE_PROMPT_MS = 30000;  // confirm window
static uint8_t  pendingUpdateNodes[CORE_MAX_NODES];
static uint8_t  pendingUpdateCount  = 0;
static bool     updatePromptActive  = false;
static uint32_t updatePromptStartMs = 0;
static int8_t   updateExecutingPos  = -1;  // index into pendingUpdateNodes[], -1 = not executing

static bool coreUpdateAlreadyQueued(uint8_t nodeIdx) {
  for (uint8_t i = 0; i < pendingUpdateCount; i++) if (pendingUpdateNodes[i] == nodeIdx) return true;
  return false;
}

// Scans the roster for newly-mismatched XIAO nodes (queued for the on-screen
// prompt) and headless PigletNode auto-push candidates (pushed immediately,
// no prompt -- PigletNode has no UI of its own). Called every coreModeTick().
static void coreUpdateDetect() {
  for (uint8_t i = 0; i < CORE_MAX_NODES; i++) {
    CoreNodeInfo& n = coreNodes[i];
    if (!n.active || !n.isPiglet || n.version[0] == '\0') continue;  // unknown version -> never flagged
    if (strncmp(n.version, FIRMWARE_VERSION, sizeof(n.version)) == 0) continue;  // matches Core -- fine

    if (n.boardType == PIGLET_BOARD_XIAO) {
      if (n.updateDeclined || coreUpdateAlreadyQueued(i)) continue;
      if (!coreOtaHasImage(PIGLET_BOARD_XIAO)) continue;
      if (pendingUpdateCount < CORE_MAX_NODES) pendingUpdateNodes[pendingUpdateCount++] = i;
    } else if (n.boardType == PIGLET_BOARD_NODE && cfg.autoUpdateHeadlessNodes) {
      if (n.autoUpdateAttempted || coreOtaBusy()) continue;
      if (!coreOtaHasImage(PIGLET_BOARD_NODE)) continue;
      n.autoUpdateAttempted = true;  // set before starting -- never retry-loop a failing node
      Serial.printf("[CORE] Auto-updating headless node %d (%s -> %s)\n", i, n.version, FIRMWARE_VERSION);
      coreOtaStart(i);
    }
  }
}

// Drives the 30 s confirm window and, once confirmed, steps the queue one
// node at a time through the shared coreOta engine. Called every
// coreModeTick(), after coreOtaTick() so a just-finished push is reflected
// immediately. A no-op whenever there's nothing pending/executing.
static void coreUpdateProcessTick() {
  uint32_t now = millis();

  if (updateExecutingPos >= 0) {
    if (coreOtaBusy()) return;  // still sending (or just started) -- wait
    updateExecutingPos++;
    if (updateExecutingPos >= (int)pendingUpdateCount) {
      pendingUpdateCount = 0;
      updateExecutingPos = -1;
    } else {
      coreOtaStart(pendingUpdateNodes[updateExecutingPos]);
    }
    return;
  }

  if (updatePromptActive) {
    if (now - updatePromptStartMs >= CORE_UPDATE_PROMPT_MS) {
      for (uint8_t i = 0; i < pendingUpdateCount; i++) {
        uint8_t idx = pendingUpdateNodes[i];
        if (idx < CORE_MAX_NODES) coreNodes[idx].updateDeclined = true;
      }
      pendingUpdateCount = 0;
      updatePromptActive = false;
    }
    return;
  }

  if (pendingUpdateCount > 0 && !coreOtaBusy()) {
    updatePromptActive  = true;
    updatePromptStartMs = now;
  }
}

static CoreUpdatePromptStatus coreUpdatePromptStatus() {
  CoreUpdatePromptStatus st = {};
  st.promptActive = updatePromptActive;
  st.promptRemainingMs = updatePromptActive
    ? (uint32_t)(CORE_UPDATE_PROMPT_MS - (millis() - updatePromptStartMs)) : 0;
  st.pendingCount = pendingUpdateCount;
  st.pushActive   = coreOtaBusy();
  CoreOtaStatus ota = coreOtaStatus();
  st.pushNodeIdx  = (ota.nodeIdx >= 0) ? (uint8_t)ota.nodeIdx : 0;  // valid only when pushActive
  return st;
}

// Called from the mesh-page button handler when a press lands while
// coreUpdatePromptStatus().promptActive is true.
static void coreUpdatePromptConfirm() {
  if (!updatePromptActive || pendingUpdateCount == 0) return;
  updatePromptActive = false;
  updateExecutingPos = 0;
  coreOtaStart(pendingUpdateNodes[0]);
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
        coreReqBuf[coreReqTail].isPiglet  = jcmkHasPigletMarker(data, len);
        coreReqBuf[coreReqTail].boardType = jcmkGetPigletBoardType(data, len);
        jcmkGetPigletVersion(data, len, coreReqBuf[coreReqTail].version);
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
            // TEXT carries scan-line data, not the marker -- conservatively
            // treat as non-Piglet until the node's next heartbeat.
            coreReqBuf[coreReqTail].isPiglet  = false;
            coreReqBuf[coreReqTail].boardType = PIGLET_BOARD_UNKNOWN;
            coreReqBuf[coreReqTail].version[0] = '\0';
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
          // Refresh version directly here (ISR-safe memcpy, no flash/SD
          // access) -- a node that was just pushed a firmware update and
          // rebooted needs this to clear the mismatch on its very next
          // heartbeat, not just at (re)registration time.
          jcmkGetPigletVersion(data, len, coreNodes[i].version);
          found = true; break;
        }
      }
      if (!found) {
        uint8_t nxt = (coreReqTail + 1) % CORE_REQ_QUEUE;
        if (nxt != coreReqHead) {
          memcpy(coreReqBuf[coreReqTail].mac, info->src_addr, 6);
          coreReqBuf[coreReqTail].isBiscuit = (len >= (int)sizeof(jcmk_text_msg_t));
          coreReqBuf[coreReqTail].isPiglet  = jcmkHasPigletMarker(data, len);
          coreReqBuf[coreReqTail].boardType = jcmkGetPigletBoardType(data, len);
          jcmkGetPigletVersion(data, len, coreReqBuf[coreReqTail].version);
          coreReqTail = nxt;
        }
      }
    } else if (type == PIG_MSG_OTA_ACK) {
      coreOtaOnAck(info->src_addr, data, len);
    }
  } else {
    if (type == JCMK_MSG_CORE_REPLY && !jcmkHaveCore && !jcmkCoreFoundPending) {
      memcpy(jcmkCoreMacPending, info->src_addr, 6);
      jcmkCoreIsPigletPending = jcmkHasPigletMarker(data, len);
      biscuitPlainReplyPending = biscuitHasPlainReply(data, len);
      jcmkCoreFoundPending = true;
    } else if (type == JCMK_MSG_HEARTBEAT && jcmkHaveCore
               && memcmp(info->src_addr, jcmkCoreMac, 6) == 0) {
      // Core's periodic broadcast: liveness signal for timeout detection, and
      // (for Piglet pairings) the shared timing anchor for slot scheduling.
      jcmkCoreLastSeenMs = millis();
      if (jcmkCoreIsPiglet) jcmkCycleEpochMs = millis();
    } else if (jcmkHaveCore && memcmp(info->src_addr, jcmkCoreMac, 6) == 0
               && len >= (int)sizeof(jcmk_text_msg_t) && type == JCMK_MSG_ADMIN) {
      // Plain-reply Biscuit admin handshake step: ack with type=15, echoing
      // back the first text byte (or low byte of counter as a fallback).
      const jcmk_text_msg_t* msg = (const jcmk_text_msg_t*)data;
      jcmkCoreLastSeenMs = millis();
      biscuitSendControl(15, msg->len ? (uint8_t)msg->text[0] : msg->counter & 0xFF);
    } else if (jcmkHaveCore && memcmp(info->src_addr, jcmkCoreMac, 6) == 0
               && len >= (int)sizeof(jcmk_text_msg_t) && type == 10) {
      // Plain-reply Biscuit channel-config message (MSG_CONFIG_UPDATE) --
      // parse the assigned channel list, then ack with type=11.
      jcmkCoreLastSeenMs = millis();
      if (biscuitPlainReplyPending) biscuitSetChannels(data, len);
      biscuitSendControl(11);
    } else if (type == JCMK_MSG_ADMIN && len >= (int)sizeof(jcmk_admin_msg_t)) {
      jcmkCoreLastSeenMs = millis();
      if (jcmkCoreIsPiglet) jcmkCycleEpochMs = millis();
      const jcmk_admin_msg_t* adm = (const jcmk_admin_msg_t*)data;
      if (adm->assignment_version != jcmkAssignVer) {
        jcmkAssignVer = adm->assignment_version;
        jcmkStartIdx  = adm->start_channel_idx;
        jcmkEndIdx    = adm->end_channel_idx;
      }
      // Transmit-slot fields are only meaningful when paired with a confirmed
      // Piglet Core; refreshed every ADMIN regardless of assignment_version.
      if (jcmkCoreIsPiglet) {
        jcmkSlotIndex = adm->node_index;
        jcmkSlotCount = (adm->node_count > 0) ? adm->node_count : 1;
      }
    } else if (type == PIG_MSG_OTA_BEGIN && jcmkHaveCore && memcmp(info->src_addr, jcmkCoreMac, 6) == 0
               && len >= (int)sizeof(pig_ota_begin_t)) {
      // ---- Core wants to update us. Queue it for the loop; never touch flash from here.
      if (!nodeOtaBeginPending) { memcpy(&nodeOtaBeginMsg, data, sizeof(nodeOtaBeginMsg)); nodeOtaBeginPending = true; }
    } else if (type == PIG_MSG_OTA_DATA && nodeOtaActive && jcmkHaveCore
               && memcmp(info->src_addr, jcmkCoreMac, 6) == 0
               && len > (int)offsetof(pig_ota_data_t, data)) {
      const pig_ota_data_t* d = (const pig_ota_data_t*)data;
      if (d->session == nodeOtaSession && d->len >= 1 && d->len <= PIG_OTA_CHUNK
          && len >= (int)offsetof(pig_ota_data_t, data) + (int)d->len) {
        if (d->seq == nodeOtaQNext) {
          uint8_t nh = (uint8_t)((nodeOtaQHead + 1) % NODE_OTA_QDEPTH);
          if (nh != nodeOtaQTail) {
            nodeOtaQ[nodeOtaQHead].len = d->len;
            memcpy(nodeOtaQ[nodeOtaQHead].data, d->data, d->len);
            nodeOtaQHead = nh;
            nodeOtaQNext++;
          }
        } else {
          nodeOtaReack = true;   // stale or out-of-order: tell the Core where we are
        }
      }
    }
  }
}

// Forward declaration -- defined further down this file; meshChOverrideSet()/
// meshChOverrideClear() below need to re-apply channel assignments immediately.
static void coreReassignChannels();

// ================================================================
//  Manual per-node channel-range override (Web UI)
//  Lets an operator pin a specific node (matched by MAC) to a fixed channel
//  range instead of the automatic even split in coreReassignChannels() --
//  e.g. a node with a directional antenna aimed at a specific band. Persisted
//  to /mesh_channel_overrides.cfg so it survives a reboot; loaded once in
//  enterCoreMode(). chStart/chEnd are actual channel numbers (e.g. 1, 149),
//  not JCMK_CHANNELS[] indices -- both must exactly match an entry in
//  JCMK_CHANNELS[] or the call fails.
// ================================================================
#define MESH_CHOVR_MAX CORE_MAX_NODES
struct MeshChanOverride { uint8_t mac[6]; uint8_t startIdx; uint8_t endIdx; };
static MeshChanOverride meshChOverrides[MESH_CHOVR_MAX];
static uint8_t          meshChOverrideCount = 0;
static const char*      MESH_CHOVR_PATH = "/mesh_channel_overrides.cfg";

static bool meshChannelToIndex(int channel, uint8_t& idxOut) {
  for (uint8_t i = 0; i < JCMK_NUM_CHANNELS; i++) {
    if (JCMK_CHANNELS[i] == channel) { idxOut = i; return true; }
  }
  return false;
}

static bool meshMacParse(const String& s, uint8_t* macOut) {
  unsigned int v[6];
  if (sscanf(s.c_str(), "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) return false;
  for (int i = 0; i < 6; i++) macOut[i] = (uint8_t)v[i];
  return true;
}

static void meshChOverrideSave() {
  if (!sdOk) return;
  digitalWrite(PINS.tft_cs, HIGH);
  if (SD.exists(MESH_CHOVR_PATH)) SD.remove(MESH_CHOVR_PATH);
  File f = SD.open(MESH_CHOVR_PATH, FILE_WRITE);
  if (!f) return;
  f.println("# Manual mesh channel overrides: MAC,startChannel,endChannel");
  for (uint8_t i = 0; i < meshChOverrideCount; i++) {
    MeshChanOverride& o = meshChOverrides[i];
    char line[40];
    snprintf(line, sizeof(line), "%02X:%02X:%02X:%02X:%02X:%02X,%d,%d",
      o.mac[0], o.mac[1], o.mac[2], o.mac[3], o.mac[4], o.mac[5],
      JCMK_CHANNELS[o.startIdx], JCMK_CHANNELS[o.endIdx]);
    f.println(line);
  }
  f.close();
}

// Call once from enterCoreMode() -- populates the in-memory table from SD.
static void meshChOverrideLoad() {
  meshChOverrideCount = 0;
  if (!sdOk) return;
  digitalWrite(PINS.tft_cs, HIGH);
  if (!SD.exists(MESH_CHOVR_PATH)) return;
  File f = SD.open(MESH_CHOVR_PATH, FILE_READ);
  if (!f) return;
  while (f.available() && meshChOverrideCount < MESH_CHOVR_MAX) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() == 0 || line[0] == '#') continue;
    int c1 = line.indexOf(',');
    int c2 = (c1 >= 0) ? line.indexOf(',', c1 + 1) : -1;
    if (c1 < 0 || c2 < 0) continue;
    uint8_t mac[6];
    if (!meshMacParse(line.substring(0, c1), mac)) continue;
    uint8_t si, ei;
    int chStart = line.substring(c1 + 1, c2).toInt();
    int chEnd   = line.substring(c2 + 1).toInt();
    if (!meshChannelToIndex(chStart, si) || !meshChannelToIndex(chEnd, ei)) continue;
    if (ei < si) { uint8_t t = si; si = ei; ei = t; }
    memcpy(meshChOverrides[meshChOverrideCount].mac, mac, 6);
    meshChOverrides[meshChOverrideCount].startIdx = si;
    meshChOverrides[meshChOverrideCount].endIdx   = ei;
    meshChOverrideCount++;
  }
  f.close();
  Serial.printf("[CORE] Loaded %d channel override(s)\n", (int)meshChOverrideCount);
}

static bool meshChOverrideFind(const uint8_t* mac, uint8_t& startIdx, uint8_t& endIdx) {
  for (uint8_t i = 0; i < meshChOverrideCount; i++) {
    if (memcmp(meshChOverrides[i].mac, mac, 6) == 0) {
      startIdx = meshChOverrides[i].startIdx;
      endIdx   = meshChOverrides[i].endIdx;
      return true;
    }
  }
  return false;
}

// ---- Public API, used by the Web UI handlers further down ----
static uint8_t meshChOverrideGetCount() { return meshChOverrideCount; }

static bool meshChOverrideGetAt(uint8_t idx, uint8_t* macOut, uint16_t& chStartOut, uint16_t& chEndOut) {
  if (idx >= meshChOverrideCount) return false;
  memcpy(macOut, meshChOverrides[idx].mac, 6);
  chStartOut = JCMK_CHANNELS[meshChOverrides[idx].startIdx];
  chEndOut   = JCMK_CHANNELS[meshChOverrides[idx].endIdx];
  return true;
}

static bool meshChOverrideSet(const uint8_t* mac, int chStart, int chEnd) {
  uint8_t si, ei;
  if (!meshChannelToIndex(chStart, si) || !meshChannelToIndex(chEnd, ei)) return false;
  if (ei < si) { uint8_t t = si; si = ei; ei = t; }
  for (uint8_t i = 0; i < meshChOverrideCount; i++) {
    if (memcmp(meshChOverrides[i].mac, mac, 6) == 0) {
      meshChOverrides[i].startIdx = si;
      meshChOverrides[i].endIdx   = ei;
      meshChOverrideSave();
      if (meshCoreActive) coreReassignChannels();
      return true;
    }
  }
  if (meshChOverrideCount >= MESH_CHOVR_MAX) return false;
  memcpy(meshChOverrides[meshChOverrideCount].mac, mac, 6);
  meshChOverrides[meshChOverrideCount].startIdx = si;
  meshChOverrides[meshChOverrideCount].endIdx   = ei;
  meshChOverrideCount++;
  meshChOverrideSave();
  if (meshCoreActive) coreReassignChannels();
  return true;
}

static bool meshChOverrideClear(const uint8_t* mac) {
  for (uint8_t i = 0; i < meshChOverrideCount; i++) {
    if (memcmp(meshChOverrides[i].mac, mac, 6) == 0) {
      for (uint8_t j = i; j < meshChOverrideCount - 1; j++) meshChOverrides[j] = meshChOverrides[j + 1];
      meshChOverrideCount--;
      meshChOverrideSave();
      if (meshCoreActive) coreReassignChannels();
      return true;
    }
  }
  return false;
}

// ---- Core mode helpers (main loop only) ----
static void coreFindOrAddNode(const uint8_t* mac, bool isBiscuit, bool isPiglet, uint8_t boardType, const char* version) {
  for (uint8_t i = 0; i < CORE_MAX_NODES; i++) {
    if (coreNodes[i].active && memcmp(coreNodes[i].mac, mac, 6) == 0) {
      coreNodes[i].lastHbMs = millis();
      if (isPiglet) coreNodes[i].isPiglet = true;  // never downgrade once confirmed
      if (coreNodes[i].boardType == PIGLET_BOARD_UNKNOWN) coreNodes[i].boardType = boardType;
      // A changed version (e.g. just pushed a firmware update and rebooted)
      // needs a fresh mismatch evaluation -- clear any stale decline/attempt.
      if (version[0] && strncmp(coreNodes[i].version, version, sizeof(coreNodes[i].version)) != 0) {
        strncpy(coreNodes[i].version, version, sizeof(coreNodes[i].version) - 1);
        coreNodes[i].version[sizeof(coreNodes[i].version) - 1] = '\0';
        coreNodes[i].updateDeclined      = false;
        coreNodes[i].autoUpdateAttempted = false;
      }
      return;
    }
  }
  for (uint8_t i = 0; i < CORE_MAX_NODES; i++) {
    if (!coreNodes[i].active) {
      coreNodes[i].active = true; coreNodes[i].lastHbMs = millis();
      coreNodes[i].recordsRx = 0; coreNodes[i].isBiscuit = isBiscuit;
      coreNodes[i].isPiglet = isPiglet; coreNodes[i].slotIndex = 0;
      coreNodes[i].boardType = boardType;
      strncpy(coreNodes[i].version, version, sizeof(coreNodes[i].version) - 1);
      coreNodes[i].version[sizeof(coreNodes[i].version) - 1] = '\0';
      coreNodes[i].updateDeclined      = false;
      coreNodes[i].autoUpdateAttempted = false;
      memcpy(coreNodes[i].mac, mac, 6);
      coreNodeCount++; jcmkAddPeer(mac);
      // Re-send CORE_REPLY now that the peer is registered.
      coreSendReply(mac);
      Serial.printf("[CORE] New %s node %d: %02X:%02X:%02X:%02X:%02X:%02X\n",
        isPiglet ? "Piglet" : (isBiscuit ? "Biscuit" : "JCMK"),
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

  // Manual overrides win over the even split for any matching MAC -- applied
  // after the split so an overridden node's range doesn't skew how the
  // remaining channels are divided among everyone else.
  for (uint8_t n = 0; n < count; n++) {
    uint8_t si, ei;
    if (meshChOverrideFind(coreNodes[slots[n]].mac, si, ei)) {
      coreNodes[slots[n]].startIdx = si;
      coreNodes[slots[n]].endIdx   = ei;
    }
  }
  coreAssignVer++;

  // Assign deterministic transmit-slot indices among Piglet-confirmed nodes
  // only -- JCMK/Biscuit nodes don't participate and are unaffected.
  uint8_t pigletIdx = 0;
  for (uint8_t n = 0; n < count; n++) {
    if (coreNodes[slots[n]].isPiglet) coreNodes[slots[n]].slotIndex = pigletIdx++;
  }
  corePigletSlotCount = pigletIdx;

  for (uint8_t n = 0; n < count; n++) {
    uint8_t slot = slots[n];
    if (coreNodes[slot].isBiscuit) {
      coreSendBiscuitRoleAndConfig(coreNodes[slot].mac,
                                   coreNodes[slot].startIdx,
                                   coreNodes[slot].endIdx);
    } else {
      jcmk_admin_msg_t msg; memcpy(msg.magic, JCMK_MAGIC, 4);
      msg.type = JCMK_MSG_ADMIN;
      msg.assignment_version = coreAssignVer;
      msg.start_channel_idx = coreNodes[slot].startIdx;
      msg.end_channel_idx = coreNodes[slot].endIdx;
      if (coreNodes[slot].isPiglet) {
        msg.node_index = coreNodes[slot].slotIndex;
        msg.node_count = corePigletSlotCount;
      } else {
        msg.node_index = n; msg.node_count = count;
      }
      esp_now_send(coreNodes[slot].mac, (uint8_t*)&msg, sizeof(msg));
    }
    delay(10);
  }
  Serial.printf("[CORE] Reassigned: %d nodes v%d (%d Piglet slots)\n", count, coreAssignVer, corePigletSlotCount);
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
      msg.type = JCMK_MSG_ADMIN;
      msg.assignment_version = coreAssignVer;
      msg.start_channel_idx = coreNodes[i].startIdx;
      msg.end_channel_idx = coreNodes[i].endIdx;
      if (coreNodes[i].isPiglet) {
        msg.node_index = coreNodes[i].slotIndex;
        msg.node_count = corePigletSlotCount;
      } else {
        msg.node_index = n; msg.node_count = coreNodeCount;
      }
      esp_now_send(coreNodes[i].mac, (uint8_t*)&msg, sizeof(msg));
    }
    n++;
    delay(10);
  }
}

static void coreSendHeartbeatToAll() {
  // Broadcast a single heartbeat instead of per-node unicast.
  // Reduces ch 6 congestion from 12 sends down to 1.
  jcmk_text_msg_t msg = {};
  memcpy(msg.magic, JCMK_MAGIC, 4);
  msg.type = JCMK_MSG_HEARTBEAT; msg.counter = ++coreHbCounter; msg.len = 0;
  esp_now_send(JCMK_BCAST, (uint8_t*)&msg, sizeof(msg));
}

// ---------------- Wardrive Go BLE bridge (in separate header to avoid Arduino ctags bug) ----------------
#include "WardriveGoBLE.h"

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
  wardriveGoPushAp(bssid, ssid, auth, ch, rssi);
  digitalWrite(PINS.tft_cs, HIGH);
  if (appendWigleRow(bssid, ssid, auth, iso8601NowUTC(), ch, rssi, lat, lon, altM, accM)) {
    coreRecordsRx++;
  }
  // Feed the yield tracker regardless of source node -- see core5gStat above.
  if (ch > 14) {
    int gi = core5gIndexOf((uint8_t)ch);
    if (gi >= 0) core5gHits[gi]++;
  }
}

static void enterCoreMode() {
  Serial.println("[CORE] Entering Core mode");
  meshCoreActive=false; coreRecordsRx=0; coreNodeCount=0;
  coreAssignVer=0; coreHbCounter=0; coreLastHbMs=0;
  coreReqHead=coreReqTail=0; coreTextHead=coreTextTail=0;
  memset(coreNodes, 0, sizeof(coreNodes));
  meshChOverrideLoad();
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
  esp_now_register_send_cb(jcmkOnSent);
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

  // Wardrive Go BLE bridge -- only ever active while on this page, gated on
  // the config toggle. Relays every node-forwarded AP (coreParseAndLogText())
  // plus this Core's own GPS.
  if (cfg.wardriveGoBridge) wardriveGoBleBegin(true);
}

static void exitCoreMode() {
  Serial.println("[CORE] Exiting Core mode");
  if (meshCoreActive) esp_now_deinit();
  meshCoreActive=false; coreNodeCount=0;

  wardriveGoBleStop();  // no-op if it was never begun

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

  wardriveGoBleTick();  // no-op unless the bridge was begun (cfg.wardriveGoBridge)

  // Relay this Core's own GPS fix ~1 Hz, independent of node traffic.
  {
    static uint32_t wgGpsMs = 0;
    if (gpsHasFix && now - wgGpsMs >= 1000) {
      wgGpsMs = now;
      int sats = gps.satellites.isValid() ? (int)gps.satellites.value() : 0;
      wardriveGoPushGps(gps.location.lat(), gps.location.lng(),
                         gps.altitude.isValid() ? gps.altitude.meters() : 0.0, sats);
    }
  }

  while (coreReqHead!=coreReqTail) {
    uint8_t i=coreReqHead; coreReqHead=(coreReqHead+1)%CORE_REQ_QUEUE;
    coreFindOrAddNode(coreReqBuf[i].mac, coreReqBuf[i].isBiscuit, coreReqBuf[i].isPiglet, coreReqBuf[i].boardType, coreReqBuf[i].version);
    coreReassignChannels();
  }
  while (coreTextHead!=coreTextTail) {
    uint8_t i=coreTextHead; coreTextHead=(coreTextHead+1)%CORE_TEXT_QUEUE;
    coreParseAndLogText(coreTextBuf[i].line);
  }
  // Fold this window's per-channel hit counts into the yield EWMA (no-op
  // between 4s windows) -- see core5gStat / coreSendBiscuitRoleAndConfig.
  coreYieldUpdate(now);
  // Drive any in-flight push of firmware to a connected node (no-op unless
  // coreOtaStart() has armed one) -- see the Core-side OTA engine above.
  coreOtaTick();
  // Queue newly-mismatched nodes for the on-device update prompt / headless
  // auto-push, then advance the prompt countdown and step the confirmed
  // queue through coreOtaStart() one node at a time.
  coreUpdateDetect();
  coreUpdateProcessTick();
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
  // The Wardrive Go app's channel selection takes effect here, not just as a filter
  // on what gets reported to it -- but a real Biscuit Core's channel assignment still
  // wins if both happen to be active at once (biscuitChannelCount checked first below).
  bool useWgChannels = !biscuitChannelCount &&
                       wardriveGoChannelOverride(wgOverrideChannels, &wgOverrideCount);

  uint8_t numCh = biscuitChannelCount ? biscuitChannelCount :
                  useWgChannels ? wgOverrideCount :
                  ((jcmkEndIdx >= jcmkStartIdx) ? (jcmkEndIdx - jcmkStartIdx + 1) : 0);
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
      // Random jitter (0-200 ms) staggers admin windows across nodes
      nodeScanAdminMs  = millis() + (uint32_t)(esp_random() % 200);
      return;
    }

    uint8_t chIdx = jcmkStartIdx + nodeScanChOffset;
    if (!biscuitChannelCount && !useWgChannels && chIdx >= JCMK_NUM_CHANNELS) { nodeScanChOffset++; return; }
    uint8_t channel = biscuitChannelCount ? biscuitChannels[nodeScanChOffset]
                     : useWgChannels      ? wgOverrideChannels[nodeScanChOffset]
                                          : JCMK_CHANNELS[chIdx];

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

    if (!jcmkCoreIsPiglet) {
      // Legacy immediate-send path: non-Piglet Core (real JCMK hardware or
      // Biscuit Pro), or Core type not yet confirmed. Unchanged from before.
      jcmkSetChannel(JCMK_ESPNOW_CH);
      for (int i = 0; i < n; i++) {
        String bssid = WiFi.BSSIDstr(i);
        String ssid  = WiFi.SSID(i);
        String auth  = authModeToString(WiFi.encryptionType(i));
        int    ch    = WiFi.channel(i);
        int    rssi  = WiFi.RSSI(i);

        // Pushed before the Biscuit bracket-notation rewrite below so the
        // phone always gets the plain WiGLE-style auth token (OPEN/WPA2/...).
        wardriveGoPushAp(bssid, ssid, auth, ch, rssi);

        if (biscuitPlainReplyPending) {
          if (auth == "OPEN") auth = "[OPEN]";
          else if (auth == "WEP") auth = "[WEP]";
          else if (auth == "WPA") auth = "[WPA_PSK]";
          else if (auth == "WPA2") auth = "[WPA2_PSK]";
          else if (auth == "WPA3") auth = "[WPA3_PSK]";
          else if (auth == "WPAWPA2") auth = "[WPA_WPA2_PSK]";
          else if (auth == "WPA2EAP") auth = "[WPA2_ENTERPRISE]";
          else if (auth == "WPA2WPA3") auth = "[WPA2_WPA3_PSK]";
          else auth = "[UNDEFINED]";
        }
        String line  = bssid + "," + ssid + "," + auth + ","
                     + String(ch) + "," + String(rssi) + ",W";
        // Log the exact payload content -- we've verified scanning finds
        // networks and sends are attempted/delivered at the radio level, but
        // never actually inspected what's inside the message being sent.
        Serial.printf("[MESH] TX: %s\n", line.c_str());
        jcmkSendText(line);
        jcmkSentCount++;
      }
      ledPulseGreen();
    } else {
      // Piglet-to-Piglet: buffer results instead of sending immediately.
      // Scanning keeps running at full pace, untouched by other nodes'
      // schedules -- jcmkPigletSlotTick() flushes this buffer only during
      // this node's own deterministic transmit slot.
      for (int i = 0; i < n; i++) {
        String bssid = WiFi.BSSIDstr(i);
        String ssid  = WiFi.SSID(i);
        String auth  = authModeToString(WiFi.encryptionType(i));
        int    ch    = WiFi.channel(i);
        int    rssi  = WiFi.RSSI(i);
        wardriveGoPushAp(bssid, ssid, auth, ch, rssi);
        String line  = bssid + "," + ssid + "," + auth + ","
                     + String(ch) + "," + String(rssi) + ",W";
        jcmkPendingPush(line);
      }
    }
  }
  WiFi.scanDelete();
  nodeScanActive = false;
  nodeScanChOffset++;
}

// ================================================================
//  Piglet-to-Piglet deterministic transmit-slot flush. Only ever active
//  when jcmkCoreIsPiglet is true; otherwise a no-op, so non-Piglet
//  pairings are completely unaffected. Never interrupts an in-flight
//  scan (only runs in the natural gap between channel scans), so a
//  node's own scanning loop is never slowed down by this.
// ================================================================
static void jcmkPigletSlotTick() {
  if (!jcmkCoreIsPiglet) return;
  if (nodeScanActive) return;      // never interrupt a live async scan
  if (jcmkPendingEmpty()) return;

  uint32_t slotCount = (jcmkSlotCount > 0) ? jcmkSlotCount : 1;
  uint32_t cycleMs   = JCMK_SLOT_MS * slotCount;
  uint32_t cyclePos  = (millis() - jcmkCycleEpochMs) % cycleMs;
  uint32_t slotStart = (uint32_t)jcmkSlotIndex * JCMK_SLOT_MS;

  if (cyclePos < slotStart || cyclePos >= slotStart + JCMK_SLOT_MS) return;  // not my turn yet

  jcmkSetChannel(JCMK_ESPNOW_CH);
  String line;
  uint8_t sent = 0;
  while (sent < 16 && jcmkPendingPop(line)) {
    jcmkSendText(line);
    jcmkSentCount++;
    sent++;
  }
  ledPulseGreen();
}

// Enter mesh node mode — call on page 4 entry
static void enterNodeMode() {
  Serial.println("[MESH] Entering node mode");
  meshNodeActive        = false;
  jcmkHaveCore          = false;
  jcmkCoreFoundPending  = false;
  jcmkCoreIsPigletPending = false;
  jcmkCoreIsPiglet      = false;
  jcmkSlotIndex         = 0;
  jcmkSlotCount         = 1;
  jcmkCycleEpochMs      = 0;
  jcmkCoreLastSeenMs    = 0;
  jcmkPendingHead       = 0;
  jcmkPendingTail       = 0;
  jcmkNetworksFound     = 0;
  jcmkSentCount         = 0;
  jcmkSendFailCount     = 0;
  jcmkHbCounter         = 0;
  jcmkTextCounter       = 0;
  jcmkLastHbMs          = 0;
  jcmkLastReqMs         = 0;
  jcmkReqInterval       = JCMK_REQ_INIT_MS;
  jcmkStartIdx          = 0;
  jcmkEndIdx            = JCMK_NUM_CHANNELS - 1;
  jcmkAssignVer         = 0;
  nodeScanActive        = false;
  nodeScanChOffset      = 0;
  nodeScanAdminWin      = false;
  biscuitChannelCount   = 0;

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
  esp_now_register_send_cb(jcmkOnSent);

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

  // Wardrive Go BLE bridge -- only ever active while on this page, gated on
  // the config toggle. Relays this node's own finds (see nodeDoScanTick()).
  if (cfg.wardriveGoBridge) wardriveGoBleBegin(false);
}

// Exit mesh node mode — call when leaving page 4
static void exitNodeMode() {
  Serial.println("[MESH] Exiting node mode");
  if (meshNodeActive) esp_now_deinit();
  meshNodeActive = false;
  jcmkHaveCore   = false;

  wardriveGoBleStop();  // no-op if it was never begun

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

  wardriveGoBleTick();  // no-op unless the bridge was begun (cfg.wardriveGoBridge)
  wardriveGoSetGpsPresent(gps.charsProcessed() > 0);

  // Relay this node's own GPS fix ~1 Hz, independent of ESP-NOW pairing state.
  {
    static uint32_t wgGpsMs = 0;
    if (gpsHasFix && now - wgGpsMs >= 1000) {
      wgGpsMs = now;
      int sats = gps.satellites.isValid() ? (int)gps.satellites.value() : 0;
      wardriveGoPushGps(gps.location.lat(), gps.location.lng(),
                         gps.altitude.isValid() ? gps.altitude.meters() : 0.0, sats);
    }
  }

  // Consume pending core-found event from the ESP-Now callback
  if (jcmkCoreFoundPending) {
    jcmkCoreFoundPending = false;
    memcpy(jcmkCoreMac, jcmkCoreMacPending, 6);
    jcmkHaveCore       = true;
    jcmkCoreIsPiglet   = jcmkCoreIsPigletPending;
    jcmkCoreLastSeenMs = now;
    jcmkCycleEpochMs   = now;
    jcmkReqInterval    = JCMK_REQ_INIT_MS;
    jcmkAddPeer(jcmkCoreMac);
    if (biscuitPlainReplyPending) {
      // Identify ourselves to the Biscuit Core ("Piglet:<version>") and give
      // it a short ch-6 admin window to send its role/channel-config handshake.
      jcmkSetChannel(JCMK_ESPNOW_CH);
      biscuitSendControl(7, 0, "Piglet:" FIRMWARE_VERSION);
      nodeScanAdminWin = true;
      nodeScanAdminMs = now;
    }
    Serial.printf("[MESH] Core: %02X:%02X:%02X:%02X:%02X:%02X%s\n",
      jcmkCoreMac[0], jcmkCoreMac[1], jcmkCoreMac[2],
      jcmkCoreMac[3], jcmkCoreMac[4], jcmkCoreMac[5],
      jcmkCoreIsPiglet ? " (Piglet — slot scheduling active)" : "");
  }

  // Core-timeout detection: if the Core goes silent, stop scanning/sending
  // into the void and return to actively searching for a (possibly new) Core.
  if (jcmkHaveCore && (now - jcmkCoreLastSeenMs >= JCMK_CORE_TIMEOUT_MS)) {
    Serial.println("[MESH] Core timed out — returning to search");
    if (esp_now_is_peer_exist(jcmkCoreMac)) esp_now_del_peer(jcmkCoreMac);
    jcmkHaveCore     = false;
    jcmkCoreIsPiglet = false;
    jcmkAssignVer    = 0;
    jcmkStartIdx     = 0;
    jcmkEndIdx       = JCMK_NUM_CHANNELS - 1;
    jcmkSlotIndex    = 0;
    jcmkSlotCount    = 1;
    jcmkPendingHead  = jcmkPendingTail = 0;  // drop any buffered-but-unsent results
    nodeScanActive   = false;
    nodeScanAdminWin = false;
    biscuitChannelCount = 0;
    jcmkReqInterval  = JCMK_REQ_INIT_MS;
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

  // Per-channel async scan — runs continuously while connected to Core, OR
  // while the Wardrive Go bridge is enabled even with no Core at all, so a
  // solo Node (no mesh coordinator in range) still scans and feeds the phone
  // app instead of sitting idle on "Searching..." forever. The CORE_REQUEST
  // backoff above keeps announcing in the background either way, so a real
  // coordinator can still be picked up later without needing a restart.
  // jcmkPigletSlotTick() only ever acts for confirmed-Piglet pairings, and
  // only in the gap between scans, so it never affects scan timing itself.
  // Drive any in-flight firmware push from a Piglet/XIAO Core (no-op unless
  // one is pending/active) -- see the Node-side OTA receiver above. Scanning
  // is paused for the duration (nodeOtaStart() parks the radio on ch 6).
  nodeOtaTick();

  if ((jcmkHaveCore || cfg.wardriveGoBridge) && !nodeOtaActive) {
    jcmkPigletSlotTick();
    nodeDoScanTick();
  }
}

// Short label for the "WG: ..." indicator that replaces the Hold hint when
// cfg.wardriveGoBridge is on. Only three states are shown: NimBLE doesn't
// expose a separate "streaming" state since subscribe implies streaming.
static const char* wgStatusLabel() {
  if (!wardriveGoBridgeRunning())   return "WG: off";
  if (wardriveGoBridgeConnected())  return "WG: connected";
  return "WG: advertising";
}

// Scrolling window into a longer string, for display space too small to show it all
// at once (e.g. a multi-channel Wardrive Go `-ch` selection). Advances by one
// character roughly every redraw (the Mesh page redraws at a ~500ms cadence), with a
// gap before it repeats so it reads as a continuous loop rather than jumping.
static const char* scrollText(const char* full, uint8_t windowChars) {
  static char out[24];
  size_t flen = strlen(full);
  if (windowChars >= sizeof(out)) windowChars = sizeof(out) - 1;
  if (flen <= windowChars) {
    strncpy(out, full, sizeof(out) - 1); out[sizeof(out) - 1] = '\0';
    return out;
  }
  char padded[160];
  snprintf(padded, sizeof(padded), "%s   ", full);
  size_t plen = strlen(padded);
  uint32_t pos = (millis() / 500) % plen;
  for (uint8_t i = 0; i < windowChars; i++) out[i] = padded[(pos + i) % plen];
  out[windowChars] = '\0';
  return out;
}

// ================================================================
//  Page 4: handles both Mesh Node and Core mode
// ================================================================
static void drawPageMeshNode() {
  int W = tft.width();

  if (meshCoreActive) {
    CoreUpdatePromptStatus ups = coreUpdatePromptStatus();
    if (ups.promptActive || ups.pushActive) {
      // A pending node-firmware mismatch takes over the whole page -- this
      // replaces the normal Core status layout rather than overlaying it.
      // The mesh-page button handler (pollButton()) consumes a press as
      // confirm while ups.promptActive, instead of advancing the page.
      pageNeedsInit[4] = true;  // force a clean redraw once the overlay ends
      tft.fillScreen(BLACK);
      tft.setTextColor(WHITE, BLACK); tft.setTextSize(1);
      tft.setCursor(2, 2);
      tft.print(ups.pushActive ? "Updating Node" : "Update Node?");
      tft.drawFastHLine(0, 12, W, WHITE);

      uint8_t idx = ups.pushActive ? ups.pushNodeIdx : pendingUpdateNodes[0];
      tft.setCursor(2, 18);
      if (idx < CORE_MAX_NODES && coreNodes[idx].active) {
        uint8_t* m = coreNodes[idx].mac;
        char buf[18];
        snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
                 m[0], m[1], m[2], m[3], m[4], m[5]);
        tft.print(buf);
      } else {
        tft.print("--:--:--:--:--:--");
      }

      if (!ups.pushActive && ups.pendingCount > 1) {
        // Accurate only before a push starts -- pendingCount is the whole
        // batch, not a "remaining" count, so it's not shown mid-push.
        tft.setCursor(2, 30);
        char qb[20]; snprintf(qb, sizeof(qb), "+%d more queued", ups.pendingCount - 1);
        tft.print(qb);
      }

      if (ups.pushActive) {
        CoreOtaStatus ota = coreOtaStatus();
        tft.setCursor(2, 44);
        tft.print("Updating...");
        float pct = ota.totalBytes ? (float)ota.sentBytes / (float)ota.totalBytes : 0.0f;
        int barW = W - 4;
        tft.drawRect(2, 56, barW, 10, WHITE);
        int fillW = (int)((barW - 2) * pct);
        if (fillW > 0) tft.fillRect(3, 57, fillW, 8, COLOR_GREEN);
      } else {
        tft.setCursor(2, 44);
        uint32_t secLeft = (ups.promptRemainingMs + 999) / 1000;
        char cb[22]; snprintf(cb, sizeof(cb), "Press=Yes (%lus)", (unsigned long)secLeft);
        tft.print(cb);
        tft.setCursor(2, 58);
        tft.setTextColor(0x4208, BLACK); tft.print("Ignore = Skip");
        tft.setTextColor(WHITE, BLACK);
      }
      return;
    }

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

    // Hint (y=110), or Wardrive Go status if the bridge is enabled
    tft.fillRect(0, 110, W, 10, BLACK); tft.setCursor(2, 111);
    if (cfg.wardriveGoBridge) {
      tft.setTextColor(wardriveGoBridgeConnected() ? COLOR_GREEN : COLOR_YELLOW, BLACK);
      tft.print(wgStatusLabel());
    } else {
      tft.setTextColor(0x4208, BLACK); tft.print("Hold btn = Node mode");
    }
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
    {
      uint8_t wgCh[48]; uint8_t wgN = 0;
      if (wardriveGoChannelOverride(wgCh, &wgN)) {
        char list[160]; int len = 0;
        for (uint8_t i = 0; i < wgN && len < (int)sizeof(list) - 5; i++)
          len += snprintf(list + len, sizeof(list) - len, i ? ",%u" : "%u", (unsigned)wgCh[i]);
        tft.print(scrollText(list, 20));
      } else if (jcmkAssignVer>0 && jcmkStartIdx<JCMK_NUM_CHANNELS && jcmkEndIdx<JCMK_NUM_CHANNELS) {
        tft.print(JCMK_CHANNELS[jcmkStartIdx]); tft.print("-"); tft.print(JCMK_CHANNELS[jcmkEndIdx]);
      } else { tft.print("all"); }
    }

    tft.fillRect(0, 64, W, 10, BLACK); tft.setCursor(2, 65);
    tft.print("Found: "); tft.print(jcmkNetworksFound);

    tft.fillRect(0, 77, W, 10, BLACK); tft.setCursor(2, 78);
    tft.print("Sent: "); tft.print(jcmkSentCount);

    // "Sent" only means esp_now_send() was called; "Fail" reflects the
    // actual delivery-status callback, so a healthy Sent count with a
    // climbing Fail count means the radio itself is dropping the frames.
    tft.fillRect(0, 90, W, 10, BLACK); tft.setCursor(2, 91);
    tft.print("Fail: "); tft.print(jcmkSendFailCount);

    tft.fillRect(0, 103, W, 10, BLACK); tft.setCursor(2, 104);
    tft.print("ENOW ch: "); tft.print(JCMK_ESPNOW_CH);

    // Hold hint (y=117), or Wardrive Go status if the bridge is enabled
    tft.fillRect(0, 117, W, 10, BLACK); tft.setCursor(2, 118);
    if (cfg.wardriveGoBridge) {
      tft.setTextColor(wardriveGoBridgeConnected() ? COLOR_GREEN : COLOR_YELLOW, BLACK);
      tft.print(wgStatusLabel());
    } else {
      tft.setTextColor(0x4208, BLACK); tft.print("Hold btn = Core mode");
    }
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

  // Try fastest-first (descending), capped by cfg.sdMaxSpiHz, and stop at the
  // first speed that mounts successfully. NOTE: this previously iterated
  // ascending and returned on first success -- since low speeds almost
  // always work, the card was effectively always pinned to 100 kHz and
  // never got a chance to negotiate anywhere near its real capability.
  uint32_t capHz = (cfg.sdMaxSpiHz > 0) ? cfg.sdMaxSpiHz : 20000000;
  const uint32_t freqs[] = { 20000000, 16000000, 12000000, 8000000, 4000000, 2000000, 1000000, 400000, 200000, 100000 };
  for (size_t i = 0; i < sizeof(freqs)/sizeof(freqs[0]); i++) {
    if (freqs[i] > capHz) continue;  // skip entries above the configured cap

    SD.end(); delay(20);
    Serial.printf("[SD] SD.begin(CS=%d) @ %lu Hz ... ", PINS.sd_cs, (unsigned long)freqs[i]);
    bool ok = SD.begin(PINS.sd_cs, SPI, freqs[i]);
    Serial.println(ok ? "OK" : "FAIL");

    if (ok) {
      uint8_t type = SD.cardType();
      if (type == CARD_NONE) { SD.end(); continue; }
      Serial.printf("[SD] Running at %lu Hz, cardType=%u, size=%llu MB\n",
                    (unsigned long)freqs[i], type, SD.cardSize() / (1024ULL * 1024ULL));
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
  JsonDocument doc(psramJsonAllocator());  // heap, not stack — avoids task stack overflow; bumped for ssidWhitelist1..10
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
  c["wardriftApiKey"]  = cfg.wardriftApiKey;
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
  c["uploadOnNetworkSeen"] = cfg.uploadOnNetworkSeen;
  c["wardriveGoBridge"] = cfg.wardriveGoBridge;
  c["autoUpdateHeadlessNodes"] = cfg.autoUpdateHeadlessNodes;
  for (int i = 0; i < 10; i++) {
    c[String("ssidWhitelist") + String(i + 1)] = cfg.ssidWhitelist[i];
  }

  String out; out.reserve(measureJson(doc) + 1); serializeJson(doc, out);
  server.send(200, "application/json", out);
}

// Minimum internal free heap to keep in reserve while walking a directory.
// Each fopen()/fclose() cycle here (via SD.open()'s underlying VFS layer)
// permanently loses roughly 100-130 bytes of internal heap with no recovery
// -- unaffected by shrinking the file's stdio buffer via setBufferSize(), so
// the loss isn't that buffer; it's deeper in the Arduino core's newlib/VFS
// fopen() plumbing, outside this codebase's control. The Web UI now only
// ever requests one page (default 50 files) at a time via offset/limit,
// which keeps a normal page far under this floor; it remains as a safety
// net for a worst-case jump to a very large offset on a directory with many
// thousands of files.
static const uint32_t DIR_LISTING_HEAP_FLOOR = 24576;

// Walks /logs then /uploaded as one continuous, stably-ordered sequence,
// skipping the first `offset` entries and collecting up to `limit` of them
// into `arr`. Peeking one entry past the page sets `hasMore` without
// including it, so the Web UI's "Load More" button only appears when there
// really is another page. Returns true if the heap-floor circuit breaker
// had to stop the walk early.
static bool collectFilesPage(JsonArray arr, uint32_t offset, uint32_t limit, bool& hasMore) {
  hasMore = false;
  uint32_t index = 0;
  bool floorHit = false;
  const char* dirs[] = { "/logs", "/uploaded" };

  for (const char* dir : dirs) {
    if (floorHit) break;
    digitalWrite(PINS.tft_cs, HIGH);
    File root = SD.open(dir);
    if (!root) continue;

    File f = root.openNextFile();
    while (f) {
      if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL) < DIR_LISTING_HEAP_FLOOR) {
        f.close();
        floorHit = true;
        break;
      }

      if (index < offset) {
        // Still skipping toward the requested page -- nothing to build yet.
        f.close();
        index++;
        f = root.openNextFile();
        continue;
      }

      if ((uint32_t)arr.size() >= limit) {
        // Page is already full; this file's mere existence means there's
        // another page, so stop without doing any more work on it.
        f.close();
        hasMore = true;
        break;
      }

      // Every fopen() in the Arduino core's VFS layer unconditionally allocates
      // a 4096-byte stdio buffer via setvbuf() (DEFAULT_FILE_BUFFER_SIZE in
      // vfs_api.cpp), even though this loop never reads file content -- .size()
      // below is served from the fstat() already cached in the File's
      // constructor. Shrinking the buffer immediately after open (must be the
      // very next call per the library's own doc comment) reduces peak
      // per-file memory pressure/fragmentation.
      f.setBufferSize(64);

      String fullPath = normalizeSdPath(dir, f.name());
      if (fullPath.length() > 0) {
        JsonObject o = arr.createNestedObject();
        o["name"] = fullPath;
        o["size"] = (uint32_t)f.size();
      }
      f.close();
      index++;
      f = root.openNextFile();
    }
    root.close();
  }

  return floorHit;
}

static void handleFiles() {
  JsonDocument doc(psramJsonAllocator());  // heap, not stack — 4KB static would overflow 8KB task stack
  doc["ok"] = sdOk;
  JsonArray arr = doc.createNestedArray("files");
  bool hasMore   = false;
  bool truncated = false;
  if (sdOk) {
    uint32_t offset = server.hasArg("offset") ? (uint32_t)server.arg("offset").toInt() : 0;
    uint32_t limit  = server.hasArg("limit")  ? (uint32_t)server.arg("limit").toInt()  : 50;
    if (limit == 0 || limit > 200) limit = 50;  // sane bounds -- Web UI always asks for 50
    truncated = collectFilesPage(arr, offset, limit, hasMore);
  }
  doc["hasMore"]   = hasMore;
  doc["truncated"] = truncated;
  String out; out.reserve(measureJson(doc) + 1); serializeJson(doc, out);
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
  bool truncated = false;
  const char* dirs[] = { "/logs", "/uploaded" };
  for (const char* dir : dirs) {
    digitalWrite(PINS.tft_cs, HIGH);
    File root = SD.open(dir);
    if (!root) continue;
    std::vector<String> paths;
    File f = root.openNextFile();
    while (f) {
      f.setBufferSize(64);  // avoid the 4KB default stdio buffer -- see addDirFiles()
      String p = normalizeSdPath(dir, f.name());
      f.close();
      // Skip dot-prefixed housekeeping entries (the .gzscratch gzip scratch
      // directory, the .upload_state.cfg per-file upload ledger) -- "Delete
      // All" should only ever touch user wardriving data. Wiping the ledger
      // here used to be harmless only because every CSV it referenced was
      // also being deleted in the same pass; the heap-floor circuit breaker
      // below can stop that pass early, which left any not-yet-deleted CSVs
      // looking "never uploaded" on the next upload round.
      if (p.length() > 0 && !isHiddenSdEntry(pathBasename(p))) paths.push_back(p);

      // Same heap-floor circuit breaker as addDirFiles() -- a directory with
      // enough accumulated files can drain internal heap via this same
      // listing loop before the delete pass below even starts. Stopping
      // early here still deletes everything collected so far; the operator
      // can click "Delete All" again afterward to finish the rest.
      if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL) < DIR_LISTING_HEAP_FLOOR) {
        Serial.printf("[WEB] handleDeleteAll(%s): stopping listing early (heap floor reached)\n", dir);
        truncated = true;
        break;
      }

      f = root.openNextFile();
    }
    root.close();
    for (const String& p : paths) {
      if (currentCsvPath.length() > 0 && p == currentCsvPath) continue;
      digitalWrite(PINS.tft_cs, HIGH);
      if (SD.remove(p)) deleted++;
    }
  }

  JsonDocument doc(psramJsonAllocator());
  doc["ok"] = true;
  doc["deleted"] = deleted;
  doc["truncated"] = truncated;
  String out; out.reserve(measureJson(doc) + 1); serializeJson(doc, out);
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
  JsonDocument doc(psramJsonAllocator());
  doc["ok"] = ok; doc["tokenStatus"] = wigleTokenStatus;
  doc["httpCode"] = wigleLastHttpCode; doc["message"] = uploadLastResult;
  String out; out.reserve(measureJson(doc) + 1); serializeJson(doc, out);
  server.send(ok ? 200 : 400, "application/json", out);
}

static void handleWdgwarsTest() {
  if (WiFi.status() != WL_CONNECTED) { server.send(400,"application/json","{\"ok\":false,\"message\":\"STA WiFi not connected\"}"); return; }
  if (cfg.wdgwarsApiKey.length() < 8)  { server.send(400,"application/json","{\"ok\":false,\"message\":\"No API key configured\"}"); return; }
  bool ok = wdgwarsTestKey();
  JsonDocument doc(psramJsonAllocator());
  doc["ok"] = ok; doc["message"] = uploadLastResult;
  String out; out.reserve(measureJson(doc) + 1); serializeJson(doc, out);
  server.send(ok ? 200 : 400, "application/json", out);
}

static void handleWdgwarsUploadAll() {
  if (!sdOk) { server.send(500,"text/plain","SD not available"); return; }
  if (WiFi.status() != WL_CONNECTED) { server.send(400,"text/plain","Not connected"); return; }
  if (cfg.wdgwarsApiKey.length() < 8)  { server.send(400,"text/plain","No API key configured"); return; }
  uint32_t okCount = uploadAllCsvsToWdgwars();
  JsonDocument doc(psramJsonAllocator());
  doc["ok"]=(okCount>0); doc["uploaded"]=okCount; doc["total"]=uploadTotalFiles; doc["message"]=uploadLastResult;
  String out; out.reserve(measureJson(doc) + 1); serializeJson(doc, out);
  server.send(200, "application/json", out);
}

static void handleWardriftTest() {
  if (WiFi.status() != WL_CONNECTED) { server.send(400,"application/json","{\"ok\":false,\"message\":\"STA WiFi not connected\"}"); return; }
  if (cfg.wardriftApiKey.length() < 8)  { server.send(400,"application/json","{\"ok\":false,\"message\":\"No API key configured\"}"); return; }
  bool ok = wardriftTestKey();
  JsonDocument doc(psramJsonAllocator());
  doc["ok"] = ok; doc["message"] = uploadLastResult;
  String out; out.reserve(measureJson(doc) + 1); serializeJson(doc, out);
  server.send(ok ? 200 : 400, "application/json", out);
}

static void handleWardriftUploadAll() {
  if (!sdOk) { server.send(500,"text/plain","SD not available"); return; }
  if (WiFi.status() != WL_CONNECTED) { server.send(400,"text/plain","Not connected"); return; }
  if (cfg.wardriftApiKey.length() < 8)  { server.send(400,"text/plain","No API key configured"); return; }
  uint32_t okCount = uploadAllCsvsToWardrift();
  JsonDocument doc(psramJsonAllocator());
  doc["ok"]=(okCount>0); doc["uploaded"]=okCount; doc["total"]=uploadTotalFiles; doc["message"]=uploadLastResult;
  String out; out.reserve(measureJson(doc) + 1); serializeJson(doc, out);
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
        f.setBufferSize(64);  // avoid the 4KB default stdio buffer -- see addDirFiles()
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
      uint32_t sz = 0;
      if (!csvHasDataRows(path, &sz)) {
        Serial.printf("[CLEANUP] Empty CSV (%u bytes), deleting: %s\n", (unsigned)sz, pathBasename(path).c_str());
        if (!SD.remove(path)) {
          Serial.printf("[CLEANUP] WARNING: SD.remove failed for %s -- file left in place\n", pathBasename(path).c_str());
        }
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
  uint32_t okCount = uploadAllPending(-1);  // web always uploads all
  JsonDocument doc(psramJsonAllocator());
  doc["ok"] = (okCount > 0); doc["uploaded"] = okCount;
  doc["total"] = uploadTotalFiles; doc["message"] = uploadLastResult;
  String out; out.reserve(measureJson(doc) + 1); serializeJson(doc, out);
  server.send(200, "application/json", out);
}

static String gzScratchPathForOne(const String& basename) {
  ensureGzScratchDir();
  return gzScratchPathFor(basename);
}

static void handleWigleUploadOne() {
  if (!sdOk) { server.send(500, "text/plain", "SD not available"); return; }
  if (WiFi.status() != WL_CONNECTED) { server.send(400, "text/plain", "Not connected"); return; }
  if (!server.hasArg("name")) { server.send(400, "text/plain", "Missing name"); return; }
  String path = server.arg("name");
  digitalWrite(PINS.tft_cs, HIGH);
  if (!SD.exists(path)) { server.send(404, "text/plain", "Not found"); return; }

  bool hasWigle    = cfg.wigleBasicToken.length() >= 8;
  bool hasWdg      = cfg.wdgwarsApiKey.length()   >= 8;
  bool hasWardrift = cfg.wardriftApiKey.length()  >= 8;
  if (!hasWigle && !hasWdg && !hasWardrift) {
    server.send(400, "text/plain", "No WiGLE token, WDGoWars API key, or Wardrift token configured");
    return;
  }

  String basename = pathBasename(path);
  FileUploadState st = loadUploadState(basename);

  uploading = true; uploadPausedScanWasEnabled = scanningEnabled; scanningEnabled = false;
  uploadTotalFiles = 1; uploadDoneFiles = 0; uploadCurrentFile = path;
  ledBlue();

  // Mirrors the combined orchestrator's per-file gzip path (uploadAllPending()):
  // try whichever services are still pending for this file -- WDGoWars, then
  // Wardrift, then WiGLE -- skipping any already marked done, and only move
  // the file to /uploaded/ once every configured service is done.
  wigleLastHttpCode = 0;  // avoid reporting a stale code if WiGLE isn't attempted below
  String wdgResult = "Already done", wardriftResult = "Already done", wigleResult = "Already done";

  if (hasWdg && !st.wdgwDone) {
    uploadTargetName = "WDGW UL";
    tftWigleUploadScreen(uploadDoneFiles, uploadTotalFiles, pathBasename(path));
    bool ok;
    if (cfg.gzipUploads) {
      String scratch = gzScratchPathForOne(basename);
      ok = gzipCompressFileToFile(path, scratch) && uploadFileToWdgwars(scratch);
      SD.remove(scratch);
    } else {
      ok = uploadFileToWdgwars(path);
    }
    if (ok) { markServiceDone(basename, UploadService::Wdgw); st.wdgwDone = true; }
    wdgResult = uploadLastResult;
    if (hasWardrift || hasWigle) delay(1500);  // brief settle between TLS sessions
  }

  if (hasWardrift && !st.wardriftDone) {
    uploadTargetName = "Wardrift UL";
    tftWigleUploadScreen(uploadDoneFiles, uploadTotalFiles, pathBasename(path));
    // Always sent uncompressed -- Wardrift's /v1/ingest/signals endpoint
    // does not honor Content-Encoding: gzip (confirmed in the field: a
    // gzip-compressed body comes back HTTP 400 "file did not contain
    // supported signal rows", even though the same file succeeds against
    // WDGoWars, which does support gzip). Ignore cfg.gzipUploads here.
    bool ok = uploadFileToWardrift(path);
    if (ok) { markServiceDone(basename, UploadService::Wardrift); st.wardriftDone = true; }
    wardriftResult = uploadLastResult;
    if (hasWigle) delay(1500);  // brief settle between TLS sessions
  }

  if (hasWigle && !st.wigleDone) {
    uploadTargetName = "WiGLE UL";
    tftWigleUploadScreen(uploadDoneFiles, uploadTotalFiles, pathBasename(path));
    bool ok;
    if (cfg.gzipUploads) {
      String scratch = gzScratchPathForOne(basename);
      ok = gzipCompressFileToFile(path, scratch) && uploadFileToWigle(scratch);
      SD.remove(scratch);
    } else {
      ok = uploadFileToWigle(path);
    }
    if (ok) { markServiceDone(basename, UploadService::Wigle); st.wigleDone = true; }
    wigleResult = uploadLastResult;
  }

  uploadDoneFiles = 1; uploading = false;
  scanningEnabled = uploadPausedScanWasEnabled; uploadCurrentFile = ""; uploadTargetName = "";
  ledOff(); forceStatusFullRedraw();

  bool ok = allConfiguredServicesDone(st);
  if (ok) {
    // See the matching comment in uploadAllPending()'s finalize loop --
    // only clear the ledger once the move actually succeeds.
    if (moveToUploaded(path)) clearUploadState(basename);
    else Serial.printf("[UPLOAD] WARNING: moveToUploaded failed for %s -- file stays in /logs, ledger kept\n", basename.c_str());
  }

  int attempted = (hasWdg ? 1 : 0) + (hasWardrift ? 1 : 0) + (hasWigle ? 1 : 0);
  String message;
  if (attempted > 1) {
    std::vector<String> parts;
    if (hasWdg)      parts.push_back("WDGW: " + wdgResult);
    if (hasWardrift) parts.push_back("Wardrift: " + wardriftResult);
    if (hasWigle)    parts.push_back("WiGLE: " + wigleResult);
    for (size_t i = 0; i < parts.size(); i++) {
      if (i) message += " | ";
      message += parts[i];
    }
  } else if (hasWdg) {
    message = wdgResult;
  } else if (hasWardrift) {
    message = wardriftResult;
  } else {
    message = wigleResult;
  }

  JsonDocument doc(psramJsonAllocator());
  doc["ok"] = ok; doc["httpCode"] = wigleLastHttpCode; doc["message"] = message;
  String out; out.reserve(measureJson(doc) + 1); serializeJson(doc, out);
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
      f.setBufferSize(64);  // avoid the 4KB default stdio buffer -- see addDirFiles()
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

  // CRC32 (ISO 3309 / gzip polynomial 0xEDB88320) -- shared implementation
  // defined above, also used by the real-compression upload path.
  auto crc32Up = gzipCrc32Update;

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

// ================================================================
//  Mesh Nodes (Core-pushed OTA to connected nodes) -- Web UI endpoints
// ================================================================
static const char* meshBoardTypeLabel(uint8_t bt) {
  switch (bt) {
    case PIGLET_BOARD_NODE:    return "PigletNode";
    case PIGLET_BOARD_XIAO:    return "Piglet";
    case PIGLET_BOARD_TDONGLE: return "TDongleC5_Piglet";
    default:                   return "Unknown";
  }
}

static void handleMeshNodes() {
  JsonDocument doc(psramJsonAllocator());
  doc["coreActive"] = meshCoreActive;

  JsonArray arr = doc.createNestedArray("nodes");
  if (meshCoreActive) {
    uint32_t now = millis();
    for (uint8_t i = 0; i < CORE_MAX_NODES; i++) {
      if (!coreNodes[i].active) continue;
      JsonObject o = arr.createNestedObject();
      char macStr[18];
      snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
               coreNodes[i].mac[0], coreNodes[i].mac[1], coreNodes[i].mac[2],
               coreNodes[i].mac[3], coreNodes[i].mac[4], coreNodes[i].mac[5]);
      o["idx"]           = i;
      o["mac"]           = macStr;
      o["boardType"]     = coreNodes[i].boardType;
      o["board"]         = meshBoardTypeLabel(coreNodes[i].boardType);
      o["isPiglet"]      = coreNodes[i].isPiglet;
      o["isBiscuit"]     = coreNodes[i].isBiscuit;
      o["recordsRx"]     = coreNodes[i].recordsRx;
      o["lastSeenMsAgo"] = now - coreNodes[i].lastHbMs;
    }
  }

  CoreOtaStatus st = coreOtaStatus();
  JsonObject ota = doc.createNestedObject("ota");
  ota["phase"]      = (int)st.phase;
  ota["nodeIdx"]    = st.nodeIdx;
  ota["sentBytes"]  = st.sentBytes;
  ota["totalBytes"] = st.totalBytes;
  ota["msg"]        = st.msg;
  ota["loadMsg"]    = st.loadMsg;

  String out;
  out.reserve(measureJson(doc) + 1);
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

static void handleMeshOtaStart() {
  if (!meshCoreActive) { server.send(400, "text/plain", "Not in Core mode"); return; }
  if (!server.hasArg("idx")) { server.send(400, "text/plain", "Missing idx"); return; }

  uint8_t idx = (uint8_t)server.arg("idx").toInt();
  bool ok = coreOtaStart(idx);
  CoreOtaStatus st = coreOtaStatus();

  JsonDocument doc(psramJsonAllocator());
  doc["ok"]  = ok;
  doc["msg"] = st.msg;

  String out;
  out.reserve(measureJson(doc) + 1);
  serializeJson(doc, out);
  server.send(ok ? 200 : 400, "application/json", out);
}

static void handleMeshOtaCancel() {
  coreOtaAbort();
  server.send(200, "text/plain", "OK");
}

// ================================================================
//  Firmware Update (self-update from GitHub Releases)
//  Checks https://api.github.com/repos/Hamspiced/piglet/releases/latest for
//  a newer FIRMWARE_VERSION and, on a Web UI button click, downloads and
//  flashes this board's own release asset via Update.h. The other two
//  boards' assets are staged into /Firmware/ on the SD card in the same
//  pass, so the mesh Core-push OTA engine above can push them to connected
//  nodes without the operator ever touching an SD card.
// ================================================================
// Minimal pull-based de-chunking Stream wrapper for HTTP chunked
// transfer-encoding. GitHub's release API response (currently 34KB+ for
// this repo, and only growing as more assets/changelog accumulate) is
// served chunked (no Content-Length). http.getStream()/getStreamPtr()
// exposes the RAW connection -- chunk-size framing bytes are only
// stripped by HTTPClient's own body readers (getString()/writeToStream()),
// so parsing the raw stream directly would corrupt the JSON. Buffering the
// de-chunked body into one String first (via writeToStream()) was tried,
// but a single-digit-KB reserve() guess was hugely undersized against the
// real 34KB+ response -- String's grow-by-doubling then needs several large
// reallocations (8KB->16KB->32KB->~34KB+), each momentarily holding both
// the old and new buffer alive at once, on top of the TLS session's own
// buffers, against this device's much smaller (~80KB) free-heap budget.
// That was fragmenting/exhausting the heap badly enough to crash shortly
// afterward, regardless of how large the reserve() guess was bumped to.
// This wrapper performs the same de-chunking HTTPClient::writeToStream()
// does internally, but pull-based so ArduinoJson parses the response as it
// arrives -- peak memory stays a few hundred bytes (ArduinoJson's own
// parsing state) no matter how large the raw response is.
class ChunkedReader : public Stream {
public:
  explicit ChunkedReader(WiFiClient* client) : _client(client) {}

  int available() override {
    fillChunk();
    return (int)_remaining;
  }
  int read() override {
    fillChunk();
    if (_remaining == 0) return -1;
    // ArduinoJson's Stream reader explicitly avoids calling read() directly
    // because the bare, non-blocking _client->read() can return -1 the
    // instant a TLS record hasn't finished arriving/decrypting yet. Called
    // from Stream::timedRead()'s own retry loop (which has no yield()), a
    // bare read() here busy-spins and can starve the WiFi/lwIP task on this
    // single-core chip until the 1 s default Stream timeout gives up mid-
    // document -- producing a spurious "IncompleteInput" parse failure well
    // before the real end of a 34KB+ response. readBytes() uses the
    // underlying client's own proven blocking-with-timeout implementation
    // (the same one used successfully everywhere else in this codebase),
    // so it waits for a byte instead of giving up immediately.
    uint8_t b;
    if (_client->readBytes(&b, 1) != 1) return -1;
    _remaining--;
    _totalRead++;
    return b;
  }
  int peek() override {
    fillChunk();
    if (_remaining == 0) return -1;
    return _client->peek();
  }
  void flush() override {}
  size_t write(uint8_t) override { return 0; }  // write side unused

private:
  WiFiClient* _client;
  size_t      _remaining = 0;
  bool        _done      = false;
  bool        _first     = true;
  size_t      _totalRead = 0;  // bytes delivered via read() so far -- diagnostics only

  void fillChunk() {
    while (_remaining == 0 && !_done) {
      if (!_first) {
        // Consume the trailing CRLF after the previous chunk's data.
        char trail[2];
        int n = _client->readBytes(trail, 2);
        if (n != 2) {
          // A genuine end-of-stream is always a "0" chunk-size line (handled
          // below), never a short/aborted trailing-CRLF read. If this ever
          // fires, the underlying TLS/TCP read stalled or the connection
          // was cut mid-stream -- surfaced here (byte offset + bytes
          // actually read) since this exact spot is suspected of causing
          // the recurring "IncompleteInput" parse failures on GitHub's
          // 34KB+ chunked release-API response.
          Serial.printf("[FWUPD] ChunkedReader: short trailing-CRLF read (%d/2 bytes) after %u bytes -- ending stream early\n",
                        n, (unsigned)_totalRead);
          _done = true;
          return;
        }
      }
      _first = false;
      String line = _client->readStringUntil('\n');
      line.trim();
      long sz = strtol(line.c_str(), nullptr, 16);
      if (sz <= 0) { _done = true; return; }
      _remaining = (size_t)sz;
    }
  }
};

// Same blocking-read rationale as ChunkedReader above, for the other response
// shape this endpoint can return: a plain Content-Length body instead of
// chunked transfer-encoding (confirmed seen in practice -- getSize() > 0).
// Without this, deserializeJson(doc, *http.getStreamPtr(), ...) falls back to
// ArduinoJson's default Stream handling, which calls the bare, non-blocking
// read() directly and can hit the same spurious IncompleteInput failure
// ChunkedReader was built to avoid.
class SizedReader : public Stream {
public:
  SizedReader(WiFiClient* client, size_t totalLen) : _client(client), _remaining(totalLen) {}

  int available() override { return (int)_remaining; }
  int read() override {
    if (_remaining == 0) return -1;
    uint8_t b;
    if (_client->readBytes(&b, 1) != 1) return -1;
    _remaining--;
    return b;
  }
  int peek() override {
    if (_remaining == 0) return -1;
    return _client->peek();
  }
  void flush() override {}
  size_t write(uint8_t) override { return 0; }

private:
  WiFiClient* _client;
  size_t      _remaining;
};

static const char* FW_OWN_ASSET = "TDongleC5_Piglet.ota.bin";
static const char* FW_OTHER_ASSETS[] = { "Piglet.ota.bin", "PigletNode.ota.bin" };
static const int   FW_OTHER_ASSET_COUNT = 2;
static const int   FW_NODE_ASSET_IDX = 1;  // index of "PigletNode.ota.bin" within FW_OTHER_ASSETS[]

static const char* GITHUB_API_HOST = "api.github.com";
static const char* GITHUB_API_PATH = "/repos/Hamspiced/piglet/releases/latest";

// fwUpdateChecked/fwUpdateAvailable/fwAvailableVersion/fwReleaseNotes/
// fwCheckError/fwDownloading/fwDownloaded/fwDownloadStage/fwDownloadedBytes/
// fwDownloadTotalBytes/fwDownloadError/fwInstalling/fwInstallError are
// declared earlier in the file (Firmware Update State section, near the
// other top-level status globals) so updateTFT()/pollButton() can reference
// them -- see the comment there for why.
// fwCheckError in particular: deliberately left populated (not blanked) on
// a failed recheck -- fwUpdateAvailable/fwAvailableVersion stay at their
// last known-good values on failure, so a transient failed recheck can't
// make a previously-detected update disappear from the Web UI.

static String fwLog = "";
static const size_t FW_LOG_MAX = 2048;  // capped buffer -- oldest whole lines trimmed from the front

static bool     fwStagingNode = false;
static String   fwStageNodeError = "";

static String fwOwnAssetUrl;
static String fwOtherAssetUrls[FW_OTHER_ASSET_COUNT];

// printf-style: both Serial.printf()s and appends an elapsed-seconds-
// prefixed line to the capped fwLog buffer the Web UI's download/install
// modal polls. Oldest whole lines are dropped once the buffer exceeds
// FW_LOG_MAX, so a long-running transfer can't grow this unbounded.
static void fwLogAppend(const char* fmt, ...) {
  char msg[160];
  va_list args;
  va_start(args, fmt);
  vsnprintf(msg, sizeof(msg), fmt, args);
  va_end(args);
  Serial.printf("[FWUPD] %s\n", msg);

  char line[180];
  snprintf(line, sizeof(line), "[%lus] %s\n", (unsigned long)(millis() / 1000), msg);
  fwLog += line;
  if (fwLog.length() > FW_LOG_MAX) {
    int cut = (int)(fwLog.length() - FW_LOG_MAX);
    int nl = fwLog.indexOf('\n', cut);
    fwLog = (nl >= 0) ? fwLog.substring(nl + 1) : "";
  }
}

// Parses "vMAJOR.MINOR" (or "MAJOR.MINOR", with/without a leading 'v') into
// a comparable (major, minor) pair -- numeric, not lexical, so e.g. "v2.9"
// correctly compares as older than "v2.10".
static void fwParseVersion(const String& verIn, int& major, int& minor) {
  String v = verIn;
  v.trim();
  if (v.length() && (v[0] == 'v' || v[0] == 'V')) v = v.substring(1);
  int dot = v.indexOf('.');
  if (dot < 0) { major = v.toInt(); minor = 0; }
  else { major = v.substring(0, dot).toInt(); minor = v.substring(dot + 1).toInt(); }
}

static bool fwIsNewer(const String& latest, const String& current) {
  int lMaj, lMin, cMaj, cMin;
  fwParseVersion(latest, lMaj, lMin);
  fwParseVersion(current, cMaj, cMin);
  if (lMaj != cMaj) return lMaj > cMaj;
  return lMin > cMin;
}

// Logs free/largest-contiguous MALLOC_CAP_DMA heap -- the narrow pool the
// hardware AES/DMA driver allocates its (small) per-TLS-session descriptors
// from, separate from (and a subset of) general MALLOC_CAP_INTERNAL free
// heap. General free heap/largest-block can look perfectly healthy while
// this specific pool is exhausted/fragmented -- that mismatch is exactly
// the "esp-aes: Failed to allocate memory" / "SSL - Memory allocation
// failed" signature seen on Web UI-triggered Check-for-Update calls after a
// long uptime with a lot of prior TLS churn (boot upload storm, other
// button clicks). Same pattern as logDmaHeap() used elsewhere in this file.
static void fwLogDmaHeap(const char* tag) {
  Serial.printf("[FWUPD] %s: dmaFree=%u dmaLargest=%u\n", tag,
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
}

// Note: deliberately does NOT reset fwUpdateAvailable/fwAvailableVersion/
// fwReleaseNotes/fwOwnAssetUrl/fwOtherAssetUrls up front. A Web UI-triggered
// recheck can fail (e.g. transient DMA-pool exhaustion, see fwLogDmaHeap()
// below) after a prior check -- boot-time or manual -- already found a real
// update; blanking those fields before knowing whether this attempt will
// succeed made a failed recheck erase a previously-detected update from the
// Web UI, which read as "GitHub not reporting the correct version". Those
// fields are now only overwritten once a complete, valid GitHub response is
// in hand (see the bottom of this function). Every failure path below just
// records fwCheckError and returns, leaving prior state untouched.
static void fwCheckForUpdate() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[FWUPD] Skipped check (no STA WiFi)");
    fwCheckError    = "STA WiFi not connected";
    fwUpdateChecked = true;
    return;
  }

  Serial.println("[FWUPD] Checking GitHub for a newer release...");
  fwLogDmaHeap("Pre-check");
  fwCheckError = "";

  // Web UI-triggered calls can follow a long run of other TLS-heavy activity
  // (boot upload storm, other Check-for-Update/test-key clicks) that left
  // the narrow AES/DMA-descriptor pool fragmented. Closing a TLS session's
  // sockets/buffers doesn't always return them to that pool synchronously --
  // some of that cleanup is posted to and processed by the WiFi/lwIP task.
  // A short yield()+delay() here can't defragment the heap (this platform's
  // allocator has no compaction), but it does give that deferred cleanup a
  // chance to actually run and release blocks back to the pool before this
  // attempt races a measurement/allocation against it.
  yield();
  delay(100);

  JsonDocument filter(psramJsonAllocator());
  filter["tag_name"] = true;
  filter["body"]     = true;
  JsonObject assetFilter = filter["assets"].add<JsonObject>();
  assetFilter["name"] = true;
  assetFilter["browser_download_url"] = true;

  // Retry the WHOLE connect+GET+parse sequence, not just the connect --
  // unlike the upload paths' connect-only retries, this response is held
  // open over a multi-second TLS session (34KB+), so it's also vulnerable
  // to a transient esp-aes/DMA-pool hiccup mid-transfer rather than just at
  // connect time: a clean 200 response that then truncates partway through
  // the body (IncompleteInput), not a parsing bug. This is most likely to
  // bite via the Web UI's "Check for Update" button (as opposed to the
  // identical call at boot, the very first TLS operation since reset with a
  // fully rested DMA pool) because by then the page's own recurring status-
  // poll fetches (status.json/update/status/mesh endpoints) have been
  // competing for the same narrow pool -- confirmed by instrumented testing:
  // dmaLargest sat at an identical, depressed value across all 3 attempts of
  // a failing check, then recovered within a few seconds once loop() (and
  // therefore server.handleClient()) ran freely again. See the dmaFailure
  // fail-fast branch below for why retrying a DMA allocation failure
  // in-place doesn't help and is handled differently from other failures.
  JsonDocument doc(psramJsonAllocator());
  bool gotValidDoc = false;

  for (int attempt = 1; attempt <= 3 && !gotValidDoc; attempt++) {
    WiFiClientSecure client;
    client.setInsecure();
    // HTTPClient::setTimeout() only forwards to the underlying client if
    // already connected() -- harmless here since it's called pre-connect,
    // but means it never actually reaches `client`. Set it directly too,
    // since ChunkedReader's per-byte reads below rely on this client's own
    // configured timeout to wait properly instead of giving up too soon.
    client.setTimeout(10000);
    HTTPClient http;
    http.setTimeout(10000);
    if (!http.begin(client, String("https://") + GITHUB_API_HOST + GITHUB_API_PATH)) {
      Serial.printf("[FWUPD] HTTP begin failed (attempt %d/3)\n", attempt);
      if (attempt < 3) { delay(500); yield(); continue; }
      fwCheckError    = "Could not start HTTPS connection";
      fwUpdateChecked = true;
      return;
    }
    http.addHeader("User-Agent", "Piglet-Firmware");
    http.addHeader("Accept", "application/vnd.github+json");

    int code = http.GET();
    if (code != HTTP_CODE_OK) {
      // A negative code (e.g. -1 = HTTPC_ERROR_CONNECTION_REFUSED) means the
      // TLS/TCP connection itself never completed -- no HTTP response was
      // ever received, so this is NOT the firmware concluding "no update
      // available". client.lastError() surfaces the actual mbedTLS/socket
      // failure (DNS, handshake, timeout, etc.) instead of just the bare,
      // uninformative HTTPClient error code.
      bool dmaFailure = false;
      if (code < 0) {
        char errBuf[100];
        int sslErr = client.lastError(errBuf, sizeof(errBuf));
        Serial.printf("[FWUPD] GitHub API returned HTTP %d (TLS error %d: %s) -- attempt %d/3\n", code, sslErr, errBuf, attempt);
        // -32512 (MBEDTLS_ERR_SSL_ALLOC_FAILED) and esp-aes's own "Failed to
        // allocate memory" both stem from the same DMA-pool pressure.
        // Flagged here so the fail-fast branch below can skip retrying it.
        dmaFailure = (sslErr == -32512);
        fwLogDmaHeap("Check connect fail");
      } else {
        Serial.printf("[FWUPD] GitHub API returned HTTP %d -- attempt %d/3\n", code, attempt);
      }
      http.end();
      if (dmaFailure) {
        // Retrying in place doesn't help: the narrow DMA pool is depressed
        // by OTHER connections (the Web UI's own polling loops), not a
        // momentary hiccup that clears itself within this handler's own
        // retry delay -- instrumented testing showed dmaLargest identical
        // across all 3 attempts of a failing check. Worse, every attempt
        // spent here is time server.handleClient() isn't running, letting
        // those other connections' pending data pile up instead of
        // draining. Fail fast so loop() gets control back sooner and the
        // pool can recover the way it's observed to -- the Web UI already
        // preserves the last known update status, so pressing the button
        // again a few seconds later is harmless.
        fwCheckError    = "Low memory (TLS alloc failed) -- wait a few seconds and try again";
        fwUpdateChecked = true;
        return;
      }
      if (attempt < 3) { delay(500); yield(); continue; }
      fwCheckError    = "GitHub connection failed";
      fwUpdateChecked = true;
      return;
    }

    // See ChunkedReader's comment above for why this parses the response
    // through a blocking-read wrapper instead of buffering it whole or
    // handing ArduinoJson the raw stream directly. GitHub's release API
    // response has been observed both chunked (getSize() <= 0) and with a
    // real Content-Length (getSize() > 0) -- SizedReader covers the latter
    // case with the same readBytes()-based blocking reads ChunkedReader uses.
    doc.clear();
    DeserializationError err;
    int bodySize = http.getSize();
    Serial.printf("[FWUPD] Response getSize()=%d -- parsing via %s path (attempt %d/3)\n",
                  bodySize, (bodySize > 0) ? "sized" : "chunked", attempt);
    if (bodySize > 0) {
      SizedReader reader(http.getStreamPtr(), (size_t)bodySize);
      err = deserializeJson(doc, reader, DeserializationOption::Filter(filter));
    } else {
      ChunkedReader reader(http.getStreamPtr());
      err = deserializeJson(doc, reader, DeserializationOption::Filter(filter));
    }
    http.end();

    if (err) {
      Serial.printf("[FWUPD] JSON parse failed: %s -- attempt %d/3\n", err.c_str(), attempt);
      if (attempt < 3) { delay(500); yield(); continue; }
      fwCheckError    = "Failed to parse GitHub response";
      fwUpdateChecked = true;
      return;
    }

    gotValidDoc = true;
  }

  String tag = doc["tag_name"] | "";
  if (tag.length() == 0) {
    Serial.println("[FWUPD] No tag_name in response");
    fwCheckError    = "GitHub response missing tag_name";
    fwUpdateChecked = true;
    return;
  }

  // A complete, valid response is in hand -- safe to replace the published
  // state now. Resolve every asset's download URL unconditionally -- even
  // when this board is already on the latest release (!fwIsNewer() below).
  // "Store Piglet Node Firmware" (Web UI) needs the PigletNode asset URL
  // whether or not this board itself needs an update, so bailing out early
  // here (as this used to do) would leave fwOtherAssetUrls[] empty and
  // break that button on an up-to-date board.
  fwOwnAssetUrl = "";
  for (int i = 0; i < FW_OTHER_ASSET_COUNT; i++) fwOtherAssetUrls[i] = "";
  for (JsonObject asset : doc["assets"].as<JsonArray>()) {
    String name = asset["name"] | "";
    String url  = asset["browser_download_url"] | "";
    if (name == FW_OWN_ASSET) fwOwnAssetUrl = url;
    for (int i = 0; i < FW_OTHER_ASSET_COUNT; i++) {
      if (name == FW_OTHER_ASSETS[i]) fwOtherAssetUrls[i] = url;
    }
  }
  String notes = doc["body"] | "";
  fwReleaseNotes = notes.substring(0, 300);

  if (!fwIsNewer(tag, FIRMWARE_VERSION)) {
    Serial.printf("[FWUPD] Up to date (latest=%s, current=%s)\n", tag.c_str(), FIRMWARE_VERSION);
    fwAvailableVersion = tag;  // still useful to show "Latest Available" even when not newer
    fwUpdateAvailable  = false;
    fwUpdateChecked    = true;
    return;
  }

  if (fwOwnAssetUrl.length() == 0) {
    Serial.println("[FWUPD] Newer release found but no matching asset for this board");
    fwAvailableVersion = tag;
    fwUpdateAvailable  = false;
    fwUpdateChecked    = true;
    return;
  }

  fwAvailableVersion = tag;
  fwUpdateAvailable  = true;
  fwUpdateChecked    = true;
  Serial.printf("[FWUPD] Update available: %s -> %s\n", FIRMWARE_VERSION, fwAvailableVersion.c_str());
}

// ================================================================
//  Non-blocking download: this board's own release asset -> SD card
//  Driven one bounded step at a time by fwDownloadTick(), called every
//  loop() iteration, so server.handleClient() keeps servicing the Web UI's
//  status-poll requests throughout a multi-megabyte transfer (mirrors the
//  coreOtaTick() mesh-push state machine above). The connect step (TLS
//  handshake + headers) still happens synchronously within a single tick
//  call -- only the body transfer is sliced across ticks.
// ================================================================
enum class FwDlPhase : uint8_t { Idle, Preparing, Connecting, Downloading, RetryWait };

static struct {
  FwDlPhase         phase          = FwDlPhase::Idle;
  WiFiClientSecure*  client        = nullptr;
  HTTPClient*        http          = nullptr;
  File               file;
  String             destPath;
  int                total         = 0;
  int                remaining     = 0;
  int                connectAttempt = 0;
  int                lastLoggedPct = 0;
  uint32_t           lastDataMs    = 0;
  uint32_t           prepStartMs   = 0;
  uint32_t           prepLastLogMs = 0;
  uint8_t            sessionAttempt = 0;  // whole-session (Preparing->...->fail) retry count
  uint32_t           retryAtMs      = 0;
} fwDl;

static const uint32_t FW_DL_STALL_MS = 20000;  // no body bytes at all for this long -> give up
static const int      FW_DL_CONNECT_MAX_ATTEMPTS = 3;

// Preparing-phase budget: loop() already stops scanning and WDGWars polling
// the instant fwDownloading goes true (see loop() below), but an async scan
// already in flight, or deferred lwIP/mbedTLS cleanup from a just-finished
// Check-for-Update call, both take a moment to actually release their
// memory back to the DMA pool. Rather than race that cleanup the way a
// direct-to-Connecting start used to, wait it out first: a mandatory
// minimum settle so background work has time to actually wind down, then
// keep polling (up to a hard cap, so a boot session where the pool truly
// never recovers doesn't hang forever) before proceeding regardless.
// Field testing showed dmaLargest climbing steadily well past the old 8 s
// cap -- 25588 -> 36852 -> 38900 bytes over 8 s and still rising, just shy
// of FW_DL_DMA_HEALTHY_THRESHOLD -- so the old cap was giving up right
// before the pool would have cleared the threshold. 30 s gives that slow
// deferred cleanup much more room to finish.
static const uint32_t FW_DL_PREP_MIN_MS = 3000;
static const uint32_t FW_DL_PREP_MAX_MS = 30000;

// Below this MALLOC_CAP_DMA largest-free-block reading, a failed connect to
// this download target is treated as resource-constrained rather than a
// plain network hiccup. Chosen from instrumented testing: a pristine,
// never-used-this-boot pool starts above 60000, but the very first TLS
// connection of a boot session appears to permanently retain some
// mbedTLS/esp-aes session-level allocation afterward -- every reading taken
// for the rest of that boot session has topped out at ~34804-36852, no
// matter how long the device sits idle. This download target has never
// succeeded even starting from that ~36852 "recovered" ceiling, so anything
// below this threshold is presumed insufficient for its particular TLS
// handshake (a larger cert chain than the lightweight GitHub API check,
// via the objects.githubusercontent.com redirect).
static const uint32_t FW_DL_DMA_HEALTHY_THRESHOLD = 40000;

// Whole-session auto-retry budget: this flow is driven from the TFT/button
// (or a Web UI modal the operator may not be watching), so nobody is
// necessarily there to notice a failure and manually click/press "try
// again". Rather than strand the screen on a dead error, keep retrying the
// entire Preparing->Connecting sequence on its own -- the DMA pool has been
// observed to vary by several KB over tens of seconds (not just monotonic
// recovery), so a later attempt genuinely has a different chance than the
// one that just failed. Capped so a truly persistent problem (no WiFi, SD
// gone, GitHub down) eventually gives up instead of looping forever.
static const uint32_t FW_DL_SESSION_RETRY_DELAY_MS = 8000;
static const uint8_t  FW_DL_SESSION_RETRY_MAX      = 12;

static void fwDownloadCleanup() {
  if (fwDl.file) fwDl.file.close();
  if (fwDl.http) { fwDl.http->end(); delete fwDl.http; fwDl.http = nullptr; }
  if (fwDl.client) { delete fwDl.client; fwDl.client = nullptr; }
}

static void fwDownloadFail(const String& err) {
  fwLogAppend("ERROR: %s", err.c_str());
  fwDownloadCleanup();
  if (sdOk && fwDl.destPath.length() && SD.exists(fwDl.destPath)) SD.remove(fwDl.destPath);
  fwDownloadError = err;

  fwDl.sessionAttempt++;
  if (fwDl.sessionAttempt < FW_DL_SESSION_RETRY_MAX) {
    // fwDownloading stays true throughout the wait -- the TFT "Updating"
    // screen and any open Web UI modal keep showing live status instead of
    // reverting to "press button to download" and silently waiting for a
    // manual retry that may never come.
    fwDownloadStage = "Failed -- retrying...";
    fwDl.retryAtMs  = millis() + FW_DL_SESSION_RETRY_DELAY_MS;
    fwDl.prepLastLogMs = 0;  // force an immediate countdown update on the next tick
    fwDl.phase      = FwDlPhase::RetryWait;
    fwLogAppend("Retrying in %lus (session attempt %d/%d)",
                (unsigned long)(FW_DL_SESSION_RETRY_DELAY_MS / 1000),
                (int)fwDl.sessionAttempt + 1, FW_DL_SESSION_RETRY_MAX);
    return;
  }

  fwDownloadStage = "Failed";
  fwDownloading   = false;
  fwDownloaded    = false;
  fwDl.phase      = FwDlPhase::Idle;
  fwLogAppend("Giving up after %d attempts -- press the download button/control again to retry", FW_DL_SESSION_RETRY_MAX);
}

static void fwDownloadStart() {
  if (fwDownloading) return;
  if (!fwUpdateAvailable || fwOwnAssetUrl.length() == 0) {
    fwDownloadError = "No update available";
    return;
  }
  if (WiFi.status() != WL_CONNECTED) {
    fwDownloadError = "STA WiFi not connected";
    return;
  }
  if (!sdOk) {
    fwDownloadError = "SD not available";
    return;
  }

  fwLog                = "";
  fwDownloadError      = "";
  fwDownloadedBytes    = 0;
  fwDownloadTotalBytes = 0;
  fwDownloaded         = false;
  fwDownloadStage      = "Pausing background tasks...";
  fwDl.destPath        = String("/Firmware/") + FW_OWN_ASSET;
  fwDl.connectAttempt  = 0;
  fwDl.sessionAttempt  = 0;
  fwDl.prepStartMs     = millis();
  fwDl.prepLastLogMs   = 0;
  fwDl.phase           = FwDlPhase::Preparing;
  fwDownloading        = true;
  fwLogAppend("Starting download of %s -- pausing scanning and background jobs first", FW_OWN_ASSET);
}

static void fwDownloadCancel() {
  if (!fwDownloading) return;
  fwLogAppend("Download cancelled by user");
  fwDownloadCleanup();
  if (sdOk && fwDl.destPath.length() && SD.exists(fwDl.destPath)) SD.remove(fwDl.destPath);
  fwDownloadStage = "Cancelled";
  fwDownloadError = "";
  fwDownloading   = false;
  fwDownloaded    = false;
  fwDl.phase      = FwDlPhase::Idle;
}

// Deletes every file in /Firmware/ on the SD card -- this board's own
// downloaded-but-not-yet-installed image, plus anything staged for the mesh
// Core-push engine. Lets the operator clear out a stuck/partial download (or
// just reclaim SD space) without needing to pull the card. Refuses while a
// download or install is actively using that directory so a file can't be
// yanked out from under an in-flight write/flash.
static bool fwClearFirmwareFolder() {
  if (fwDownloading || fwInstalling) return false;
  if (!sdOk) return false;

  File dir = SD.open("/Firmware");
  if (!dir) return true;              // directory doesn't exist -- nothing to clear
  if (!dir.isDirectory()) { dir.close(); return true; }

  uint32_t removed = 0;
  File f = dir.openNextFile();
  while (f) {
    bool   isDir = f.isDirectory();
    String path  = normalizeSdPath("/Firmware", f.name());
    f.close();
    if (!isDir && SD.remove(path)) removed++;
    f = dir.openNextFile();
  }
  dir.close();

  fwDownloaded         = false;
  fwDownloadedBytes    = 0;
  fwDownloadTotalBytes = 0;
  fwDownloadStage      = "";
  fwLogAppend("Cleared %u staged firmware file(s) from /Firmware", (unsigned)removed);
  return true;
}

static void fwDownloadTick() {
  if (!fwDownloading) return;

  switch (fwDl.phase) {
    case FwDlPhase::RetryWait: {
      // Holding pattern after a failed whole-session attempt -- see
      // fwDownloadFail() for why this auto-retries instead of stopping.
      // Updates the visible countdown at most once a second.
      uint32_t nowMs = millis();
      uint32_t remainMs = (nowMs < fwDl.retryAtMs) ? (fwDl.retryAtMs - nowMs) : 0;
      if (fwDl.prepLastLogMs == 0 || nowMs - fwDl.prepLastLogMs >= 1000) {
        fwDl.prepLastLogMs = nowMs;
        fwDownloadStage = "Failed -- retrying in " + String((remainMs + 999) / 1000) + "s...";
      }
      if (nowMs >= fwDl.retryAtMs) {
        fwLogAppend("Retrying now (session attempt %d/%d)...", (int)fwDl.sessionAttempt + 1, FW_DL_SESSION_RETRY_MAX);
        fwDl.connectAttempt = 0;
        fwDl.prepStartMs    = millis();
        fwDl.prepLastLogMs  = 0;
        fwDownloadStage     = "Pausing background tasks...";
        fwDl.phase          = FwDlPhase::Preparing;
      }
      break;
    }

    case FwDlPhase::Preparing: {
      // loop() already forces scanning/WDGWars off the instant fwDownloading
      // went true above -- this phase just waits for that to actually take
      // effect (an async scan already in flight, or deferred lwIP/mbedTLS
      // cleanup from whatever TLS session ran right before this button
      // press) before spending the DMA pool on a new TLS handshake. Polls
      // at most once a second so the modal shows real elapsed time instead
      // of sitting unchanged on "Starting...".
      uint32_t elapsed = millis() - fwDl.prepStartMs;
      if (elapsed == 0 || millis() - fwDl.prepLastLogMs >= 1000) {
        fwDl.prepLastLogMs = millis();
        uint32_t dmaLargest = heap_caps_get_largest_free_block(MALLOC_CAP_DMA);
        fwDownloadStage = "Waiting for memory to free up (" + String(elapsed / 1000) + "s)...";
        fwLogAppend("Settling -- dmaLargest=%u (%lus elapsed)", (unsigned)dmaLargest, (unsigned long)(elapsed / 1000));
      }

      bool minElapsed = elapsed >= FW_DL_PREP_MIN_MS;
      bool timedOut   = elapsed >= FW_DL_PREP_MAX_MS;
      uint32_t dmaLargestNow = heap_caps_get_largest_free_block(MALLOC_CAP_DMA);
      bool healthy = dmaLargestNow >= FW_DL_DMA_HEALTHY_THRESHOLD;

      if (minElapsed && (healthy || timedOut)) {
        if (healthy) {
          fwLogAppend("Memory looks healthy (dmaLargest=%u) -- connecting", (unsigned)dmaLargestNow);
        } else {
          fwLogAppend("Memory still low (dmaLargest=%u) after %lus -- connecting anyway",
                      (unsigned)dmaLargestNow, (unsigned long)(FW_DL_PREP_MAX_MS / 1000));
        }
        fwDl.connectAttempt = 0;
        fwDownloadStage     = "Connecting...";
        fwDl.phase          = FwDlPhase::Connecting;
      }
      break;
    }

    case FwDlPhase::Connecting: {
      fwDl.connectAttempt++;
      fwLogAppend("Connecting (attempt %d/%d)...", fwDl.connectAttempt, FW_DL_CONNECT_MAX_ATTEMPTS);

      bool   connectFailed = false;
      bool   dmaFailure    = false;
      String failReason;

      // Manually handle GitHub's one-hop redirect to
      // objects.githubusercontent.com instead of HTTPC_FORCE_FOLLOW_REDIRECTS.
      // That mode closes the first (github.com) TLS session and immediately
      // opens the second (heavier) one back-to-back with no settle time.
      // Instrumented testing showed this redirect target can fail to
      // allocate memory for RSA certificate verification (-17040: RSA
      // public key operation / BIGNUM allocation failure) even starting
      // from an otherwise healthy pool -- giving the first connection's
      // just-freed resources a brief moment to actually return to the
      // MALLOC_CAP_DMA pool before opening the second TLS session is the
      // one lever this manual approach can pull that FORCE_FOLLOW_REDIRECTS
      // cannot.
      String targetUrl = fwOwnAssetUrl;
      for (int hop = 0; hop < 2; hop++) {
        fwDl.client = new WiFiClientSecure();
        fwDl.client->setInsecure();
        fwDl.http = new HTTPClient();
        fwDl.http->setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);  // handled manually here
        // HTTPClient::GET() below is a blocking call -- loop() (and therefore
        // server.handleClient(), and the Web UI's progress poll) is frozen
        // for however long it takes to resolve. A real DMA-alloc failure
        // surfaces almost instantly, but a genuine network hang would
        // otherwise freeze the Web UI for the full timeout, twice per
        // attempt (two hops) -- kept short so a stuck connection doesn't
        // leave the modal looking dead for too long.
        fwDl.http->setTimeout(10000);

        if (!fwDl.http->begin(*fwDl.client, targetUrl)) {
          connectFailed = true;
          failReason    = "HTTP begin failed";
          break;
        }

        int code = fwDl.http->GET();
        bool isRedirect = (code == HTTP_CODE_MOVED_PERMANENTLY || code == HTTP_CODE_FOUND ||
                           code == HTTP_CODE_SEE_OTHER || code == HTTP_CODE_TEMPORARY_REDIRECT);

        if (isRedirect && hop == 0) {
          String location = fwDl.http->getLocation();
          fwDl.http->end();
          delete fwDl.http;   fwDl.http   = nullptr;
          delete fwDl.client; fwDl.client = nullptr;
          if (location.length() == 0) {
            connectFailed = true;
            failReason    = "Redirect with no Location header";
            break;
          }
          targetUrl = location;
          fwLogAppend("Redirected to CDN -- settling before second connection");
          yield();
          delay(400);
          continue;  // second hop, fresh client/http against the CDN URL
        }

        if (code != HTTP_CODE_OK) {
          connectFailed = true;
          if (code < 0) {
            char errBuf[100];
            int sslErr = fwDl.client->lastError(errBuf, sizeof(errBuf));
            failReason = "HTTP " + String(code) + " (TLS " + String(sslErr) + ": " + errBuf + ")";
            // DMA-pool pressure against this specific target does NOT
            // reliably surface as any one consistent mbedTLS error code --
            // observed in the field as -32512 (MBEDTLS_ERR_SSL_ALLOC_FAILED),
            // -29312 ("connection indicated an EOF"), -1 ("Generic error",
            // with an explicit "esp-aes: Failed to allocate memory" driver
            // log alongside it), and -17040 (RSA public key operation /
            // BIGNUM allocation failure) across different attempts and
            // sessions. Rather than guess at which mbedTLS codes mean the
            // same thing, classify directly from the measured pool size
            // instead -- see FW_DL_DMA_HEALTHY_THRESHOLD above.
            dmaFailure = (heap_caps_get_largest_free_block(MALLOC_CAP_DMA) < FW_DL_DMA_HEALTHY_THRESHOLD);
            fwLogDmaHeap("Download connect fail");
          } else {
            failReason = "HTTP " + String(code);
          }
          break;
        }

        break;  // success -- fwDl.client/fwDl.http stay alive for the Downloading phase
      }

      if (connectFailed) {
        fwLogAppend("%s -- attempt %d/%d", failReason.c_str(), fwDl.connectAttempt, FW_DL_CONNECT_MAX_ATTEMPTS);
        fwDownloadCleanup();
        if (dmaFailure) {
          fwDownloadFail("Low memory (TLS alloc failed) -- wait a few seconds and try again");
          return;
        }
        if (fwDl.connectAttempt < FW_DL_CONNECT_MAX_ATTEMPTS) {
          delay(500);
          return;  // retry next tick, phase stays Connecting
        }
        fwDownloadFail(failReason);
        return;
      }

      int len = fwDl.http->getSize();
      if (len <= 0) { fwDownloadFail("Unknown content length"); return; }

      digitalWrite(PINS.tft_cs, HIGH);
      if (!SD.exists("/Firmware")) SD.mkdir("/Firmware");
      if (SD.exists(fwDl.destPath)) SD.remove(fwDl.destPath);  // delete before write
      fwDl.file = SD.open(fwDl.destPath, FILE_WRITE);
      if (!fwDl.file) { fwDownloadFail("Cannot open destination file"); return; }

      fwDl.total               = len;
      fwDl.remaining           = len;
      fwDl.lastLoggedPct       = 0;
      fwDl.lastDataMs          = millis();
      fwDownloadTotalBytes     = (uint32_t)len;
      fwDownloadStage          = "Downloading...";
      fwLogAppend("Connected -- downloading %d bytes", len);
      fwDl.phase               = FwDlPhase::Downloading;
      break;
    }

    case FwDlPhase::Downloading: {
      if (!fwDl.http->connected() && fwDl.remaining > 0) {
        fwDownloadFail("Connection lost");
        return;
      }
      WiFiClient* stream = fwDl.http->getStreamPtr();
      size_t avail = stream->available();
      if (avail == 0) {
        if (millis() - fwDl.lastDataMs > FW_DL_STALL_MS) fwDownloadFail("Download stalled");
        return;  // nothing ready yet -- try again next tick
      }

      uint8_t buf[2048];
      int want = (int)((avail > sizeof(buf)) ? sizeof(buf) : avail);
      int n = stream->readBytes(buf, want);
      if (n <= 0) return;
      digitalWrite(PINS.tft_cs, HIGH);
      if (fwDl.file.write(buf, n) != (size_t)n) { fwDownloadFail("SD write failed"); return; }

      fwDl.remaining    -= n;
      fwDownloadedBytes += (uint32_t)n;
      fwDl.lastDataMs    = millis();

      int pct = (fwDl.total > 0) ? (int)(((uint64_t)fwDownloadedBytes * 100) / fwDl.total) : 0;
      if (pct >= fwDl.lastLoggedPct + 10) {
        fwDl.lastLoggedPct = (pct / 10) * 10;
        fwLogAppend("Downloaded %d%% (%u / %u bytes)", pct, (unsigned)fwDownloadedBytes, (unsigned)fwDl.total);
      }

      if (fwDl.remaining <= 0) {
        fwDl.file.close();
        fwDl.http->end();
        delete fwDl.http;   fwDl.http   = nullptr;
        delete fwDl.client; fwDl.client = nullptr;
        fwDownloading   = false;
        fwDownloaded    = true;
        fwDownloadStage = "Downloaded -- ready to install";
        fwLogAppend("Download complete (%u bytes)", (unsigned)fwDownloadedBytes);
        fwDl.phase = FwDlPhase::Idle;
      }
      break;
    }

    default:
      break;
  }
}

// Reads the already-downloaded SD file back in and flashes it via Update.h.
// Purely local (SD -> flash), no network -- a couple of seconds for a
// ~1.3MB image, so unlike the download step this stays a simple blocking
// call.
static bool fwInstallFromSD() {
  if (fwInstalling) return false;
  if (!fwDownloaded) {
    fwInstallError = "No firmware downloaded";
    return false;
  }
  if (!sdOk) {
    fwInstallError = "SD not available";
    return false;
  }

  fwInstalling   = true;
  fwInstallError = "";

  String path = String("/Firmware/") + FW_OWN_ASSET;
  digitalWrite(PINS.tft_cs, HIGH);
  File f = SD.open(path, FILE_READ);
  if (!f) {
    fwInstallError = "Cannot open downloaded file";
    fwLogAppend("ERROR: %s", fwInstallError.c_str());
    fwInstalling = false;
    return false;
  }

  size_t size = f.size();
  if (size == 0) {
    f.close();
    fwInstallError = "Downloaded file is empty";
    fwLogAppend("ERROR: %s", fwInstallError.c_str());
    fwInstalling = false;
    return false;
  }

  if (!Update.begin(size, U_FLASH)) {
    fwInstallError = String("Update.begin failed: ") + Update.errorString();
    fwLogAppend("ERROR: %s", fwInstallError.c_str());
    f.close();
    fwInstalling = false;
    return false;
  }

  fwLogAppend("Flashing %u bytes...", (unsigned)size);
  uint8_t buf[1024];
  size_t  written = 0;
  bool    writeFailed = false;
  while (f.available()) {
    int n = f.read(buf, sizeof(buf));
    if (n <= 0) break;
    if (Update.write(buf, n) != (size_t)n) { writeFailed = true; break; }
    written += (size_t)n;
    yield();
  }
  f.close();

  if (writeFailed || written != size) {
    fwInstallError = "Flash write failed";
    fwLogAppend("ERROR: %s", fwInstallError.c_str());
    Update.abort();
    fwInstalling = false;
    return false;
  }
  if (!Update.end(true)) {
    fwInstallError = String("Update.end failed: ") + Update.errorString();
    fwLogAppend("ERROR: %s", fwInstallError.c_str());
    fwInstalling = false;
    return false;
  }

  fwLogAppend("Flash OK -- rebooting");
  Serial.println("[FWUPD] Flash OK -- restarting");
  closeLogFile();
  delay(500);
  ESP.restart();
  return true;  // unreached
}

static bool fwDownloadToSD(const String& url, const String& destPath) {
  if (url.length() == 0) return false;
  if (!sdOk) return false;

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  http.setTimeout(20000);
  if (!http.begin(client, url)) return false;

  // Retry transient TLS connect failures -- same esp-aes/DMA-descriptor
  // allocation pressure the test-key/token functions guard against can hit
  // here too, especially since this is often one of several back-to-back
  // HTTPS connections during a single "Install Update" pass. HTTPClient::
  // connect() re-attempts _client->connect() on every call as long as the
  // previous attempt left it disconnected (returnError() stops the client
  // on failure), so simply calling GET() again is sufficient -- no need to
  // http.end()/begin() between attempts.
  int code = -1;
  for (int attempt = 1; attempt <= 3; attempt++) {
    code = http.GET();
    if (code == HTTP_CODE_OK) break;
    if (code < 0) {
      char errBuf[100];
      int sslErr = client.lastError(errBuf, sizeof(errBuf));
      Serial.printf("[FWUPD] Stage download (%s) HTTP %d (TLS %d: %s) -- attempt %d/3\n",
                    destPath.c_str(), code, sslErr, errBuf, attempt);
      if (attempt < 3) { delay(500); yield(); }
    } else {
      Serial.printf("[FWUPD] Stage download (%s) HTTP %d\n", destPath.c_str(), code);
      break;  // a real HTTP response was received -- retrying won't change it
    }
  }
  if (code != HTTP_CODE_OK) {
    http.end();
    return false;
  }

  int len = http.getSize();
  if (len <= 0) { http.end(); return false; }

  digitalWrite(PINS.tft_cs, HIGH);
  if (!SD.exists("/Firmware")) SD.mkdir("/Firmware");
  if (SD.exists(destPath)) SD.remove(destPath);  // delete before write

  File f = SD.open(destPath, FILE_WRITE);
  if (!f) { http.end(); return false; }

  WiFiClient* stream = http.getStreamPtr();
  uint8_t buf[1024];
  int remaining = len;
  bool ok = true;
  while (http.connected() && remaining > 0) {
    size_t avail = stream->available();
    if (avail) {
      int want = (int)((avail > sizeof(buf)) ? sizeof(buf) : avail);
      int n = stream->readBytes(buf, want);
      if (n <= 0) break;
      if (f.write(buf, n) != (size_t)n) { ok = false; break; }
      remaining -= n;
    } else {
      delay(2);
    }
    yield();
  }
  f.close();
  http.end();

  if (!ok || remaining > 0) { SD.remove(destPath); return false; }
  return true;
}

static bool fwHasNodeAsset() {
  return fwOtherAssetUrls[FW_NODE_ASSET_IDX].length() > 0;
}

// "Store Piglet Node Firmware": stages /Firmware/PigletNode.ota.bin from the
// latest GitHub release so headless PigletNode nodes can be pushed updates
// over ESP-NOW, independent of whether this board's own firmware is current.
static bool fwStageNodeFirmware() {
  if (fwStagingNode) return false;
  if (WiFi.status() != WL_CONNECTED) {
    fwStageNodeError = "STA WiFi not connected";
    return false;
  }

  fwStagingNode    = true;
  fwStageNodeError = "";

  if (!fwHasNodeAsset()) {
    // Not resolved yet -- e.g. this board is already on the latest release,
    // so no Web UI "Check for Update" click has happened this session.
    fwCheckForUpdate();
  }

  bool ok = false;
  if (!fwHasNodeAsset()) {
    fwStageNodeError = "PigletNode asset not found in latest release";
  } else {
    String dest = String("/Firmware/") + FW_OTHER_ASSETS[FW_NODE_ASSET_IDX];
    ok = fwDownloadToSD(fwOtherAssetUrls[FW_NODE_ASSET_IDX], dest);
    if (!ok) fwStageNodeError = "Download failed";
  }

  fwStagingNode = false;
  Serial.printf("[FWUPD] Store Piglet Node Firmware: %s\n", ok ? "OK" : fwStageNodeError.c_str());
  return ok;
}

static void handleUpdateStatus() {
  // fwLog can hold up to FW_LOG_MAX (2048) bytes of text -- size the
  // document well above that plus the other fields and JSON overhead.
  JsonDocument doc(psramJsonAllocator());
  doc["checked"]          = fwUpdateChecked;
  doc["available"]        = fwUpdateAvailable;
  doc["currentVersion"]   = FIRMWARE_VERSION;
  doc["availableVersion"] = fwAvailableVersion;
  doc["notes"]            = fwReleaseNotes;
  doc["checkError"]       = fwCheckError;

  doc["downloading"]      = fwDownloading;
  doc["downloaded"]       = fwDownloaded;
  doc["downloadStage"]    = fwDownloadStage;
  doc["downloadedBytes"]  = fwDownloadedBytes;
  doc["totalBytes"]       = fwDownloadTotalBytes;
  doc["downloadError"]    = fwDownloadError;

  doc["installing"]       = fwInstalling;
  doc["installError"]     = fwInstallError;

  doc["log"]              = fwLog;

  doc["nodeAssetKnown"]   = fwHasNodeAsset();
  doc["stagingNode"]      = fwStagingNode;
  doc["stageNodeError"]   = fwStageNodeError;

  String out;
  out.reserve(measureJson(doc) + 1);
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

static void handleUpdateCheck() {
  if (WiFi.status() != WL_CONNECTED) {
    server.send(400, "application/json", "{\"ok\":false,\"message\":\"STA WiFi not connected\"}");
    return;
  }
  fwCheckForUpdate();

  bool ok = (fwCheckError.length() == 0);
  JsonDocument doc(psramJsonAllocator());
  doc["ok"]               = ok;
  doc["message"]          = fwCheckError;
  doc["available"]        = fwUpdateAvailable;
  doc["availableVersion"] = fwAvailableVersion;

  String out;
  out.reserve(measureJson(doc) + 1);
  serializeJson(doc, out);
  server.send(ok ? 200 : 502, "application/json", out);
}

static void handleUpdateDownloadStart() {
  fwDownloadStart();
  // fwDownloadStart() is synchronous but non-blocking -- it only validates
  // preconditions and arms the state machine fwDownloadTick() drives from
  // loop(). fwDownloading is true here if a transfer is now (still) in
  // progress; otherwise fwDownloadError holds the rejection reason.
  bool ok = fwDownloading;

  JsonDocument doc(psramJsonAllocator());
  doc["ok"]    = ok;
  doc["error"] = fwDownloadError;

  String out;
  out.reserve(measureJson(doc) + 1);
  serializeJson(doc, out);
  server.send(ok ? 200 : 400, "application/json", out);
}

static void handleUpdateDownloadCancel() {
  fwDownloadCancel();
  server.send(200, "text/plain", "OK");
}

// Deletes every file currently staged in /Firmware/ (this board's own
// downloaded-but-not-yet-installed image, plus anything staged for the mesh
// Core-push engine) -- lets the operator clear a stuck/partial download or
// just reclaim SD space without pulling the card.
static void handleUpdateClearFirmware() {
  bool ok = fwClearFirmwareFolder();

  JsonDocument doc(psramJsonAllocator());
  doc["ok"] = ok;
  if (!ok) {
    doc["error"] = (fwDownloading || fwInstalling)
      ? "Cannot clear while a download or install is in progress"
      : "SD not available";
  }

  String out;
  out.reserve(measureJson(doc) + 1);
  serializeJson(doc, out);
  server.send(ok ? 200 : 400, "application/json", out);
}

static void handleUpdateInstallFromSD() {
  // Blocking, but purely local (SD -> flash, no network) so it's quick. On
  // success this calls ESP.restart() internally and never returns -- a
  // response is only sent here if it failed before flashing began.
  bool ok = fwInstallFromSD();

  JsonDocument doc(psramJsonAllocator());
  doc["ok"]    = ok;
  doc["error"] = fwInstallError;

  String out;
  out.reserve(measureJson(doc) + 1);
  serializeJson(doc, out);
  server.send(ok ? 200 : 500, "application/json", out);
}

static void handleUpdateStageNode() {
  if (WiFi.status() != WL_CONNECTED) { server.send(400, "text/plain", "STA WiFi not connected"); return; }

  bool ok = fwStageNodeFirmware();
  if (ok) coreOtaPreload();  // reload PSRAM images so the new file is usable without a reboot

  JsonDocument doc(psramJsonAllocator());
  doc["ok"]    = ok;
  doc["error"] = fwStageNodeError;

  String out;
  out.reserve(measureJson(doc) + 1);
  serializeJson(doc, out);
  server.send(ok ? 200 : 500, "application/json", out);
}

// ---- Manual per-node channel-range overrides (Web UI) ----
static bool webMacParse(const String& s, uint8_t* macOut) {
  unsigned int v[6];
  if (sscanf(s.c_str(), "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) return false;
  for (int i = 0; i < 6; i++) macOut[i] = (uint8_t)v[i];
  return true;
}

static void handleMeshChannelOverridesGet() {
  JsonDocument doc(psramJsonAllocator());
  JsonArray arr = doc.createNestedArray("overrides");
  uint8_t count = meshChOverrideGetCount();
  for (uint8_t i = 0; i < count; i++) {
    uint8_t mac[6]; uint16_t chStart, chEnd;
    if (!meshChOverrideGetAt(i, mac, chStart, chEnd)) continue;
    JsonObject o = arr.createNestedObject();
    char macStr[18];
    snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    o["mac"]     = macStr;
    o["chStart"] = chStart;
    o["chEnd"]   = chEnd;
  }

  String out;
  out.reserve(measureJson(doc) + 1);
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

static void handleMeshChannelOverridesSet() {
  if (!server.hasArg("mac") || !server.hasArg("chStart") || !server.hasArg("chEnd")) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"Missing mac/chStart/chEnd\"}");
    return;
  }
  uint8_t mac[6];
  if (!webMacParse(server.arg("mac"), mac)) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"Bad MAC format\"}");
    return;
  }
  int chStart = server.arg("chStart").toInt();
  int chEnd   = server.arg("chEnd").toInt();
  bool ok = meshChOverrideSet(mac, chStart, chEnd);

  JsonDocument doc(psramJsonAllocator());
  doc["ok"] = ok;
  if (!ok) doc["error"] = "Invalid channel -- must match a supported 2.4/5GHz channel number";
  String out;
  out.reserve(measureJson(doc) + 1);
  serializeJson(doc, out);
  server.send(ok ? 200 : 400, "application/json", out);
}

static void handleMeshChannelOverridesClear() {
  if (!server.hasArg("mac")) { server.send(400, "text/plain", "Missing mac"); return; }
  uint8_t mac[6];
  if (!webMacParse(server.arg("mac"), mac)) { server.send(400, "text/plain", "Bad MAC format"); return; }
  bool ok = meshChOverrideClear(mac);
  server.send(ok ? 200 : 404, "text/plain", ok ? "OK" : "Not found");
}

// Fire-and-forget sink for Web UI button-press logging (see logBtn() in
// INDEX_HTML) -- makes field debugging able to see exactly which control
// was pressed and when, directly in the serial console.
static void handleUiLog() {
  String msg = server.hasArg("msg") ? server.arg("msg") : "(unknown)";
  Serial.printf("[UI] %s\n", msg.c_str());
  server.send(200, "text/plain", "OK");
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
  server.on("/wardrift/test",      HTTP_POST, handleWardriftTest);
  server.on("/wardrift/uploadAll", HTTP_POST, handleWardriftUploadAll);
  server.on("/reboot",          HTTP_POST, handleReboot);
  server.on("/uiLog",           HTTP_POST, handleUiLog);
  server.on("/cleanup",         HTTP_POST, handleCleanup);
  server.on("/deleteAll",       HTTP_POST, handleDeleteAll);
  server.on("/mesh/nodes.json", handleMeshNodes);
  server.on("/mesh/ota/start",  HTTP_POST, handleMeshOtaStart);
  server.on("/mesh/ota/cancel", HTTP_POST, handleMeshOtaCancel);
  server.on("/update/status",         handleUpdateStatus);
  server.on("/update/check",          HTTP_POST, handleUpdateCheck);
  server.on("/update/downloadStart",  HTTP_POST, handleUpdateDownloadStart);
  server.on("/update/downloadCancel", HTTP_POST, handleUpdateDownloadCancel);
  server.on("/update/clearFirmware",  HTTP_POST, handleUpdateClearFirmware);
  server.on("/update/installFromSD",  HTTP_POST, handleUpdateInstallFromSD);
  server.on("/update/stageNode",      HTTP_POST, handleUpdateStageNode);
  server.on("/mesh/channelOverrides",       HTTP_GET,  handleMeshChannelOverridesGet);
  server.on("/mesh/channelOverrides",       HTTP_POST, handleMeshChannelOverridesSet);
  server.on("/mesh/channelOverrides/clear", HTTP_POST, handleMeshChannelOverridesClear);
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
    // wifioff=false -- see the matching comment in handleStaTransitions()
    // below for why tearing down the STA netif here can crash the
    // still-running WebServer.
    WiFi.disconnect(false, true); delay(50);
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
    // wifioff MUST be false here: true calls STA.end(), which tears down
    // the STA network interface while the WebServer (server.handleClient()
    // runs every loop() iteration and never stops) may still have a client
    // connected through it. That previously crashed with a Guru Meditation
    // Instruction access fault (MEPC=0) inside esp_pbuf_free() ->
    // esp_netif_free_rx_buffer() -- lwIP freeing a receive buffer that
    // referenced the just-destroyed interface.
    WiFi.disconnect(false, false);  // stop reconnect; eraseap=false keeps NVS credentials
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

// ---- Home network auto-return: connect + upload mid-drive ----

// Minimum time between connection attempts triggered by seeing the home
// SSID in scan results -- avoids hammering connectSTA() every scan cycle
// (every ~1.5 s in aggressive mode) while lingering near the edge of range.
static const uint32_t HOME_RETURN_RETRY_MS = 30000UL;

static void checkHomeNetworkReturn() {
  if (!homeNetworkSeen) return;
  homeNetworkSeen = false;  // consume -- re-armed next time it's seen in a scan

  // Feature disabled via config -- toggleable, default on.
  if (!cfg.uploadOnNetworkSeen) return;

  // Already connected, or AP actively serving the config UI -- nothing to do.
  if (WiFi.status() == WL_CONNECTED) return;
  if (apWindowActive) return;

  static uint32_t lastAttemptMs = 0;
  if (lastAttemptMs != 0 && (millis() - lastAttemptMs) < HOME_RETURN_RETRY_MS) return;
  lastAttemptMs = millis();

  Serial.println("[WIFI] Home network seen while wardriving -- connecting to upload");

  bool staOk = connectSTA(12000);
  lastStaStatus = WiFi.status();

  if (!staOk) {
    Serial.println("[WIFI] Home network connect failed -- resuming wardriving");
    WiFi.setAutoReconnect(false);
    WiFi.persistent(false);
    // wifioff=false -- see the matching comment in handleStaTransitions()
    // for why tearing down the STA netif here can crash the still-running
    // WebServer.
    WiFi.disconnect(false, false);  // eraseap=false keeps NVS credentials
    delay(50);
    WiFi.mode(WIFI_STA);           // idle STA ready for scanning
    return;
  }

  // Connected -- shouldPauseScanning() now returns true so wardriving pauses
  // automatically; handleStaTransitions() resumes it once STA disconnects
  // (e.g. driving back out of home WiFi range).
  bool hasWigle = cfg.wigleBasicToken.length() > 0;
  bool hasWdg   = cfg.wdgwarsApiKey.length()   > 0;

  if (sdOk && (hasWigle || hasWdg) && cfg.maxBootUploads != 0) {
    Serial.print("[UPLOAD] Home network connected. Services: ");
    if (hasWdg)   Serial.print("WDGoWars ");
    if (hasWigle) Serial.print("WiGLE ");
    Serial.println();
    uploadAllPending(cfg.maxBootUploads);
  } else {
    Serial.println("[UPLOAD] Skipped (SD not ready, no tokens set, or maxBootUploads=0)");
  }
}

// Last-known GPS position — used when fix is temporarily lost so networks
// aren't logged at 0,0 (null island).
// Updated every loop() iteration (not just on scan) so position stays current
// even when driving through areas with no networks.
// Quality-gated: requires HDOP ≤ 10 and ≥ 3 satellites to prevent a brief
// low-quality re-acquisition from overwriting a good cached position.
static bool     lastGpsValid   = false;
static double   lastLat = 0, lastLon = 0, lastAlt = 0, lastAcc = 0;
static uint32_t lastGpsValidMs = 0;          // millis() when position was last cached
static const uint32_t GPS_CACHE_MAX_MS = 180000UL;  // discard cache after 3 min

// ---------------- Scan (2.4 + 5 GHz) ----------------
static void processScanResults(int n) {
  if (n <= 0) { WiFi.scanDelete(); return; }

  String firstSeen = iso8601NowUTC();
  double lat = 0, lon = 0, altM = 0, accM = 0;
  bool   havePosition = false;

  if (gpsHasFix && gpsCoordIsValid(gps.location.lat(), gps.location.lng())) {
    lat  = gps.location.lat();
    lon  = gps.location.lng();
    altM = gps.altitude.isValid() ? gps.altitude.meters() : 0.0;
    accM = gps.hdop.isValid()     ? gps.hdop.hdop()       : 0.0;
    havePosition = true;
    // lastLat/lastLon is maintained by loop() — no update here.
  } else if (lastGpsValid && (millis() - lastGpsValidMs) <= GPS_CACHE_MAX_MS &&
             gpsCoordIsValid(lastLat, lastLon)) {
    lat  = lastLat;
    lon  = lastLon;
    altM = lastAlt;
    accM = lastAcc;
    havePosition = true;
  }

  uint32_t wrote = 0;
  uint32_t skipped = 0;
  for (int i = 0; i < n; i++) {
    // Periodic cooperative yield so a large batch (dense-area scan) doesn't
    // monopolize the CPU between appendWigleRow()'s own flush-triggered yields.
    if ((i & 0x0F) == 15) yield();

    int ch = WiFi.channel(i);
    bool chUnknown = (ch == 0);
    bool is2g = (ch >= 1 && ch <= 14) || chUnknown;
    bool is5g = (ch >= 32 && ch <= 177);

    if (!is2g && !is5g) continue;

    String ssid = WiFi.SSID(i);
    String mac  = WiFi.BSSIDstr(i);
    int rssi = WiFi.RSSI(i);
    String authStr = authModeToString(WiFi.encryptionType(i));

    // Exact, case-sensitive match against the configured home network --
    // lets the device notice it has returned home mid-drive instead of
    // only checking at boot.
    if (!homeNetworkSeen && cfg.homeSsid.length() > 0 && ssid == cfg.homeSsid) {
      homeNetworkSeen = true;
    }

    if (is2g) networksFound2G++;
    else      networksFound5G++;

    // Never fabricate a position (0,0 or otherwise) -- if there's no
    // trustworthy fix or cached last-known-good location yet, skip logging
    // this network entirely rather than writing "Null Island". It'll be
    // logged on a later scan cycle once a real position is available.
    if (!havePosition) { skipped++; continue; }

    if (appendWigleRow(mac, ssid, authStr, firstSeen, ch, rssi, lat, lon, altM, accM)) wrote++;
  }

  WiFi.scanDelete();

  // Force flush after each scan batch so data reaches the SD card promptly.
  // Minimises data loss if the device loses power between scan cycles.
  if (wrote > 0 && sdOk && logFile) {
    digitalWrite(PINS.tft_cs, HIGH);
    logFile.flush();
  }

  if (wrote > 0) ledPulseGreen();
  if (skipped > 0) {
    Serial.printf("[SCAN] %u network(s) not logged -- no valid GPS position yet (never writes 0,0)\n", (unsigned)skipped);
  }
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

// On XIAO ESP32-C5 (the T-Dongle's chip), GPIO0 is the BOOT/flash-mode
// strapping pin. Although the T-Dongle's button is wired to GPIO28 (not
// GPIO0), this guard is kept for defense-in-depth consistency with the
// main Piglet firmware in case of future pin-map changes; set once near
// the end of setup().
static const uint32_t BTN_BOOT_SETTLE_MS = 1500;
static uint32_t btnBootSettleUntilMs = 0;

// Button: triple-tap on pig page (3) = twerk; single press = advance page;
//         2 s hold on mesh page (4) = toggle Core / Node; double press on
//         mesh page (4) = toggle the Wardrive Go BLE bridge.
static void pollButton() {
  static uint32_t pressStartMs  = 0;
  static bool     pressing      = false;
  static bool     longTriggered = false;
  // Multi-click detection -- sliding window: each new click only needs to
  // land within MULTI_MS of the PREVIOUS click, not of the very first one.
  // Anchoring the window to the first click made a 3-tap trigger (e.g. the
  // pig page's sasquatch easter egg) much harder to land than a 2-tap one,
  // since all 3 taps had to fit inside one fixed window measured from tap 1
  // -- a sliding window gives the same per-tap timing tolerance no matter
  // how many taps are in the sequence.
  static uint8_t  clickCount    = 0;
  static uint32_t lastClickMs   = 0;
  static const uint32_t MULTI_MS = 400;  // max gap between taps

  if (millis() < btnBootSettleUntilMs) return;

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
      uint32_t now = millis();
      if (clickCount > 0 && (now - lastClickMs) > MULTI_MS) {
        clickCount = 0;  // previous sequence timed out -- start a fresh one
      }
      clickCount++;
      lastClickMs = now;
    }
    pressing = false;
  }

  // Evaluate once the window has elapsed since the most recent click
  if (clickCount > 0 && (millis() - lastClickMs) > MULTI_MS) {
    if (clickCount >= 3 && currentPage == 3) {
      sasquatchStartTFT();
    } else if (clickCount == 2 && currentPage == 3) {
      pigTwerkStartTFT();
    } else if (clickCount >= 2 && currentPage == 4) {
      // Double press on Mesh Node/Core page -> toggle the Wardrive Go BLE
      // bridge immediately (mesh mode is already active on this page) and
      // persist the setting, so it also auto-starts on this page after a
      // reboot -- same effect as flipping the Web UI toggle and saving.
      cfg.wardriveGoBridge = !cfg.wardriveGoBridge;
      saveConfigToSD();
      if (cfg.wardriveGoBridge) wardriveGoBleBegin(meshCoreActive);
      else                      wardriveGoBleSuspend();
      Serial.print("[BTN] Double press -> Wardrive Go bridge ");
      Serial.println(cfg.wardriveGoBridge ? "ENABLED" : "DISABLED");
    } else if (currentPage == 4 && meshCoreActive && coreUpdatePromptStatus().promptActive) {
      // "Update Nodes through Core?" prompt showing -- a press here confirms
      // it instead of advancing to the next page, since the prompt itself is
      // using this page's normal single-press action.
      coreUpdatePromptConfirm();
      Serial.println("[BTN] Press -> mesh update prompt confirmed");
    } else if (currentPage == 0 && fwDownloaded && !fwDownloading && !fwInstalling) {
      // Download finished, status page is showing "Restart to install" --
      // a single press restarts now. The actual install runs automatically
      // at next boot (see setup()), not here, so it never competes with
      // anything else for memory.
      Serial.println("[BTN] Press -> restarting to install firmware update");
      ESP.restart();
    } else if (currentPage == 0 && fwUpdateAvailable && !fwDownloading && !fwDownloaded && !fwInstalling) {
      // Status page is showing the "Firmware update available" banner --
      // a single press starts the on-device download instead of advancing
      // pages. Driven entirely from the TFT/button, no Web UI involved, so
      // there's no browser polling traffic competing with the download's
      // TLS session for the same narrow DMA-capable heap pool.
      Serial.println("[BTN] Press -> starting on-device firmware download");
      fwDownloadStart();
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

  // Resume an on-device firmware update staged by a previous session: the
  // "Download & Install" on-device flow (status page button press) writes
  // this board's own release asset to /Firmware/ and asks the user to
  // restart rather than installing immediately, so the install itself never
  // competes with anything else for memory -- see updateTFT()/pollButton()
  // for the download/restart side of this flow. fwInstallFromSD() normally
  // requires fwDownloaded to already be true (set by this session's own
  // download); after a fresh boot that in-RAM flag is gone, so set it here
  // once the file's presence on SD is confirmed. A failed install
  // (corrupt/partial file) deletes it so a bad image can't block every boot.
  if (sdOk && SD.exists(String("/Firmware/") + FW_OWN_ASSET)) {
    Serial.println("[BOOT] Staged firmware update found -- installing...");
    tft.fillScreen(BLACK);
    tft.setTextColor(WHITE, BLACK);
    tftDrawCentered(20, "Installing", 2);
    tftDrawCentered(50, "firmware update...", 1);
    tftDrawCentered(85, "Do not power off", 1);
    fwDownloaded = true;
    if (!fwInstallFromSD()) {
      // fwInstallFromSD() only returns on failure -- success calls
      // ESP.restart() internally and never comes back here.
      Serial.printf("[BOOT] Staged firmware install failed: %s -- removing file\n", fwInstallError.c_str());
      SD.remove(String("/Firmware/") + FW_OWN_ASSET);
      fwDownloaded = false;
    }
  }

  // Mesh Core OTA images: load once now (PSRAM-resident) so that entering
  // Core mode later via long-press -- with no reboot in between -- already
  // has whatever /Firmware/*.ota.bin images are on the card ready to push.
  coreOtaPreload();

  // GPS (UART via Qwiic connector)
  // QWIIC pinout: RX = GPIO12 (from GPS TX), TX = GPIO11 (to GPS RX).
  // RX buffer 512 bytes: at 9600 baud the default 256-byte buffer fills in ~267ms.
  Serial.printf("[GPS] UART on RX=%d TX=%d Baud=%lu\n", PINS.gps_rx, PINS.gps_tx, (unsigned long)cfg.gpsBaud);
  GPSSerial.setRxBufferSize(512);
  GPSSerial.begin(cfg.gpsBaud, SERIAL_8N1, PINS.gps_rx, PINS.gps_tx);

  // GPS connection check — wait up to 2 s for any bytes from the module.
  // Diagnosis key:
  //   chars=0            -> RX not connected (GPIO12 not reaching GPS TX)
  //   chars>0, failed>0  -> baud rate mismatch or signal noise
  //   chars>0, failed=0  -> UART wired correctly; fix comes once outside with signal
  {
    Serial.println("[GPS] Checking wiring...");
    uint32_t gpsCheckEnd = millis() + 2000;
    while (millis() < gpsCheckEnd) {
      while (GPSSerial.available()) gps.encode(GPSSerial.read());
      delay(10);
    }
    uint32_t chars     = gps.charsProcessed();
    uint32_t sentences = gps.passedChecksum();
    uint32_t failed    = gps.failedChecksum();
    if (chars == 0) {
      Serial.printf("[GPS] WARNING: No data on RX=GPIO%d\n"
                    "[GPS]   -> Check GPS TX wire is on GPIO%d\n"
                    "[GPS]   -> Check GPS module is powered (3.3V on QWIIC)\n",
                    PINS.gps_rx, PINS.gps_rx);
    } else if (failed > sentences) {
      Serial.printf("[GPS] Data on RX but high checksum errors (chars=%lu ok=%lu fail=%lu)\n"
                    "[GPS]   -> Likely wrong baud rate (currently %lu) or signal noise\n",
                    chars, sentences, failed, (unsigned long)cfg.gpsBaud);
    } else {
      Serial.printf("[GPS] UART OK — chars=%lu sentences=%lu failed=%lu\n",
                    chars, sentences, failed);
    }
  }

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

  // Self-update check: boot-only, separate from the mid-drive "home network
  // seen" auto-upload path. Silent/non-fatal if offline.
  if (staOk) {
    fwCheckForUpdate();
  }

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
          f.setBufferSize(64);  // avoid the 4KB default stdio buffer -- see addDirFiles()
          String path = normalizeSdPath("/logs", f.name());
          if (path.endsWith(".csv")) toDelete.push_back(path);
          f.close(); f = root.openNextFile();
        }
        root.close();
      }
      for (const String& path : toDelete) {
        uint32_t sz = 0;
        if (!csvHasDataRows(path, &sz)) {
          Serial.printf("[CLEANUP] Empty CSV (%u bytes), deleting: %s\n", (unsigned)sz, pathBasename(path).c_str());
          if (!SD.remove(path)) Serial.printf("[CLEANUP] WARNING: SD.remove failed for %s -- file left in place\n", pathBasename(path).c_str());
          clearUploadState(pathBasename(path));
        }
      }
    }
    pruneUploadState();  // drop ledger entries for files no longer in /logs
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
      uploadAllPending(cfg.maxBootUploads);
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
        // Give any WDGoWars jobs queued during boot upload a brief chance to
        // resolve before we drop the STA connection they'd need to check on.
        wdgwarsDrainPendingJobs(5000);

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

  // Arm the button boot-settle guard now, right as loop() is about to start —
  // see pollButton() for the rationale.
  btnBootSettleUntilMs = millis() + BTN_BOOT_SETTLE_MS;

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

  // Update last-known-good position every loop — decoupled from scan results
  // so it stays current even when no networks are found.
  // Quality gate: HDOP ≤ 10 and ≥ 3 satellites guards against brief bad fixes
  // (e.g. re-acquisition after a tunnel) overwriting a good cached position.
  if (gpsHasFix) {
    float hdop = gps.hdop.isValid()       ? gps.hdop.hdop()           : 99.0f;
    int   sats = gps.satellites.isValid() ? (int)gps.satellites.value() : 0;
    if (hdop <= 10.0f && sats >= 3 && gpsCoordIsValid(gps.location.lat(), gps.location.lng())) {
      lastLat        = gps.location.lat();
      lastLon        = gps.location.lng();
      lastAlt        = gps.altitude.isValid() ? gps.altitude.meters() : 0.0;
      lastAcc        = hdop;
      lastGpsValid   = true;
      lastGpsValidMs = millis();
    }
  }

  // GPS health log every 10 s until fix acquired. Skipped during a firmware
  // download: each burst of Serial.printf() over the native USB-Serial/JTAG
  // CDC interface measurably dipped MALLOC_CAP_DMA largest-free-block in
  // field testing (the USB peripheral's own internal buffers compete for
  // the same DMA-capable heap pool TLS needs), right when that pool is
  // already under the most pressure.
  if (!gpsHasFix && !fwDownloading) {
    static uint32_t lastGpsDiagMs = 0;
    if (millis() - lastGpsDiagMs >= 10000) {
      lastGpsDiagMs = millis();
      uint32_t chars  = gps.charsProcessed();
      uint32_t ok     = gps.passedChecksum();
      uint32_t failed = gps.failedChecksum();
      int      sats   = gps.satellites.isValid() ? (int)gps.satellites.value() : 0;
      Serial.printf("[GPS] chars=%lu ok=%lu fail=%lu sats=%d fix=NO\n",
                    chars, ok, failed, sats);
      if (chars == 0)
        Serial.printf("[GPS]   No data — RX=GPIO%d not receiving. Check GPS TX wire.\n",
                      PINS.gps_rx);
      else if (failed > ok)
        Serial.printf("[GPS]   Checksum errors dominate — check baud rate (%lu bps)\n",
                      (unsigned long)cfg.gpsBaud);
    }
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
  if (!meshNodeActive && !meshCoreActive) {
    handleStaTransitions();
    checkHomeNetworkReturn();
  }

  // Check on any WDGoWars jobs queued during the last upload batch. This is
  // rate-limited internally to at most one short network round-trip per
  // call, so it never blocks scanning, GPS, or the display. Skipped while a
  // firmware download is in flight -- its own HTTPS connection would
  // otherwise compete with the download's TLS session for the same narrow
  // DMA-capable heap pool.
  if (!fwDownloading) wdgwarsServicePendingJobs();

  // Drive an in-progress firmware download one bounded step at a time. A
  // no-op unless the Web UI's "Download & Install Firmware" modal has
  // started a transfer.
  fwDownloadTick();

  // Scanning — mesh page handles its own logic; skip normal scan path
  if (currentPage == 4) {
    if (meshCoreActive) coreModeTick();
    else                nodeModeTick();
  } else {
    autoPaused = shouldPauseScanning();
    wifi_mode_t m = WiFi.getMode();
    bool apActive = (m == WIFI_AP || m == WIFI_AP_STA);
    // A firmware download holding a TLS session open fully stops scanning
    // (overriding even a user scan override) rather than just throttling
    // it, so the radio and heap aren't also servicing scan traffic while
    // the download needs every spare byte of the DMA-capable heap pool.
    bool allowScan = !fwDownloading && scanningEnabled && sdOk && !apActive && (userScanOverride || !autoPaused);
    if (allowScan) doScanOnce();
  }

  delay(10);
}
