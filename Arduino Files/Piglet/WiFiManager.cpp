#include "WiFiManager.h"
#include "Globals.h"
#include "esp_netif.h"

// ---- STA disconnect helper ----

void handleStaTransitions() {
  wl_status_t now = WiFi.status();

  // Detect transition: connected -> not connected
  if (lastStaStatus == WL_CONNECTED && now != WL_CONNECTED) {
    Serial.println("[WIFI] STA disconnected -> scanning can resume");

    // Only force scanning ON if the user has NOT explicitly overridden scanning off
    if (!(userScanOverride && !scanningEnabled)) {
      scanningEnabled = true;
    }
  }

  lastStaStatus = now;
}

// ---- DNS helpers ----

static void forceDnsEspNetif(IPAddress dns1, IPAddress dns2) {
  esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  if (!netif) {
    Serial.println("[NET] esp_netif WIFI_STA_DEF not found");
    return;
  }

  esp_netif_dns_info_t info;

  // MAIN DNS
  memset(&info, 0, sizeof(info));
  info.ip.type = ESP_IPADDR_TYPE_V4;
  info.ip.u_addr.ip4.addr = (uint32_t)dns1;
  esp_netif_set_dns_info(netif, ESP_NETIF_DNS_MAIN, &info);

  // BACKUP DNS
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

  IPAddress dns1(1,1,1,1);  // Cloudflare
  IPAddress dns2(8,8,8,8);  // Google

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

  IPAddress ip;
  if (WiFi.hostByName("api.wigle.net", ip)) {
    Serial.print("[NET] api.wigle.net -> "); Serial.println(ip);
  } else {
    Serial.println("[NET] DNS lookup failed for api.wigle.net");
  }
}

// ---- AP ----

void startAP() {
  Serial.println("[WIFI] Starting SoftAP...");

  // Hard reset WiFi state (important after a failed STA attempt, esp. C5/C6)
  WiFi.disconnect(true, true);
  delay(100);
  WiFi.mode(WIFI_OFF);
  delay(200);

  WiFi.mode(WIFI_AP);
  delay(100);

  // WPA2 PSK must be >= 8 chars; otherwise start OPEN AP
  const bool useOpen = (cfg.wardriverPsk.length() < 8);

  bool ok = false;
  if (useOpen) {
    Serial.println("[WIFI] AP password < 8 chars -> starting OPEN AP");
    ok = WiFi.softAP(cfg.wardriverSsid.c_str());
  } else {
    ok = WiFi.softAP(cfg.wardriverSsid.c_str(), cfg.wardriverPsk.c_str());
  }

  apStartMs = millis();
  apClientSeen = false;
  apWindowActive = true;
  apExtended = false;
  apExtendedStartMs = 0;
  apForceClose = false;

  Serial.print("[WIFI] AP SSID: "); Serial.println(cfg.wardriverSsid);
  Serial.print("[WIFI] AP PSK:  "); Serial.println(useOpen ? "(open)" : "(set)");
  Serial.print("[WIFI] AP start: "); Serial.println(ok ? "OK" : "FAIL");
  Serial.print("[WIFI] AP IP: "); Serial.println(WiFi.softAPIP());
}

// ---- STA ----

bool connectSTA(uint32_t timeoutMs) {
  Serial.println("[WIFI] STA connect attempt...");
  if (cfg.homeSsid.length() == 0) {
    Serial.println("[WIFI] No home SSID in config");
    return false;
  }

  Serial.print("[WIFI] Home SSID: ");
  Serial.println(cfg.homeSsid);

  // IMPORTANT: set DNS BEFORE WiFi.begin() so it sticks on ESP32-C6/C5
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

// ---- AP window enforcement ----

void stopAPIfAllowed() {
  if (!apWindowActive) return;

  // Decide whether the AP should close now.
  //   - apForceClose  : WebUI "Start Scan" button hit -> close immediately
  //   - apExtended    : a client connected within the first 60 s; use the
  //                     5 min rolling window instead of the 60 s budget
  //   - otherwise     : original 60 s behaviour
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

    WiFi.softAPdisconnect(true);
    apWindowActive = false;
    apExtended = false;
    apForceClose = false;

    // After AP shuts down, ensure STA is NOT trying to reconnect to home WiFi
    WiFi.setAutoReconnect(false);
    WiFi.persistent(false);
    WiFi.disconnect(true, true);
    delay(50);

    // If STA is not connected, keep WiFi in STA-only mode
    if (WiFi.status() != WL_CONNECTED) {
      WiFi.mode(WIFI_STA);
    }

    // IMPORTANT: After AP shuts off, scanning should begin (unless user forced OFF)
    if (!(userScanOverride && !scanningEnabled)) {
      scanningEnabled = true;
      Serial.println("[SCAN] AP timed out -> scanning re-enabled");
    } else {
      Serial.println("[SCAN] AP timed out -> scanning remains OFF (user override)");
    }

    Serial.println("[WIFI] AP stopped -> scanning eligible (if SD OK and not STA connected)");
  }
}

// ---- Auto-pause logic ----

bool shouldPauseScanning() {
  if (WiFi.status() == WL_CONNECTED) return true;

  wifi_mode_t m = WiFi.getMode();
  if (m == WIFI_AP || m == WIFI_AP_STA) {
    return true; // pause anytime AP is up
  }
  return false;
}
