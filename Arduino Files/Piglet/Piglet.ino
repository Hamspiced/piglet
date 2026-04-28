/*
 * XIAO ESP32S3 2.4GHz Wardriver (WiGLE CSV + GPS + SD + OLED + Web UI)
 *
 * Features:
 * - 2.4GHz WiFi scan -> WiGLE CSV on SD (append GPS per row)
 * - SSD1306 OLED status
 * - User button toggles scanning on/off
 * - Boot: start SoftAP (192.168.4.1), attempt STA to home WiFi
 * - If STA connects: upload previous CSV files to WiGLE using Basic token
 * - Web UI: browse/download/delete files, edit config (saved to SD)
 * - SoftAP only lasts 60 seconds; then wardrive begins
 *
 * NOTES:
 * - Pins below are for XIAO ESP32-S3 WITHOUT Sense expansion.
 * - WiGLE CSV format: WigleWifi-1.4 header + standard columns.
 */

#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include <sys/time.h>
#include <esp_sleep.h>

#if defined(CONFIG_IDF_TARGET_ESP32S3)
  #include <driver/rtc_io.h>
#endif

#include "Globals.h"
#include "Config.h"
#include "GPS.h"
#include "SDUtils.h"
#include "Display.h"
#include "WiFiManager.h"
#include "Scanner.h"
#include "WigleUpload.h"
#include "WebUI.h"
#include "MeshNode.h"

// -------- Battery Test (uncomment to enable) --------
#include "battery_test.h"

// ---------------- Deep Sleep ----------------
static const uint32_t LONG_PRESS_MS = 2000;

static void enterDeepSleep() {
  Serial.println("[SLEEP] Long press detected – entering deep sleep...");

  // Flush & close the active CSV log
  closeLogFile();

  // Disconnect WiFi cleanly
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);

  // Show message on OLED
  display.clearDisplay();
  display.setTextSize(2);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(16, 24);
  display.print("Sleep...");
  display.display();
  delay(600);

  // Configure wake source (button press = LOW on INPUT_PULLUP)
  #if defined(CONFIG_IDF_TARGET_ESP32S3)
    rtc_gpio_pullup_en((gpio_num_t)pins.btn);
    rtc_gpio_pulldown_dis((gpio_num_t)pins.btn);
    esp_sleep_enable_ext0_wakeup((gpio_num_t)pins.btn, 0);  // wake on LOW
  #elif defined(CONFIG_IDF_TARGET_ESP32C6) || defined(CONFIG_IDF_TARGET_ESP32C5)
    esp_deep_sleep_enable_gpio_wakeup(1ULL << pins.btn, ESP_GPIO_WAKEUP_GPIO_LOW);
  #else
    esp_sleep_enable_ext0_wakeup((gpio_num_t)pins.btn, 0);  // fallback
  #endif

  // Turn off display
  display.ssd1306_command(SSD1306_DISPLAYOFF);

  Serial.println("[SLEEP] Goodnight.");
  Serial.flush();

  esp_deep_sleep_start();  // never returns – wake triggers reset
}

// ---------------- Button ----------------
static void onPageChange(uint8_t oldPage, uint8_t newPage) {
  if (newPage == 3) {
    Serial.println("[PAGE] Entered pause page -> scanning paused");
  } else if (oldPage == 3) {
    Serial.println("[PAGE] Left pause page -> scanning resumed");
  }

  // Clear status-page pause when leaving page 0
  if (oldPage == 0 && statusPagePaused) {
    statusPagePaused = false;
    Serial.println("[PAGE] Left status page -> status pause cleared");
  }

  // Entering pig page -> reset animation position
  if (newPage == 4) {
    pig.x = 0;
    pig.dx = 1;
    pig.phase = 0;
  }

  // Mesh node page lifecycle
  if (newPage == 5) enterNodeMode();
  if (oldPage == 5) exitNodeMode();

  Serial.print("[PAGE] ");
  Serial.print(oldPage);
  Serial.print(" -> ");
  Serial.println(newPage);
}

static const uint32_t DOUBLE_PRESS_MS = 350;  // max gap between presses for double-press

static void pollButton() {
  static uint32_t lastDebounce = 0;
  static int lastState = HIGH;
  static bool latched = false;
  static uint32_t pressStartMs = 0;
  static bool longPressTriggered = false;

  // Double-press detection
  static uint8_t  clickCount = 0;
  static uint32_t firstClickMs = 0;

  int s = digitalRead(pins.btn);

  if (s != lastState) {
    lastDebounce = millis();
    lastState = s;
  }

  if ((millis() - lastDebounce) > 40) {
    if (s == LOW) {
      if (!latched) {
        pressStartMs = millis();
        longPressTriggered = false;
        latched = true;
      }
      // While held, check for long press
      if (latched && !longPressTriggered &&
          (millis() - pressStartMs >= LONG_PRESS_MS)) {
        longPressTriggered = true;
        clickCount = 0;  // cancel any pending click
        enterDeepSleep();  // never returns
      }
    } else {
      // Released
      if (latched && !longPressTriggered) {
        clickCount++;
        if (clickCount == 1) {
          firstClickMs = millis();
        }
      }
      latched = false;
      longPressTriggered = false;
    }
  }

  // Evaluate click count after double-press window expires
  if (clickCount > 0 && (millis() - firstClickMs) > DOUBLE_PRESS_MS) {
    if (clickCount >= 2 && currentPage == 0) {
      // Double press on status page -> toggle scan pause
      statusPagePaused = !statusPagePaused;
      Serial.print("[BTN] Double press -> status page scan ");
      Serial.println(statusPagePaused ? "PAUSED" : "RESUMED");
    } else {
      // Single press -> cycle page (once, regardless of extra clicks)
      uint8_t oldPage = currentPage;
      currentPage = (currentPage + 1) % PAGE_COUNT;
      onPageChange(oldPage, currentPage);
    }
    clickCount = 0;
  }
}

// ================================================================
//  setup()
// ================================================================
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println();
  Serial.printf("[BOOT] Reset reason: %d\n", (int)esp_reset_reason());

  esp_sleep_wakeup_cause_t wakeup = esp_sleep_get_wakeup_cause();
  if (wakeup == ESP_SLEEP_WAKEUP_EXT0 || wakeup == ESP_SLEEP_WAKEUP_GPIO) {
    Serial.println("[BOOT] Woke from deep sleep (button press)");
  }

  networksFound2G = 0;
  networksFound5G = 0;

  // --- Bootstrap pins by chip so we can bring up SD and read config ---
  pins = detectPinsByChip();
  Serial.print("[PINS] Bootstrap (chip-detected) pinmap: ");
  Serial.println(pins.name);

  Serial.println();
  Serial.println("=== Piglet Wardriver Boot ===");

  // =========================
  // Phase 1: SD bring-up using bootstrap pins (chip-detected)
  // =========================
  bool cfgLoaded = false;

  Serial.println("[SD] Initializing SPI + SD (bootstrap pins)...");
  Serial.print("[SD] SPI pins SCK=");
  Serial.print(pins.sd_sck);
  Serial.print(" MISO=");
  Serial.print(pins.sd_miso);
  Serial.print(" MOSI=");
  Serial.print(pins.sd_mosi);
  Serial.print(" CS=");
  Serial.println(pins.sd_cs);

  SPI.begin(pins.sd_sck, pins.sd_miso, pins.sd_mosi, pins.sd_cs);

  // Try SD at a reasonable speed first, then fall back slower
  sdOk = SD.begin(pins.sd_cs, SPI, 8000000);
  if (!sdOk) sdOk = SD.begin(pins.sd_cs, SPI, 4000000);

  Serial.print("[SD] SD.begin (bootstrap pins): ");
  Serial.println(sdOk ? "OK" : "FAIL");

  // Attempt to load config if SD came up on bootstrap pins
  if (sdOk) {
    cfgLoaded = loadConfigFromSD();
    Serial.print("[CFG] Import (bootstrap pins): ");
    Serial.println(cfgLoaded ? "OK" : "SKIPPED/FAIL");
  } else {
    Serial.println("[CFG] Import skipped (bootstrap SD FAIL)");
  }

  // =========================
  // Phase 2: Select FINAL pins from config + chip detect
  // =========================
  PinMap finalPins = pickPinsFromConfig();

  Serial.print("[PINS] Config board=");
  Serial.print(cfg.board);
  Serial.print(" -> final pinmap: ");
  Serial.println(finalPins.name);

  // Did anything relevant change?
  bool pinsChanged =
    (finalPins.sd_cs   != pins.sd_cs)   ||
    (finalPins.sd_sck  != pins.sd_sck)  ||
    (finalPins.sd_miso != pins.sd_miso) ||
    (finalPins.sd_mosi != pins.sd_mosi) ||
    (finalPins.sda     != pins.sda)     ||
    (finalPins.scl     != pins.scl)     ||
    (finalPins.gps_rx  != pins.gps_rx)  ||
    (finalPins.gps_tx  != pins.gps_tx)  ||
    (finalPins.btn     != pins.btn);

  // Commit final pins now
  pins = finalPins;

  if (pinsChanged || !sdOk) {
    Serial.println("[SD] (Re)initializing SPI + SD on FINAL pins...");

    SPI.begin(pins.sd_sck, pins.sd_miso, pins.sd_mosi, pins.sd_cs);

    sdOk = SD.begin(pins.sd_cs, SPI, 8000000);
    if (!sdOk) sdOk = SD.begin(pins.sd_cs, SPI, 4000000);

    Serial.print("[SD] SD.begin (final pins): ");
    Serial.println(sdOk ? "OK" : "FAIL");
  }

  // If SD is OK NOW but we couldn't load config earlier, load it now
  if (sdOk && !cfgLoaded) {
    cfgLoaded = loadConfigFromSD();
    Serial.print("[CFG] Import (final pins): ");
    Serial.println(cfgLoaded ? "OK" : "SKIPPED/FAIL");

    // Config might change cfg.board; re-pick pins if needed
    PinMap cfgPins = pickPinsFromConfig();
    bool pinsChangedAgain =
      (cfgPins.sd_cs   != pins.sd_cs)   ||
      (cfgPins.sd_sck  != pins.sd_sck)  ||
      (cfgPins.sd_miso != pins.sd_miso) ||
      (cfgPins.sd_mosi != pins.sd_mosi) ||
      (cfgPins.sda     != pins.sda)     ||
      (cfgPins.scl     != pins.scl)     ||
      (cfgPins.gps_rx  != pins.gps_rx)  ||
      (cfgPins.gps_tx  != pins.gps_tx)  ||
      (cfgPins.btn     != pins.btn);

    if (pinsChangedAgain) {
      Serial.print("[PINS] Config load changed pinmap -> ");
      Serial.println(cfgPins.name);

      pins = cfgPins;

      Serial.println("[SD] Re-init SPI + SD after config pinmap change...");
      SPI.begin(pins.sd_sck, pins.sd_miso, pins.sd_mosi, pins.sd_cs);

      sdOk = SD.begin(pins.sd_cs, SPI, 8000000);
      if (!sdOk) sdOk = SD.begin(pins.sd_cs, SPI, 4000000);

      Serial.print("[SD] SD.begin (post-config pins): ");
      Serial.println(sdOk ? "OK" : "FAIL");
    }
  }

  // =========================
  // Phase 3: Init Button + OLED + GPS using FINAL pins
  // =========================
  pinMode(pins.btn, INPUT_PULLUP);
  Serial.print("[BTN] Init OK (GPIO ");
  Serial.print(pins.btn);
  Serial.println(", INPUT_PULLUP)");

  // I2C OLED (final pins)
  Serial.println("[LCD] Initializing I2C + SSD1306 (final pins)...");
  pinMode(pins.sda, INPUT_PULLUP);
  pinMode(pins.scl, INPUT_PULLUP);
  delay(50);

  Wire.begin(pins.sda, pins.scl);
  Serial.print("[I2C] SDA="); Serial.print(pins.sda);
  Serial.print(" SCL="); Serial.println(pins.scl);
  Serial.print("[I2C] SDA level="); Serial.print(digitalRead(pins.sda));
  Serial.print(" SCL level="); Serial.println(digitalRead(pins.scl));
  Wire.setClock(100000);

  bool lcdOk = false;
  lcdOk = display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  if (!lcdOk) {
    Serial.println("[LCD] 0x3C failed, trying 0x3D...");
    lcdOk = display.begin(SSD1306_SWITCHCAPVCC, 0x3D);
  }

  if (lcdOk) {
    Serial.println("[LCD] SSD1306 init OK");
    showSplashScreen();
  } else {
    Serial.println("[LCD] SSD1306 init FAIL at 0x3C and 0x3D");
  }

  // GPS UART (final pins)
  Serial.println("[GPS] Initializing UART...");
  Serial.print("[GPS] Pins RX=");
  Serial.print(pins.gps_rx);
  Serial.print(" TX=");
  Serial.print(pins.gps_tx);
  Serial.print(" Baud=");
  Serial.println(cfg.gpsBaud);

  GPSSerial.begin(cfg.gpsBaud, SERIAL_8N1, pins.gps_rx, pins.gps_tx);
  Serial.println("[GPS] UART started");

  // Try STA FIRST (short timeout). Do NOT start AP unless STA fails.
  WiFi.mode(WIFI_STA);
  bool staOk = connectSTA(12000);

  // If STA failed, stop background reconnect attempts, then start AP
  if (!staOk) {
    WiFi.setAutoReconnect(false);
    WiFi.persistent(false);
    WiFi.disconnect(true, true);
    delay(100);

    startAP();
  }

  lastStaStatus = WiFi.status();

  // Boot upload: WDGoWars first (if key set), then WiGLE (if token set),
  // then move files. Requires STA connection and SD card.
  {
    bool hasWigle = cfg.wigleBasicToken.length() > 0;
    bool hasWdg   = cfg.wdgwarsApiKey.length()   > 0;

    if (staOk && sdOk && (hasWigle || hasWdg)) {
      Serial.print("[UPLOAD] STA connected. Services: ");
      if (hasWdg)   Serial.print("WDGoWars ");
      if (hasWigle) Serial.print("WiGLE ");
      Serial.println();
      Serial.printf("[HEAP] Free: %d bytes\n", ESP.getFreeHeap());

      if (cfg.maxBootUploads != 0) {
        // -1 = no limit, positive = capped; pass directly (uploadAllCsvsToWigle treats -1 as unlimited)
        int limit = cfg.maxBootUploads;  // -1 or positive
        if (limit == -1)
          Serial.println("[UPLOAD] Auto-uploading ALL CSVs (no limit)...");
        else
          Serial.printf("[UPLOAD] Auto-uploading CSVs (max %d)...\n", limit);

        uint32_t uploaded = uploadAllCsvsToWigle(limit);
        Serial.printf("[UPLOAD] Done: %d files moved\n", uploaded);

        // Reload WiGLE history if WiGLE was active and uploads occurred
        if (uploaded > 0 && hasWigle) {
          Serial.println("[WiGLE] Loading upload history...");
          delay(2000);
          wigleLoadHistory();
          Serial.printf("[WiGLE] History loaded (%d files)\n", wigleHistoryCount);
        }
      } else {
        Serial.println("[UPLOAD] Auto-upload disabled (maxBootUploads=0). Use web UI.");
      }
    } else {
      Serial.println("[UPLOAD] Upload not attempted (STA/SD not ready or no tokens set).");
    }
  }

  // Now start web server after WiGLE operations are complete
  Serial.println("[WEB] Starting web server routes...");
  startWebServer();
  
  if (staOk) {
    Serial.print("[WEB] STA IP: ");
    Serial.println(WiFi.localIP());
  } else if (apWindowActive) {
    Serial.print("[WEB] AP IP: ");
    Serial.println(WiFi.softAPIP());
    Serial.println("[WEB] AP UI: http://192.168.4.1/");
  }

  // Purge any header-only CSVs left over from previous sessions
  if (sdOk) {
    Serial.println("[SD] Cleaning up empty CSVs...");
    deleteEmptyCsvs();
  }

  // Create a fresh log for this run
  if (sdOk) {
    bool lfOk = openLogFile();
    Serial.print("[SD] Log file create: ");
    Serial.println(lfOk ? "OK" : "FAIL");
  } else {
    Serial.println("[SD] Log file create skipped (SD FAIL).");
  }

  // Battery test (configurable)
  if (sdOk && cfg.batteryTest) {
    batteryTestInit();
    Serial.println("[BATT] Battery test enabled");
  }

  updateOLED(0);

  Serial.println("=== Boot complete ===");
}

// ================================================================
//  loop()
// ================================================================
void loop() {
  // Web server
  server.handleClient();

  // Track AP client presence and enforce 60s AP window
  if (apWindowActive && WiFi.getMode() == WIFI_AP_STA) {
    if (WiFi.softAPgetStationNum() > 0) {
      if (!apClientSeen) Serial.println("[WIFI] AP client connected");
      apClientSeen = true;
    }
  }
  stopAPIfAllowed();

  // GPS parsing
  while (GPSSerial.available()) gps.encode(GPSSerial.read());

  bool prevFix = gpsHasFix;
  gpsHasFix = gps.location.isValid() && gps.location.age() < 2000;

  if (gpsHasFix != prevFix) {
    Serial.print("[GPS] Fix state changed -> ");
    Serial.println(gpsHasFix ? "LOCKED" : "NO FIX");
    if (gpsHasFix) {
      Serial.print("[GPS] Lat=");
      Serial.print(gps.location.lat(), 6);
      Serial.print(" Lon=");
      Serial.println(gps.location.lng(), 6);
    }
  }

  float speedKmph = gps.speed.isValid() ? gps.speed.kmph() : 0.0f;

  // Feed heading smoothing buffer when course is valid and we're moving enough
  if (gpsHasFix &&
      gps.course.isValid() &&
      gps.course.age() < 2000 &&
      gps.speed.isValid() &&
      gps.speed.kmph() >= HEADING_MIN_SPEED_KMPH) {
    headingFeed(gps.course.deg());
  }

  // Apply display units preference
  float speedDisplay = speedKmph;
  const char* speedUnitLabel = "km/h";
  if (cfg.speedUnits == "mph") {
    speedDisplay = speedKmph * 0.621371f;
    speedUnitLabel = "mph";
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

  // OLED refresh
  static uint32_t lastOled = 0;
  if (currentPage == 4) {
    // Pig page: animation runs at its own framerate (~90ms)
    pigAnimTick();
  } else if (millis() - lastOled > 500) {
    lastOled = millis();
    updateOLED(speedDisplay);  // also dispatches page 5 (mesh node)
  }

  handleStaTransitions();

  // Scanning – page-aware logic
  // Mesh node page handles its own scan via nodeModeTick(); skip normal path.
  if (currentPage == 5) {
    nodeModeTick();
  } else {
    autoPaused = shouldPauseScanning();
    wifi_mode_t m = WiFi.getMode();
    bool apActive = (m == WIFI_AP || m == WIFI_AP_STA);

    bool allowScan;
    if (currentPage == 3) {
      // Pause page: always stop scanning
      allowScan = false;
    } else if (currentPage == 0) {
      // Status page: respect AP/STA pause + double-press pause
      allowScan = scanningEnabled && sdOk && !statusPagePaused &&
                  !apActive && (userScanOverride || !autoPaused);
    } else {
      // Pages 1 (networks), 2 (nav), 4 (pig): respect AP/STA pause
      allowScan = scanningEnabled && sdOk && !apActive && (userScanOverride || !autoPaused);
    }
    allowScanForOled = allowScan;

    if (allowScan) {
      doScanOnce();
    }
  }

  // Battery test tick (uncomment to enable)
  batteryTestTick();

  delay(10);
}
