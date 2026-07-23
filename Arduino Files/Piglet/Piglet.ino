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

  // Mesh node page lifecycle — always enter as Node; long-press activates Core
  if (newPage == 5) enterNodeMode();
  if (oldPage == 5) {
    if (meshCoreActive) exitCoreMode();
    else                exitNodeMode();
  }

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
        if (currentPage == 5) {
          // Mesh page: toggle Core / Node instead of sleeping
          if (meshCoreActive) {
            exitCoreMode();
            enterNodeMode();
          } else {
            exitNodeMode();
            enterCoreMode();
          }
        } else {
          enterDeepSleep();  // never returns
        }
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
    if (clickCount >= 3 && currentPage == 4) {
      sasquatchStart();
    } else if (clickCount == 2 && currentPage == 4) {
      pigTwerkStart();
    } else if (clickCount >= 2 && currentPage == 0) {
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
  if (pins.btn >= 0) pinMode(pins.btn, INPUT_PULLUP);
  Serial.print("[BTN] Init");
  if (pins.btn >= 0) { Serial.print(" OK (GPIO "); Serial.print(pins.btn); Serial.print(", INPUT_PULLUP)"); }
  else Serial.print(" skipped (btn=-1, no button on this board)");
  Serial.println();

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
    display.setRotation(cfg.rotateScreen180 ? 2 : 0);
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

  GPSSerial.setRxBufferSize(512);  // 512 bytes ≈ 530 ms at 9600 baud; avoids overflow during WiFi scans
  GPSSerial.begin(cfg.gpsBaud, SERIAL_8N1, pins.gps_rx, pins.gps_tx);
  Serial.println("[GPS] UART started");

  // GPS connection check — wait up to 2 s for any bytes from the module.
  // Diagnosis key:
  //   chars=0            -> RX not connected (check GPS TX wire)
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
                    pins.gps_rx, pins.gps_rx);
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
        WiFi.setAutoReconnect(false);
        WiFi.persistent(false);
        WiFi.disconnect(true, true);
        delay(100);
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

      } else {
        Serial.println("[UPLOAD] Auto-upload disabled (maxBootUploads=0). Use web UI.");
      }
    } else {
      Serial.println("[UPLOAD] Upload not attempted (STA/SD not ready or no tokens set).");
    }
  }

  // Auto-start wardriving: if enabled, drop the STA link after uploads so
  // scanning begins immediately. shouldPauseScanning() returns true while
  // WL_CONNECTED, so without this the device waits for STA to time out.
  // Skipped when meshModeOnBoot is set (mesh mode handles its own teardown).
  {
    String mm = cfg.meshModeOnBoot; mm.toLowerCase();
    bool meshBoot = (mm == "core" || mm == "node");
    if (cfg.autoStartAfterUpload && staOk && !meshBoot) {
      Serial.println("[BOOT] autoStartAfterUpload: disconnecting STA — wardriving begins now");
      WiFi.setAutoReconnect(false);
      WiFi.persistent(false);
      WiFi.disconnect(true, false);  // eraseap=false keeps NVS credentials
      delay(100);
      WiFi.mode(WIFI_STA);           // idle STA ready for scanning
      staOk = false;                 // suppress STA IP print below
      scanningEnabled = true;
    }
  }

  // Now start web server after WiGLE operations are complete
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

  // Auto-start mesh mode if meshModeOnBoot=Core|Node.
  // Core mode: STA was used for upload above and must be torn down so
  // ESP-Now can start cleanly on the admin channel without interference.
  // Node mode: STA was already skipped.
  {
    String mm = cfg.meshModeOnBoot;
    mm.toLowerCase();
    if (mm == "node") {
      Serial.println("[BOOT] meshModeOnBoot=Node — entering Mesh Node mode");
      currentPage = 5;  // mesh page on XIAO
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
      currentPage = 5;
      enterCoreMode();
    }
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

  // Track AP client presence and enforce AP window. The window does NOT
  // auto-extend on connect; the user must click "Stay" in the WebUI prompt
  // to push it out to the 5 min budget.
  if (apWindowActive && WiFi.getMode() == WIFI_AP_STA) {
    if (WiFi.softAPgetStationNum() > 0) {
      if (!apClientSeen) {
        Serial.println("[WIFI] AP client connected");
        apClientSeen = true;
      }
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

  // Update last-known-good position every loop — decoupled from scan results
  // so it stays current even when no networks are found.
  // Quality gate: HDOP ≤ 10 and ≥ 3 satellites guards against brief bad fixes
  // (e.g. re-acquisition after a tunnel) overwriting a good cached position.
  if (gpsHasFix) {
    float hdop = gps.hdop.isValid()       ? gps.hdop.hdop()           : 99.0f;
    int   sats = gps.satellites.isValid() ? (int)gps.satellites.value() : 0;
    if (hdop <= 10.0f && sats >= 3) {
      lastLat        = gps.location.lat();
      lastLon        = gps.location.lng();
      lastAlt        = gps.altitude.isValid() ? gps.altitude.meters() : 0.0;
      lastAcc        = hdop;
      lastGpsValid   = true;
      lastGpsValidMs = millis();
    }
  }

  // GPS health log every 10 s until fix acquired
  if (!gpsHasFix) {
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
                      pins.gps_rx);
      else if (failed > ok)
        Serial.printf("[GPS]   Checksum errors dominate — check baud rate (%lu bps)\n",
                      (unsigned long)cfg.gpsBaud);
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

  // Skip STA transition handler in mesh mode — it calls WiFi.disconnect(wifioff=true)
  // which stops the WiFi driver and deinits ESP-Now.
  if (!meshNodeActive && !meshCoreActive) handleStaTransitions();

  // Scanning – page-aware logic
  // Mesh node page handles its own scan via nodeModeTick(); skip normal path.
  if (currentPage == 5) {
    if (meshCoreActive) coreModeTick();
    else                nodeModeTick();
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
