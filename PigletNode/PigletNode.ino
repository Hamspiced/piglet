/*
 * PigletNode — Standalone ESP-Now Mesh Node
 * Target: Seeed XIAO ESP32-C5
 *
 * Boots directly as a JCMK-compatible mesh scanning node.
 * No display, no GPS, no SD card required.
 * Power it on near a Piglet running in Core mode and it will
 * auto-pair, receive a channel assignment, and begin scanning.
 *
 * Compatible with:
 *   - Piglet (XIAO ESP32-S3/C5/C6) in Core mode
 *   - Piglet T-Dongle C5 in Core mode
 *   - justcallmekoko/ESP32Marauder (core role)
 *
 * License: CC BY-NC-SA 4.0
 */

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include "esp_wifi.h"

#define FIRMWARE_VERSION "v2.57"

// ================================================================
//  Board — XIAO ESP32-C5
// ================================================================

// BOOT / user button (GPIO0, INPUT_PULLUP, LOW = pressed).
// Hold >2 s to force a re-search (useful when moving between Cores).
static const int BTN_PIN = 0;

// Built-in user LED — active LOW on all XIAO variants.
// The board package defines LED_BUILTIN; fall back to GPIO15 if not set.
#ifndef LED_BUILTIN
  #define LED_BUILTIN 15
#endif
static const bool LED_ACTIVE_LOW = true;

// ================================================================
//  JCMK protocol constants  (must match Piglet Core exactly)
// ================================================================
static const uint8_t  ESPNOW_CH           = 6;
static const uint8_t  JCMK_MAGIC[4]       = {'E', 'N', 'O', 'W'};
static const uint8_t  JCMK_BCAST[6]       = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
static const uint32_t REQ_INIT_MS         = 300;
static const uint32_t REQ_MAX_MS          = 5000;
static const uint32_t HB_MS              = 5000;
static const uint32_t SCAN_DWELL_MS      = 80;   // ms per channel
static const uint32_t ADMIN_WIN_MS       = 500;  // ch-6 window after each scan cycle
static const uint32_t CORE_TIMEOUT_MS    = 90000; // re-search if no ADMIN for 90 s
#define JCMK_TEXT_MAX 200

enum JcmkMsgType : uint8_t {
  MSG_CORE_REQUEST = 1,
  MSG_CORE_REPLY   = 2,
  MSG_HEARTBEAT    = 3,
  MSG_TEXT         = 4,
  MSG_ADMIN        = 5
};

typedef struct __attribute__((packed)) {
  char     magic[4];
  uint8_t  type;
  uint32_t counter;
  uint16_t len;
  char     text[JCMK_TEXT_MAX + 1];
} jcmk_text_msg_t;  // 212 bytes — full size required by Biscuit Node/Pro

typedef struct __attribute__((packed)) {
  char    magic[4];
  uint8_t type;
  uint8_t assignment_version;
  uint8_t node_index;
  uint8_t node_count;
  uint8_t start_channel_idx;
  uint8_t end_channel_idx;
} jcmk_admin_msg_t;

// JCMK dual-band channel table (2.4 GHz + 5 GHz)
// Indices are shared with the Core — do NOT reorder.
static const uint8_t CHANNELS[] = {
  // 2.4 GHz (ch 1-14)
  1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14,
  // 5 GHz UNII-1/2/2e/3
  36, 40, 44, 48, 52, 56, 60, 64,
  100, 112, 116, 120, 124, 128, 132, 136, 140, 144,
  149, 153, 157, 161, 165, 169, 173, 177
};
static const uint8_t NUM_CHANNELS = (uint8_t)(sizeof(CHANNELS));

// ================================================================
//  Node state
// ================================================================
static bool     nodeActive    = false;
static bool     haveCore      = false;
static uint8_t  coreMac[6]   = {0};
static uint8_t  startIdx      = 0;
static uint8_t  endIdx        = NUM_CHANNELS - 1;  // default = all channels
static uint8_t  assignVer     = 0;
static uint32_t netFound      = 0;
static uint32_t netSent       = 0;
static uint32_t hbCounter     = 0;
static uint32_t lastHbMs      = 0;
static uint32_t lastReqMs     = 0;
static uint32_t reqInterval   = REQ_INIT_MS;
static uint32_t lastAdminMs   = 0;  // tracks last ADMIN from Core for timeout

// Per-channel scan state
static bool     scanActive    = false;
static uint8_t  scanChOffset  = 0;
static bool     scanAdminWin  = false;
static uint32_t scanAdminMs   = 0;

// Pending core-found event (set from ESP-Now callback, consumed in loop)
static volatile bool coreFoundPending  = false;
static uint8_t       coreMacPending[6] = {0};

// ================================================================
//  LED
// ================================================================
static void ledWrite(bool on) {
  digitalWrite(LED_BUILTIN, LED_ACTIVE_LOW ? !on : on);
}

// Non-blocking blink: fast (200 ms) = searching, slow (1000 ms) = linked
static void ledTick() {
  static uint32_t lastMs = 0;
  static bool     state  = false;
  uint32_t now = millis();
  uint32_t interval = haveCore ? 1000 : 200;
  if (now - lastMs >= interval) {
    lastMs = now;
    state  = !state;
    ledWrite(state);
  }
}

// ================================================================
//  ESP-Now / WiFi helpers
// ================================================================
static void setChannel(uint8_t ch) {
  esp_wifi_set_ps(WIFI_PS_NONE);
  // Do NOT use promiscuous mode: on IDF 5.x, disabling promiscuous after
  // a prior STA connection causes the driver to revert to the home router channel.
  esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
}

static void addPeer(const uint8_t* mac) {
  if (esp_now_is_peer_exist(mac)) esp_now_del_peer(mac);
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, mac, 6);
  peer.channel = 0;
  peer.encrypt = false;
  esp_now_add_peer(&peer);
}

static void sendRequest() {
  jcmk_text_msg_t msg = {};
  memcpy(msg.magic, JCMK_MAGIC, 4);
  msg.type = MSG_CORE_REQUEST;
  esp_now_send(JCMK_BCAST, (uint8_t*)&msg, sizeof(msg));
}

static void sendHeartbeat() {
  jcmk_text_msg_t msg = {};
  memcpy(msg.magic, JCMK_MAGIC, 4);
  msg.type    = MSG_HEARTBEAT;
  msg.counter = ++hbCounter;
  esp_now_send(JCMK_BCAST, (uint8_t*)&msg, sizeof(msg));
}

static void sendText(const String& s) {
  jcmk_text_msg_t msg = {};
  memcpy(msg.magic, JCMK_MAGIC, 4);
  msg.type    = MSG_TEXT;
  msg.counter = hbCounter;
  uint16_t slen = (uint16_t)((s.length() < JCMK_TEXT_MAX) ? s.length() : JCMK_TEXT_MAX);
  msg.len = slen;
  memcpy(msg.text, s.c_str(), slen);
  msg.text[slen] = '\0';
  // Always full struct size — Biscuit Pro drops packets < 212 bytes.
  esp_now_send(JCMK_BCAST, (uint8_t*)&msg, sizeof(msg));
}

static String authStr(wifi_auth_mode_t m) {
  switch (m) {
    case WIFI_AUTH_OPEN:            return "OPEN";
    case WIFI_AUTH_WEP:             return "WEP";
    case WIFI_AUTH_WPA_PSK:         return "WPA";
    case WIFI_AUTH_WPA2_PSK:        return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK:    return "WPAWPA2";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2EAP";
    case WIFI_AUTH_WPA3_PSK:        return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK:   return "WPA2WPA3";
    default:                        return "UNKNOWN";
  }
}

// ================================================================
//  ESP-Now receive callback (runs in ISR context — keep minimal)
// ================================================================
static void onRecv(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
  if (len < 5) return;
  if (data[0]!='E' || data[1]!='N' || data[2]!='O' || data[3]!='W') return;
  uint8_t type = data[4];

  if (type == MSG_CORE_REPLY && !haveCore && !coreFoundPending) {
    memcpy(coreMacPending, info->src_addr, 6);
    coreFoundPending = true;

  } else if (type == MSG_ADMIN && len >= (int)sizeof(jcmk_admin_msg_t)) {
    // ---- JCMK binary admin (Piglet Core, JCMK Core) ----
    // Layout: magic[4], type, assignment_version, node_index, node_count,
    //         start_channel_idx, end_channel_idx
    // Only update if the assignment version has advanced (prevents stale replays).
    const jcmk_admin_msg_t* adm = (const jcmk_admin_msg_t*)data;
    if (adm->assignment_version != assignVer) {
      assignVer = adm->assignment_version;
      startIdx  = adm->start_channel_idx;
      endIdx    = adm->end_channel_idx;
    }
    lastAdminMs = millis();

  } else if (type == 10 && len >= 11) {
    // ---- Biscuit MSG_CONFIG_UPDATE (type 10) ----
    // Payload: jcmk_text_msg_t with text = "channels=1,2,...;dwell=N"
    // Parse the channel list and map actual channel numbers back to CHANNELS[] indices.
    const jcmk_text_msg_t* tm = (const jcmk_text_msg_t*)data;
    uint16_t slen = (tm->len < JCMK_TEXT_MAX) ? tm->len : JCMK_TEXT_MAX;
    char buf[JCMK_TEXT_MAX + 1];
    memcpy(buf, tm->text, slen);
    buf[slen] = '\0';

    // Find "channels=" prefix
    const char* chStart = strstr(buf, "channels=");
    if (chStart) {
      chStart += 9;  // skip "channels="
      uint8_t first = 0xFF, last = 0xFF;
      const char* p = chStart;
      while (*p && *p != ';') {
        int ch = atoi(p);
        // Find index of this channel number in CHANNELS[]
        for (uint8_t i = 0; i < NUM_CHANNELS; i++) {
          if (CHANNELS[i] == (uint8_t)ch) {
            if (first == 0xFF) first = i;
            last = i;
            break;
          }
        }
        // Advance past this number
        while (*p && *p != ',' && *p != ';') p++;
        if (*p == ',') p++;
      }
      if (first != 0xFF) {
        startIdx  = first;
        endIdx    = last;
        assignVer = (assignVer == 255) ? 1 : assignVer + 1;  // mark as assigned
        Serial.printf("[NODE] Biscuit config: ch %d-%d (idx %d-%d)\n",
          CHANNELS[startIdx], CHANNELS[endIdx], startIdx, endIdx);
      }
    }
    lastAdminMs = millis();

  } else if (type == MSG_HEARTBEAT && haveCore) {
    // Core is still alive — reset timeout
    lastAdminMs = millis();
  }
}

// ================================================================
//  Re-enter searching state (called on boot and on Core timeout)
// ================================================================
static void resetToSearching() {
  haveCore     = false;
  assignVer    = 0;
  startIdx     = 0;
  endIdx       = NUM_CHANNELS - 1;  // default = all channels until Core assigns a range
  hbCounter    = 0;
  lastHbMs     = 0;
  lastReqMs    = 0;
  reqInterval  = REQ_INIT_MS;
  lastAdminMs  = 0;
  scanActive   = false;
  scanChOffset = 0;
  scanAdminWin = false;
  memset(coreMac, 0, 6);
  coreFoundPending = false;

  // Return to home channel and re-broadcast search
  setChannel(ESPNOW_CH);
  Serial.println("[NODE] Searching for Core on ch 6...");
}

// ================================================================
//  Per-channel async scan tick
// ================================================================
static void scanTick() {
  uint8_t numCh = (endIdx >= startIdx) ? (endIdx - startIdx + 1) : 0;
  if (numCh == 0) return;

  // Admin window: radio is on ch 6, waiting before next cycle
  if (scanAdminWin) {
    if (millis() - scanAdminMs >= ADMIN_WIN_MS) {
      scanAdminWin = false;
      scanChOffset = 0;  // begin next cycle
    }
    return;
  }

  if (!scanActive) {
    // Full cycle done — hop to ch 6, heartbeat, then wait admin window
    if (scanChOffset >= numCh) {
      setChannel(ESPNOW_CH);
      sendHeartbeat();
      lastHbMs     = millis();
      scanAdminWin = true;
      // Random jitter (0-200 ms) staggers admin windows across nodes
      scanAdminMs  = millis() + (uint32_t)(esp_random() % 200);
      return;
    }

    uint8_t chIdx = startIdx + scanChOffset;
    if (chIdx >= NUM_CHANNELS) { scanChOffset++; return; }
    uint8_t channel = CHANNELS[chIdx];

    int16_t rc = WiFi.scanNetworks(/*async*/true, /*hidden*/true,
                                   /*passive*/false, SCAN_DWELL_MS, channel);
    if (rc == WIFI_SCAN_RUNNING || rc == 0) {
      scanActive = true;
    } else {
      scanChOffset++;  // channel failed — skip it
    }
    return;
  }

  // Poll for scan completion
  int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) return;

  if (n > 0) {
    netFound += (uint32_t)n;
    // Return to ch 6 before sending (Core only listens on ch 6)
    setChannel(ESPNOW_CH);
    for (int i = 0; i < n; i++) {
      String line = WiFi.BSSIDstr(i) + "," + WiFi.SSID(i) + ","
                  + authStr(WiFi.encryptionType(i)) + ","
                  + String(WiFi.channel(i)) + "," + String(WiFi.RSSI(i)) + ",W";
      sendText(line);
      netSent++;
    }
  }
  WiFi.scanDelete();
  scanActive = false;
  scanChOffset++;
}

// ================================================================
//  Main node tick — call every loop()
// ================================================================
static void nodeTick() {
  if (!nodeActive) return;
  uint32_t now = millis();

  // --- Consume pending core-found event from callback ---
  if (coreFoundPending) {
    coreFoundPending = false;
    memcpy(coreMac, coreMacPending, 6);
    haveCore    = true;
    reqInterval = REQ_INIT_MS;
    lastAdminMs = now;
    addPeer(coreMac);
    Serial.printf("[NODE] Core found: %02X:%02X:%02X:%02X:%02X:%02X\n",
      coreMac[0], coreMac[1], coreMac[2], coreMac[3], coreMac[4], coreMac[5]);
  }

  // --- Core timeout: re-search if ADMIN/HB silent for CORE_TIMEOUT_MS ---
  if (haveCore && lastAdminMs > 0 && (now - lastAdminMs) >= CORE_TIMEOUT_MS) {
    Serial.println("[NODE] Core timed out — re-searching...");
    WiFi.scanDelete();
    resetToSearching();
    return;
  }

  // --- CORE_REQUEST with exponential backoff (only when radio is free) ---
  if (!haveCore && !scanActive && (now - lastReqMs >= reqInterval)) {
    lastReqMs  = now;
    setChannel(ESPNOW_CH);
    sendRequest();
    reqInterval = (reqInterval * 2 > REQ_MAX_MS) ? REQ_MAX_MS : reqInterval * 2;
  }

  // --- Heartbeat backup (fires if scan stalls) ---
  if (haveCore && !scanActive && (now - lastHbMs >= HB_MS)) {
    lastHbMs = now;
    setChannel(ESPNOW_CH);
    sendHeartbeat();
  }

  // --- Channel scan (runs continuously once paired) ---
  if (haveCore) scanTick();
}

// ================================================================
//  Periodic serial status (every 10 s)
// ================================================================
static void statusTick() {
  static uint32_t lastMs = 0;
  if (millis() - lastMs < 10000) return;
  lastMs = millis();

  if (!haveCore) {
    Serial.printf("[NODE] Searching... (req interval %lu ms)\n",
                  (unsigned long)reqInterval);
  } else {
    Serial.printf("[NODE] Core %02X:%02X:%02X:%02X:%02X:%02X | "
                  "ch %d-%d (v%d) | found %lu | sent %lu\n",
      coreMac[0], coreMac[1], coreMac[2], coreMac[3], coreMac[4], coreMac[5],
      (startIdx < NUM_CHANNELS) ? CHANNELS[startIdx] : 0,
      (endIdx   < NUM_CHANNELS) ? CHANNELS[endIdx]   : 0,
      assignVer,
      (unsigned long)netFound,
      (unsigned long)netSent);
  }
}

// ================================================================
//  Button: hold >2 s to force a re-search
// ================================================================
static void buttonTick() {
  static bool     pressing      = false;
  static bool     triggered     = false;
  static uint32_t pressStartMs  = 0;

  bool low = (digitalRead(BTN_PIN) == LOW);

  if (low && !pressing) {
    pressing = true; triggered = false; pressStartMs = millis();
  }
  if (!low) {
    pressing = false; triggered = false;
  }
  if (pressing && !triggered && (millis() - pressStartMs >= 2000)) {
    triggered = true;
    Serial.println("[BTN] Long press — re-searching for Core...");
    WiFi.scanDelete();
    resetToSearching();
  }
}

// ================================================================
//  setup()
// ================================================================
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println();
  Serial.println("=== PigletNode Boot ===");
  Serial.printf("JCMK channels: %d  |  home ch: %d\n", NUM_CHANNELS, ESPNOW_CH);

  // LED
  pinMode(LED_BUILTIN, OUTPUT);
  ledWrite(false);

  // Button
  pinMode(BTN_PIN, INPUT_PULLUP);

  // WiFi: full deinit → clean STA reinit.
  // WiFi.disconnect(wifioff=true) only calls esp_wifi_stop(), leaving the
  // radio in a stopped state so setChannel() silently fails afterwards.
  // WiFi.mode(WIFI_OFF) calls esp_wifi_deinit() for a true clean slate.
  WiFi.mode(WIFI_OFF);   // full deinit
  delay(100);
  WiFi.mode(WIFI_STA);   // clean reinit — radio is now running
  WiFi.setAutoReconnect(false);
  WiFi.persistent(false);
  delay(100);

  // Init ESP-Now
  esp_err_t err = esp_now_init();
  if (err != ESP_OK) {
    Serial.printf("[NODE] esp_now_init FAILED: %d\n", (int)err);
    // Rapid blink = fatal error; power-cycle to retry
    while (true) {
      ledWrite(true);  delay(50);
      ledWrite(false); delay(50);
    }
  }
  esp_now_register_recv_cb(onRecv);

  // Lock radio to JCMK home channel AFTER init
  delay(50);
  setChannel(ESPNOW_CH);
  // Verify channel stuck (helps diagnose if radio wasn't running)
  { uint8_t pri; wifi_second_chan_t sec; esp_wifi_get_channel(&pri, &sec);
    Serial.printf("[NODE] Channel after set: %d (target=%d)%s\n", pri, ESPNOW_CH,
      (pri == ESPNOW_CH) ? " OK" : " *** MISMATCH!");
    if (pri != ESPNOW_CH) { delay(50); setChannel(ESPNOW_CH);
      esp_wifi_get_channel(&pri, &sec);
      Serial.printf("[NODE] Channel after retry: %d\n", pri); }
  }
  addPeer(JCMK_BCAST);

  // Random stagger (200-3000 ms) so multiple nodes don't flood the Core together
  uint32_t stagger = (uint32_t)(esp_random() % 2800) + 200;
  Serial.printf("[NODE] Stagger %lu ms...\n", (unsigned long)stagger);
  delay(stagger);

  nodeActive = true;
  Serial.println("[NODE] Ready — searching for Core on ch 6...");
}

// ================================================================
//  loop()
// ================================================================
void loop() {
  nodeTick();
  ledTick();
  buttonTick();
  statusTick();
  delay(10);
}
