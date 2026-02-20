#include "Scanner.h"
#include "Globals.h"
#include "Config.h"
#include "Dedup.h"
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

void doScanOnce() {
  static uint32_t lastScanMs = 0;
  uint32_t interval = (cfg.scanMode == "powersaving") ? 12000 : 4500;

  if (millis() - lastScanMs < interval) return;

  Serial.print("[SCAN] Starting WiFi scan (mode=");
  Serial.print(cfg.scanMode);
  Serial.println(")...");

  uint32_t t0 = millis();
  int n = WiFi.scanNetworks(false, true);   // ONE scan only
  static uint8_t zeroScanCount = 0;

  // IMPORTANT: enforce interval even on empty/failed scans
  lastScanMs = millis();

  if (n <= 0) {
    zeroScanCount++;

    Serial.printf("[SCAN] EMPTY (%u) — resetting WiFi if stuck\n", zeroScanCount);

    // Keep scan state clean even on failure
    WiFi.scanDelete();

    // If we get multiple empty scans in a row, reset WiFi radio (C6 fix)
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

  zeroScanCount = 0;  // got real results -> clear counter
  uint32_t dt = millis() - t0;

  Serial.printf("[SCAN] scanNetworks() returned %d in %lu ms\n", n, (unsigned long)dt);

  if (n <= 0) {
    Serial.println("[SCAN] No networks found");
    WiFi.scanDelete();
    return;
  }

  Serial.print("[SCAN] Found raw networks: ");
  Serial.println(n);

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

    // Some cores return 0 for "unknown". Assume 2.4G for logging purposes.
    bool chUnknown = (ch == 0);

    bool is2g = (ch >= 1 && ch <= 14) || chUnknown;
    bool is5g = (ch >= 32 && ch <= 177);

    if (!is2g) {
      if (!(wardriverIsC5() && is5g)) continue;
    }

    String ssid = WiFi.SSID(i);
    String mac  = WiFi.BSSIDstr(i);

    uint64_t key = bssidStrToKey48(mac);
    if (seenCheckOrInsert(key)) {
      continue; // already logged this AP
    }

    int rssi = WiFi.RSSI(i);

    wifi_auth_mode_t auth = WiFi.encryptionType(i);
    String authStr = authModeToString(auth);

    if (is2g) networksFound2G++;
    else      networksFound5G++;

    appendWigleRow(mac, ssid, authStr, firstSeen, ch, rssi, lat, lon, altM, accM);
    wrote++;
  }

  WiFi.scanDelete();

  Serial.print("[SCAN] Wrote 2.4G/5G rows: ");
  Serial.println(wrote);
}
