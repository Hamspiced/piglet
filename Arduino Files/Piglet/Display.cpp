#include "Display.h"
#include "Globals.h"
#include "GPS.h"
#include "Config.h"
#include "SDUtils.h"
#include "MeshNode.h"
#include <math.h>

// ---- Splash slogans ----

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

// ---- Drawing helpers ----

static void drawCentered(int y, const char* txt, uint8_t size) {
  display.setTextSize(size);
  int16_t x1, y1;
  uint16_t w, h;
  display.getTextBounds(txt, 0, 0, &x1, &y1, &w, &h);
  int x = (OLED_W - (int)w) / 2;
  display.setCursor(x, y);
  display.print(txt);
}

static void drawCompassNoFix(int cx, int cy) {
  // simple ring placeholder (clean + readable)
  const int r = 13;
  display.drawCircle(cx, cy, r, SSD1306_WHITE);
  display.drawCircle(cx, cy, r - 1, SSD1306_WHITE);   // slight thickness
  display.fillCircle(cx, cy, 2, SSD1306_WHITE);       // center dot (optional)
}

static const char* courseTo8way(double deg) {
  if (!isfinite(deg)) return "\xe2\x80\x94"; // em-dash UTF-8
  // Normalize 0..360
  while (deg < 0) deg += 360.0;
  while (deg >= 360.0) deg -= 360.0;

  // 8 sectors of 45 deg, centered on N at 0 deg
  int idx = (int)((deg + 22.5) / 45.0) & 7;

  static const char* dirs[8] = {"N","NE","E","SE","S","SW","W","NW"};
  return dirs[idx];
}

static void drawFilledChevronArrowBig(int cx, int cy, const char* dir) {
  // Bigger elongated dart + notched tail.
  // Tuned for ~40px wide right-side widget.

  struct Tri { int8_t x1,y1,x2,y2,x3,y3; };
  Tri mainT, notchT;

  if (!strcmp(dir,"N")) {
    mainT  = {  0,-12,  -9, 12,   9, 12 };
    notchT = { -4, 12,   4, 12,   0,  4 };
  } else if (!strcmp(dir,"NE")) {
    mainT  = { 10,-10, -12, -2,   2, 12 };
    notchT = { -5,  2,  -2,  5,  -6,  6 };
  } else if (!strcmp(dir,"E")) {
    mainT  = { 12,  0, -12, -9, -12,  9 };
    notchT = { -12, -4, -12,  4,  -4,  0 };
  } else if (!strcmp(dir,"SE")) {
    mainT  = { 10, 10,   2,-12, -12,  2 };
    notchT = { -2, -5,  -5, -2,  -6, -6 };
  } else if (!strcmp(dir,"S")) {
    mainT  = {  0, 12,  -9,-12,   9,-12 };
    notchT = { -4,-12,   4,-12,   0, -4 };
  } else if (!strcmp(dir,"SW")) {
    mainT  = { -10, 10,  12,  2,  -2,-12 };
    notchT = {  5, -2,   2, -5,   6, -6 };
  } else if (!strcmp(dir,"W")) {
    mainT  = { -12, 0,   12,-9,   12, 9 };
    notchT = {  12,-4,   12, 4,    4, 0 };
  } else if (!strcmp(dir,"NW")) {
    mainT  = { -10,-10,  -2, 12,  12, -2 };
    notchT = {  2,  5,   5,  2,   6,  6 };
  } else {
    return;
  }

  display.fillTriangle(cx + mainT.x1,  cy + mainT.y1,
                       cx + mainT.x2,  cy + mainT.y2,
                       cx + mainT.x3,  cy + mainT.y3,
                       SSD1306_WHITE);

  display.fillTriangle(cx + notchT.x1, cy + notchT.y1,
                       cx + notchT.x2, cy + notchT.y2,
                       cx + notchT.x3, cy + notchT.y3,
                       SSD1306_BLACK);
}

// ---- Battery ----

// Returns battery percentage 1-100, or -1 if not configured / no battery detected.
static int readBatteryPercent() {
  if (cfg.battPin < 0) return -1;

  // 16-sample averaging via calibrated ADC
  uint32_t sum = 0;
  for (int i = 0; i < 16; i++) {
    sum += analogReadMilliVolts((uint8_t)cfg.battPin);
  }
  float adcMv = (float)sum / 16.0f;

  // 1:2 voltage divider -> actual battery voltage
  float battV = adcMv * 2.0f / 1000.0f;

  // Below ~2.5 V means no battery is connected (pin floating or USB-only)
  if (battV < 2.5f) return -1;

  // Linear LiPo mapping: 3.0 V = 0 %, 4.2 V = 100 %
  int pct = (int)((battV - 3.0f) / (4.2f - 3.0f) * 100.0f);
  if (pct < 1)   pct = 1;
  if (pct > 100) pct = 100;
  return pct;
}

// ---- Progress bar ----

void oledProgressBar(int x, int y, int w, int h, float pct) {
  if (pct < 0) pct = 0;
  if (pct > 1) pct = 1;
  display.drawRect(x, y, w, h, SSD1306_WHITE);
  int fill = (int)((w - 2) * pct);
  if (fill > 0) display.fillRect(x + 1, y + 1, fill, h - 2, SSD1306_WHITE);
}

// ---- Pig animation state ----
PigAnim pig;

static const int16_t PIG_W = 44;
static const int16_t PIG_H = 26;

static void drawPig(int16_t x, int16_t y, uint8_t phase) {
  // Filled cartoon pig (side view, facing LEFT)
  // Bounding box: PIG_W x PIG_H.  Filled white with black details.

  // ---- BODY (big rounded oval) ----
  display.fillRoundRect(x + 14, y + 3, 22, 15, 7, SSD1306_WHITE);

  // ---- HEAD (overlapping circle) ----
  display.fillCircle(x + 14, y + 10, 7, SSD1306_WHITE);

  // ---- SNOUT (protruding from head) ----
  display.fillRoundRect(x + 2, y + 8, 9, 7, 3, SSD1306_WHITE);

  // ---- NOSTRILS (black dots on white snout) ----
  display.drawPixel(x + 4, y + 11, SSD1306_BLACK);
  display.drawPixel(x + 6, y + 11, SSD1306_BLACK);

  // ---- EYE (black square with white catchlight) ----
  display.fillRect(x + 9, y + 7, 3, 3, SSD1306_BLACK);
  display.drawPixel(x + 10, y + 7, SSD1306_WHITE);

  // ---- EAR (filled triangle on top of head) ----
  display.fillTriangle(x + 12, y + 4, x + 18, y + 5, x + 15, y + 0, SSD1306_WHITE);
  display.drawLine(x + 14, y + 2, x + 16, y + 4, SSD1306_BLACK);  // inner ear fold

  // ---- MOUTH ----
  display.drawLine(x + 6, y + 14, x + 9, y + 15, SSD1306_BLACK);

  // ---- TAIL (curly pigtail, pixel art) ----
  display.drawPixel(x + 36, y + 6,  SSD1306_WHITE);
  display.drawPixel(x + 37, y + 5,  SSD1306_WHITE);
  display.drawPixel(x + 38, y + 5,  SSD1306_WHITE);
  display.drawPixel(x + 39, y + 6,  SSD1306_WHITE);
  display.drawPixel(x + 39, y + 7,  SSD1306_WHITE);
  display.drawPixel(x + 38, y + 8,  SSD1306_WHITE);
  display.drawPixel(x + 39, y + 9,  SSD1306_WHITE);
  display.drawPixel(x + 40, y + 9,  SSD1306_WHITE);
  display.drawPixel(x + 41, y + 8,  SSD1306_WHITE);

  // ---- BELLY LINE ----
  display.drawFastHLine(x + 15, y + 18, 20, SSD1306_WHITE);

  // ---- LEGS (2-phase walk cycle) ----
  const bool liftA = (phase & 1);
  const bool liftB = !liftA;
  const int16_t legTop = y + 18;
  const int16_t lx[4] = {
    (int16_t)(x + 16), (int16_t)(x + 22),
    (int16_t)(x + 28), (int16_t)(x + 33)
  };

  for (int i = 0; i < 4; i++) {
    bool lift = (i == 0 || i == 3) ? liftA : liftB;
    int16_t lxi = lx[i];
    if (lift) {
      // Leg angled forward
      display.drawLine(lxi, legTop, lxi - 2, legTop + 5, SSD1306_WHITE);
      display.drawLine(lxi - 2, legTop + 5, lxi,     legTop + 5, SSD1306_WHITE);
    } else {
      // Leg planted straight down
      display.drawLine(lxi, legTop, lxi,     legTop + 5, SSD1306_WHITE);
      display.drawLine(lxi, legTop + 5, lxi + 2, legTop + 5, SSD1306_WHITE);
    }
  }
}

void pigAnimTick() {
  uint32_t now = millis();
  if (now - pig.lastMs < pig.frameMs) return;
  pig.lastMs = now;

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  // Header
  display.setTextSize(2);
  display.setCursor(0, 0);
  display.print("Piglet");
  display.drawFastHLine(0, OLED_YELLOW_H - 1, OLED_W, SSD1306_WHITE);

  // Move + bounce
  pig.x += pig.dx;
  if (pig.x <= 0)              { pig.x = 0;              pig.dx =  1; }
  if (pig.x >= (128 - PIG_W))  { pig.x = 128 - PIG_W;    pig.dx = -1; }

  pig.phase = (pig.phase + 1) & 3;
  int16_t bob = (pig.phase == 1 || pig.phase == 3) ? 1 : 0;

  // Keep pig inside screen vertically
  if (pig.y > (OLED_H - PIG_H)) {
    pig.y = (OLED_H - PIG_H);
  }

  drawPig(pig.x, pig.y + bob, pig.phase);

  display.display();
}

// ---- Page renderers ----

// Page 0: Status (existing)
static void drawPageStatus(float speedValue) {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  // ----- Yellow band header (0..15) -----
  display.setTextSize(2);
  display.setCursor(0, 0);

  if (uploading) {
    // Show the active upload service (set by WigleUpload.cpp before each upload)
    if (uploadTargetName.length() > 0)
      display.print(uploadTargetName);
    else
      display.print("Uploading");
    display.setTextSize(1);
  } else {
    // Match the real scan eligibility logic from loop()
    bool allowScan = scanningEnabled && sdOk && (userScanOverride || !autoPaused);

    display.print("Status:");

    // --- PLAY / PAUSE glyph ---
    int playX = 84;
    int iconY = 2;

    if (allowScan) {
      // PLAY
      display.fillTriangle(playX, iconY, playX, iconY + 12, playX + 10, iconY + 6, SSD1306_WHITE);
    } else {
      // PAUSE
      display.fillRect(playX,     iconY, 3, 12, SSD1306_WHITE);
      display.fillRect(playX + 6, iconY, 3, 12, SSD1306_WHITE);
    }

    // --- GPS indicator: X when NO FIX, otherwise 6-bar satellite-strength meter ---
    const int topY    = iconY;
    const int meterX  = 108;
    const int meterW  = 18;
    const int meterH  = 12;
    const int barW    = 2;
    const int barStep = 3;

    if (!gpsHasFix) {
      // NO FIX: show X
      int x0 = meterX + 3;
      int y0 = topY;
      display.drawLine(x0, y0, x0 + 10, y0 + 12, SSD1306_WHITE);
      display.drawLine(x0 + 10, y0, x0, y0 + 12, SSD1306_WHITE);
    } else {
      // FIX: show satellite-strength bars (1..6)
      int sats = 0;
      if (gps.satellites.isValid()) sats = (int)gps.satellites.value();
      if (sats < 0) sats = 0;
      if (sats > 6) sats = 6;

      for (int i = 0; i < 6; i++) {
        int bh = 2 + (i * 2);
        int bx = meterX + i * barStep;
        int by = topY + meterH - bh;

        display.drawRect(bx, by, barW, bh, SSD1306_WHITE);

        if (i < sats) {
          display.fillRect(bx, by, barW, bh, SSD1306_WHITE);
        }
      }
    }

    display.setTextSize(1);
  }

  // separator at color boundary
  display.drawFastHLine(0, OLED_YELLOW_H - 1, OLED_W, SSD1306_WHITE);

  // ----- Blue area starts -----
  int y = OLED_YELLOW_H;

  if (uploading) {
    // Row 1: progress count + fail counter on the right
    display.setCursor(0, y);
    display.print(uploadDoneFiles);
    display.print("/");
    display.print(uploadTotalFiles);
    display.print(" files");
    if (uploadFailedFiles > 0) {
      // Right-align the fail count — each size-1 char is 6px wide
      String failTxt = String("F:") + String(uploadFailedFiles);
      display.setCursor(OLED_W - (int)failTxt.length() * 6, y);
      display.print(failTxt);
    }

    y += 10;
    String name = uploadCurrentFile.length() ? pathBasename(uploadCurrentFile) : "";
    if (name.length() > 0) {
      // Shorten filename if fail counter is shown to avoid overlap
      int maxChars = (uploadFailedFiles > 0) ? 16 : 21;
      if ((int)name.length() > maxChars) name = name.substring(0, maxChars);
      display.setCursor(0, y);
      display.print(name);
      y += 10;
    }

    float pct = (uploadTotalFiles == 0) ? 0.0f : ((float)uploadDoneFiles / (float)uploadTotalFiles);
    oledProgressBar(0, y, 128, 10, pct);
    y += 14;

    display.setCursor(0, y);
    display.print("STA: ");
    display.print(WiFi.status() == WL_CONNECTED ? "YES" : "NO");
    display.print("  SD: ");
    display.print(sdOk ? "OK" : "FAIL");

    display.display();
    return;
  }

  // Normal status lines (FIXED ROWS so layout doesn't shift by board)
  const int yLine2g   = OLED_YELLOW_H + 0;   // 16
  const int yLine5g   = OLED_YELLOW_H + 10;  // 26 (reserved even if not C5)
  const int yLineSpd  = OLED_YELLOW_H + 20;  // 36
  const int yLineSD   = OLED_YELLOW_H + 30;  // 46
  const int yBottom   = OLED_YELLOW_H + 40;  // 56

  // 2.4G (always)
  display.setCursor(0, yLine2g);
  display.print("2.4G: ");
  display.print(networksFound2G);

  // Battery percentage (far right, below GPS bars) — cached every 5 s
  {
    static int cachedBattPct = -1;
    static uint32_t lastBattRead = 0;
    uint32_t now = millis();
    if (cfg.battPin >= 0 && (cachedBattPct < 0 || (now - lastBattRead) >= 5000)) {
      cachedBattPct = readBatteryPercent();
      lastBattRead = now;
    }
    if (cachedBattPct > 0) {
      display.setTextSize(1);
      int chars = (cachedBattPct >= 100) ? 3 : (cachedBattPct >= 10) ? 2 : 1;
      display.setCursor(OLED_W - chars * 6, yLine2g);
      display.print(cachedBattPct);
    }
  }

  // 5G (reserved row; only print on C5, but DO NOT change layout)
  if (wardriverIsC5()) {
    display.setCursor(0, yLine5g);
    display.print("5G: ");
    display.print(networksFound5G);
  }

  // Speed (always at same place)
  display.setCursor(0, yLineSpd);
  display.print("Speed: ");
  display.print(speedValue, 1);
  display.print(cfg.speedUnits == "mph" ? " mph" : " km/h");

  // SD (always at same place)
  display.setCursor(0, yLineSD);
  display.print("SD: ");
  display.print(sdOk ? "OK" : "FAIL");

  // Bottom row: IP / AP countdown / Compass (ALWAYS SAME Y)
  display.setCursor(0, yBottom);

  if (WiFi.status() == WL_CONNECTED) {
    display.print("IP: ");
    display.print(WiFi.localIP());
  }
  else if (apWindowActive) {
    uint32_t elapsed = millis() - apStartMs;
    uint32_t remainingMs = (elapsed >= AP_WINDOW_MS) ? 0 : (AP_WINDOW_MS - elapsed);
    uint32_t remainingS  = (remainingMs + 999) / 1000;

    display.print("AP: 192.168.4.1 ");
    display.print(remainingS);
    display.print("s");
  }
  else {
    // No STA and AP window is not active: show compass
    const uint32_t nowMs = millis();

    // ---- Fixed compass geometry ----
    const int COMPASS_CX = 114;
    const int COMPASS_CY = 40;
    const int LABEL_Y    = 55;

    bool canUseFresh =
      gpsHasFix &&
      gps.course.isValid() &&
      (gps.course.age() < 2000) &&
      gps.speed.isValid() &&
      (gps.speed.kmph() >= HEADING_MIN_SPEED_KMPH);

    // If fresh+fast enough, use smoothed heading
    if (canUseFresh) {
      double h = headingSmoothedDeg();
      if (isfinite(h)) {
        lastGoodHeadingDeg = h;
        lastGoodHeadingMs  = nowMs;
      }
    }

    bool haveHeld = isfinite(lastGoodHeadingDeg) && ((nowMs - lastGoodHeadingMs) <= HEADING_HOLD_MS);

    if (!haveHeld) {
      drawCompassNoFix(COMPASS_CX, COMPASS_CY);
    } else {
      const char* dir = courseTo8way(lastGoodHeadingDeg);

      // Arrow centered at fixed location
      drawFilledChevronArrowBig(COMPASS_CX, COMPASS_CY, dir);

      // Label fixed below arrow (centered)
      display.setTextSize(1);

      int16_t x1, y1;
      uint16_t w, h;
      display.getTextBounds(dir, 0, 0, &x1, &y1, &w, &h);

      int labelX = COMPASS_CX - ((int)w / 2);
      if (labelX < 0) labelX = 0;
      if (labelX > 127 - (int)w) labelX = 127 - (int)w;

      int labelY = LABEL_Y;
      if (labelY > 63 - 8) labelY = 63 - 8;

      display.setCursor(labelX, labelY);
      display.print(dir);
    }
  }

  display.display();
}

// Page 1: Network counts (large text)
static void drawPageNetworks() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  // Yellow header
  display.setTextSize(2);
  display.setCursor(0, 0);
  display.print("Networks");
  display.drawFastHLine(0, OLED_YELLOW_H - 1, OLED_W, SSD1306_WHITE);

  // 2.4G count
  display.setTextSize(1);
  display.setCursor(0, 18);
  display.print("2.4G:");
  display.setTextSize(2);
  display.setCursor(36, 16);
  display.print(networksFound2G);

  // 5G count
  display.setTextSize(1);
  display.setCursor(0, 34);
  display.print("5GHz:");
  display.setTextSize(2);
  display.setCursor(36, 32);
  display.print(networksFound5G);

  // Total
  display.setTextSize(1);
  display.setCursor(0, 50);
  display.print("Total:");
  display.setTextSize(2);
  display.setCursor(36, 48);
  display.print(networksFound2G + networksFound5G);

  display.display();
}

// Page 2: Navigation — large compass arrow + direction + speed
static void drawPageNavigation(float speedValue) {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  // Yellow header
  display.setTextSize(2);
  display.setCursor(0, 0);
  display.print("Nav");

  // GPS signal indicator in header (matches status page)
  {
    const int iconY  = 2;
    const int meterX = 108;
    const int barW   = 2;
    const int barStep = 3;
    const int meterH = 12;

    if (!gpsHasFix) {
      int x0 = meterX + 3;
      display.drawLine(x0, iconY, x0 + 10, iconY + 12, SSD1306_WHITE);
      display.drawLine(x0 + 10, iconY, x0, iconY + 12, SSD1306_WHITE);
    } else {
      int sats = 0;
      if (gps.satellites.isValid()) sats = (int)gps.satellites.value();
      if (sats < 0) sats = 0;
      if (sats > 6) sats = 6;

      for (int i = 0; i < 6; i++) {
        int bh = 2 + (i * 2);
        int bx = meterX + i * barStep;
        int by = iconY + meterH - bh;
        display.drawRect(bx, by, barW, bh, SSD1306_WHITE);
        if (i < sats) {
          display.fillRect(bx, by, barW, bh, SSD1306_WHITE);
        }
      }
    }
  }

  display.drawFastHLine(0, OLED_YELLOW_H - 1, OLED_W, SSD1306_WHITE);

  const uint32_t nowMs = millis();

  bool canUseFresh =
    gpsHasFix &&
    gps.course.isValid() &&
    (gps.course.age() < 2000) &&
    gps.speed.isValid() &&
    (gps.speed.kmph() >= HEADING_MIN_SPEED_KMPH);

  if (canUseFresh) {
    double h = headingSmoothedDeg();
    if (isfinite(h)) {
      lastGoodHeadingDeg = h;
      lastGoodHeadingMs  = nowMs;
    }
  }

  bool haveHeld = isfinite(lastGoodHeadingDeg) && ((nowMs - lastGoodHeadingMs) <= HEADING_HOLD_MS);

  // Left side: compass arrow (centered in left half)
  const int arrowCX = 32;
  const int arrowCY = 33;

  if (!haveHeld) {
    drawCompassNoFix(arrowCX, arrowCY);
    display.setTextSize(1);
    display.setCursor(20, 50);
    display.print("No Fix");
  } else {
    const char* dir = courseTo8way(lastGoodHeadingDeg);
    drawFilledChevronArrowBig(arrowCX, arrowCY, dir);

    // Direction label below arrow
    display.setTextSize(2);
    int16_t x1, y1;
    uint16_t tw, th;
    display.getTextBounds(dir, 0, 0, &x1, &y1, &tw, &th);
    int labelX = arrowCX - ((int)tw / 2);
    if (labelX < 0) labelX = 0;
    display.setCursor(labelX, 48);
    display.print(dir);
  }

  // Right side: speed (large)
  display.setTextSize(1);
  display.setCursor(68, 18);
  display.print("Speed");

  display.setTextSize(3);
  // Format speed as integer for large display
  int spd = (int)(speedValue + 0.5f);
  char spdBuf[8];
  snprintf(spdBuf, sizeof(spdBuf), "%d", spd);
  // Right-align in the right half
  int16_t sx1, sy1;
  uint16_t sw, sh;
  display.getTextBounds(spdBuf, 0, 0, &sx1, &sy1, &sw, &sh);
  int spdX = 128 - (int)sw - 2;
  if (spdX < 68) spdX = 68;
  display.setCursor(spdX, 30);
  display.print(spdBuf);

  // Units label
  display.setTextSize(1);
  display.setCursor(68, 56);
  display.print(cfg.speedUnits == "mph" ? "mph" : "km/h");

  // Divider between compass and speed
  display.drawFastVLine(63, OLED_YELLOW_H, OLED_H - OLED_YELLOW_H, SSD1306_WHITE);

  display.display();
}

// Page 3: Pause scanning
static void drawPagePaused() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  // Yellow header
  display.setTextSize(2);
  display.setCursor(0, 0);
  display.print("Paused");
  display.drawFastHLine(0, OLED_YELLOW_H - 1, OLED_W, SSD1306_WHITE);

  // Big pause icon (two bars)
  display.fillRect(42, 24, 10, 30, SSD1306_WHITE);
  display.fillRect(76, 24, 10, 30, SSD1306_WHITE);

  display.setTextSize(1);
  display.setCursor(24, 56);
  display.print("Scan paused");

  display.display();
}

// ---- Main OLED dispatch ----

void updateOLED(float speedValue) {
  // During upload, always show upload progress regardless of page
  if (uploading) {
    drawPageStatus(speedValue);
    return;
  }

  switch (currentPage) {
    case 0:  drawPageStatus(speedValue);     break;
    case 1:  drawPageNetworks();             break;
    case 2:  drawPageNavigation(speedValue); break;
    case 3:  drawPagePaused();               break;
    case 4:  /* pig handled by pigAnimTick in loop() */ break;
    case 5:  drawPageMeshNode();             break;
    default: drawPageStatus(speedValue);     break;
  }
}

// ---- Boot splash screen ----

void showSplashScreen() {
  const char* slogan = pickSplashSlogan();

  // --- Splash layout constants ---
  const int YELLOW_H = OLED_YELLOW_H;
  const int BAR_W    = 108;
  const int BAR_H    = 14;
  const int BAR_X    = (OLED_W - BAR_W) / 2;
  const int BAR_Y    = YELLOW_H + 18;

  const char* credit = "By Midwest Gadgets";

  // --- Static header ---
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  // Title in yellow
  drawCentered(0, "Piglet", 2);

  // Credit in blue
  display.setTextSize(1);
  {
    int16_t x1, y1;
    uint16_t w, h;
    display.getTextBounds(credit, 0, 0, &x1, &y1, &w, &h);
    int x = (OLED_W - (int)w) / 2;
    int y = YELLOW_H + 2;
    display.setCursor(x, y);
    display.print(credit);
  }

  // Version — lower-right corner
  display.setTextSize(1);
  display.setCursor(OLED_W - (int)strlen(FIRMWARE_VERSION) * 6 - 1, OLED_H - 8);
  display.print(FIRMWARE_VERSION);

  display.display();

  // --- Animated "fake loading bar" ---
  const uint32_t animMs  = 1600;
  const uint32_t frameMs = 35;
  uint32_t start = millis();

  const int innerPad = 2;
  const int innerW   = BAR_W - innerPad * 2;
  const int blockW   = 18;
  int pos = 0;
  int dir = 1;

  while (millis() - start < animMs) {
    display.clearDisplay();

    display.setTextColor(SSD1306_WHITE);
    drawCentered(0, "Piglet", 2);

    // Credit
    display.setTextSize(1);
    {
      int16_t x1, y1;
      uint16_t w, h;
      display.getTextBounds(credit, 0, 0, &x1, &y1, &w, &h);
      int x = (OLED_W - (int)w) / 2;
      int y = YELLOW_H + 2;
      display.setCursor(x, y);
      display.print(credit);
    }

    // Bar frame
    display.drawRect(BAR_X, BAR_Y, BAR_W, BAR_H, SSD1306_WHITE);

    // Text inside the bar
    display.setTextSize(1);
    int16_t x1, y1;
    uint16_t tw, th;
    display.getTextBounds(slogan, 0, 0, &x1, &y1, &tw, &th);
    int tx = BAR_X + (BAR_W - (int)tw) / 2;
    int ty = BAR_Y + (BAR_H - (int)th) / 2;

    // Clear bar interior
    display.fillRect(BAR_X + innerPad, BAR_Y + innerPad, innerW, BAR_H - innerPad * 2, SSD1306_BLACK);

    // Moving block
    display.fillRect(BAR_X + innerPad + pos, BAR_Y + innerPad, blockW, BAR_H - innerPad * 2, SSD1306_WHITE);

    // Re-draw slogan on top
    display.setCursor(tx, ty);
    display.setTextColor(SSD1306_WHITE);
    display.print(slogan);

    // Version — lower-right corner
    display.setTextSize(1);
    display.setCursor(OLED_W - (int)strlen(FIRMWARE_VERSION) * 6 - 1, OLED_H - 8);
    display.print(FIRMWARE_VERSION);

    display.display();

    // Bounce the block left/right
    pos += dir * 4;
    if (pos <= 0) { pos = 0; dir = 1; }
    if (pos >= (innerW - blockW)) { pos = innerW - blockW; dir = -1; }

    delay(frameMs);
  }

  // Final "100%" look for a beat
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  drawCentered(0, "Piglet", 2);

  // Credit
  display.setTextSize(1);
  {
    int16_t x1, y1;
    uint16_t w, h;
    display.getTextBounds(credit, 0, 0, &x1, &y1, &w, &h);
    int x = (OLED_W - (int)w) / 2;
    int y = YELLOW_H + 2;
    display.setCursor(x, y);
    display.print(credit);
  }

  display.drawRect(BAR_X, BAR_Y, BAR_W, BAR_H, SSD1306_WHITE);
  display.fillRect(BAR_X + 2, BAR_Y + 2, BAR_W - 4, BAR_H - 4, SSD1306_WHITE);

  // Slogan inside filled bar
  display.setTextSize(1);
  drawCentered(BAR_Y + 3, slogan, 1);

  // Version — lower-right corner
  display.setTextSize(1);
  display.setCursor(OLED_W - (int)strlen(FIRMWARE_VERSION) * 6 - 1, OLED_H - 8);
  display.print(FIRMWARE_VERSION);

  display.display();
  delay(250);
}
