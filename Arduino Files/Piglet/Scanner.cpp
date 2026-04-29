#include "Scanner.h"
#include "Globals.h"
#include "Config.h"
#include "GPS.h"
#include "SDUtils.h"

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

// ---- Result processor (shared between sync and async paths) ----
static void processScanResults(int n) {
  if (n <= 0) { WiFi.scanDelete(); return; }

  String firstSeen = iso8601NowUTC();
  double lat = 0, lon = 0, altM = 0, accM = 0;
  if (gpsHasFix) {
    lat  = gps.location.lat();
    lon  = gps.location.lng();
    altM = gps.altitude.meters();
    accM = gps.hdop.hdop();
  }

  uint32_t wrote = 0;
  for (int i = 0; i < n; i++) {
    int ch = WiFi.channel(i);
    bool chUnknown = (ch == 0);
    bool is2g = (ch >= 1 && ch <= 14) || chUnknown;
    bool is5g = (ch >= 32 && ch <= 177);

    if (!is2g) {
      if (!(wardriverIsC5() && is5g)) continue;
    }

    String ssid   = WiFi.SSID(i);
    String mac    = WiFi.BSSIDstr(i);
    int    rssi   = WiFi.RSSI(i);
    String authStr = authModeToString(WiFi.encryptionType(i));

    if (is2g) networksFound2G++;
    else      networksFound5G++;

    appendWigleRow(mac, ssid, authStr, firstSeen, ch, rssi, lat, lon, altM, accM);
    wrote++;
  }

  WiFi.scanDelete();
  Serial.printf("[SCAN] Wrote %lu rows\n", (unsigned long)wrote);
}

void doScanOnce() {
  static uint32_t lastScanStartMs  = 0;
  static bool     scanInProgress   = false;
  static uint8_t  zeroScanCount    = 0;

  // ---- Timing ----
  // aggressive:  100 ms/channel dwell, 1500 ms minimum gap between scan starts
  // powersaving: 200 ms/channel dwell, 10000 ms gap
  //
  // With 100 ms/channel the hardware finishes a 13-channel 2.4 GHz sweep in
  // ~1.3 s instead of the old ~3.9 s (default 300 ms dwell).  Using async
  // mode means that time no longer blocks the main loop — GPS parsing, the
  // web server and OLED updates all continue while the radio hops channels.
  bool powersave     = (cfg.scanMode == "powersaving");
  uint32_t gapMs     = powersave ? 10000 : 1500;
  uint32_t dwellMs   = powersave ?   200 :  100;

  // ---- Check if the async scan launched last iteration has finished ----
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

  // ---- Wait for the minimum gap before starting the next scan ----
  if (millis() - lastScanStartMs < gapMs) return;

  // ---- Kick off a new async scan ----
  // async=true, show_hidden=true, passive=false, max_ms_per_chan=dwellMs
  int16_t rc = WiFi.scanNetworks(/*async*/true, /*show_hidden*/true,
                                 /*passive*/false, dwellMs);
  if (rc == WIFI_SCAN_RUNNING || rc == 0) {
    scanInProgress = true;
    Serial.printf("[SCAN] Async scan started (dwell=%lu ms)\n", (unsigned long)dwellMs);
  } else {
    // Shouldn’t normally happen; fall back and retry after gap
    Serial.printf("[SCAN] scanNetworks start failed (%d)\n", rc);
    lastScanStartMs = millis();
  }
}
