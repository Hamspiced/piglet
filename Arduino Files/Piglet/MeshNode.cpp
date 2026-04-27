#include "MeshNode.h"
#include "Globals.h"
#include "GPS.h"
#include <esp_now.h>
#include "esp_wifi.h"

// ================================================================
//  Protocol constants
// ================================================================
const uint8_t  JCMK_ESPNOW_CH       = 6;
static const uint8_t  JCMK_MAGIC[4] = {'E','N','O','W'};
static const uint8_t  JCMK_BCAST[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
static const uint32_t JCMK_REQ_INIT_MS = 300;
static const uint32_t JCMK_REQ_MAX_MS  = 5000;
static const uint32_t JCMK_HB_MS       = 5000;
static const uint32_t JCMK_SCAN_MS     = 4500;
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
const uint8_t JCMK_CHANNELS[] = {
  1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14,
  36, 40, 44, 48, 52, 56, 60, 64,
  100, 112, 116, 120, 124, 128, 132, 136, 140, 144,
  149, 153, 157, 161, 165, 169, 173, 177
};
const uint8_t JCMK_NUM_CHANNELS = (uint8_t)(sizeof(JCMK_CHANNELS));

// ================================================================
//  Node state
// ================================================================
bool     meshNodeActive   = false;
bool     jcmkHaveCore     = false;
uint8_t  jcmkCoreMac[6]  = {0};
uint8_t  jcmkStartIdx    = 0;
uint8_t  jcmkEndIdx      = 0;
uint8_t  jcmkAssignVer   = 0;
uint32_t jcmkNetworksFound = 0;
uint32_t jcmkSentCount   = 0;

static uint32_t jcmkHbCounter   = 0;
static uint32_t jcmkLastHbMs    = 0;
static uint32_t jcmkLastReqMs   = 0;
static uint32_t jcmkReqInterval = JCMK_REQ_INIT_MS;
static uint32_t jcmkLastScanMs  = 0;

// Pending core-found event — written in ESP-Now callback, consumed in loop
static volatile bool  jcmkCoreFoundPending = false;
static uint8_t        jcmkCoreMacPending[6] = {0};

// ================================================================
//  Local helpers
// ================================================================
static String meshAuthModeToString(wifi_auth_mode_t m) {
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

// ================================================================
//  ESP-Now helpers
// ================================================================
static void jcmkSetChannel(uint8_t ch) {
  esp_wifi_set_ps(WIFI_PS_NONE);
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
  esp_wifi_set_promiscuous(false);
}

static bool jcmkAddPeer(const uint8_t* mac) {
  if (esp_now_is_peer_exist(mac)) esp_now_del_peer(mac);
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, mac, 6);
  peer.channel = 0;  // follow current home channel
  peer.encrypt = false;
  return (esp_now_add_peer(&peer) == ESP_OK);
}

static void jcmkSendCoreRequest() {
  jcmk_req_msg_t msg;
  memcpy(msg.magic, JCMK_MAGIC, 4);
  msg.type = JCMK_MSG_CORE_REQUEST;
  esp_now_send(JCMK_BCAST, (uint8_t*)&msg, sizeof(msg));
}

static void jcmkSendHeartbeat() {
  jcmk_hb_msg_t msg;
  memcpy(msg.magic, JCMK_MAGIC, 4);
  msg.type    = JCMK_MSG_HEARTBEAT;
  msg.counter = ++jcmkHbCounter;
  const uint8_t* dest = jcmkHaveCore ? jcmkCoreMac : JCMK_BCAST;
  esp_now_send(dest, (uint8_t*)&msg, sizeof(msg));
}

static void jcmkSendText(const String& s) {
  jcmk_text_msg_t msg;
  memcpy(msg.magic, JCMK_MAGIC, 4);
  msg.type    = JCMK_MSG_TEXT;
  msg.counter = jcmkHbCounter;
  uint16_t slen = (uint16_t)((s.length() < JCMK_TEXT_MAX) ? s.length() : JCMK_TEXT_MAX);
  msg.len = slen;
  memcpy(msg.text, s.c_str(), slen);
  msg.text[slen] = '\0';
  // magic(4) + type(1) + counter(4) + len(2) + text(slen+1)
  size_t pktLen = 11 + slen + 1;
  const uint8_t* dest = jcmkHaveCore ? jcmkCoreMac : JCMK_BCAST;
  esp_now_send(dest, (uint8_t*)&msg, pktLen);
}

// ESP-Now receive callback — called from WiFi task; keep it short
static void jcmkOnRecv(const esp_now_recv_info_t* info,
                        const uint8_t* data, int len) {
  if (len < 5) return;
  if (data[0] != 'E' || data[1] != 'N' || data[2] != 'O' || data[3] != 'W') return;
  uint8_t type = data[4];

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

// ================================================================
//  Scan and forward assigned channels to Core
// ================================================================
static void nodeDoScan() {
  int n = WiFi.scanNetworks(false, true);
  if (n > 0) {
    jcmkNetworksFound += (uint32_t)n;
    for (int i = 0; i < n; i++) {
      uint8_t ch = (uint8_t)WiFi.channel(i);
      bool inRange = false;
      for (uint8_t j = jcmkStartIdx; j <= jcmkEndIdx && j < JCMK_NUM_CHANNELS; j++) {
        if (JCMK_CHANNELS[j] == ch) { inRange = true; break; }
      }
      if (!inRange) continue;

      String bssid = WiFi.BSSIDstr(i);
      String ssid  = WiFi.SSID(i);
      String auth  = meshAuthModeToString(WiFi.encryptionType(i));
      int    rssi  = WiFi.RSSI(i);

      // JCMK node WiFi record format: BSSID,SSID,SECURITY,CHANNEL,RSSI,W
      String line = bssid + "," + ssid + "," + auth + ","
                  + String((int)ch) + "," + String(rssi) + ",W";
      jcmkSendText(line);
      jcmkSentCount++;
    }
    WiFi.scanDelete();
  } else {
    WiFi.scanDelete();
  }
  // Return radio to JCMK home channel so ESP-Now can transmit
  jcmkSetChannel(JCMK_ESPNOW_CH);

  // Drain GPS serial buffer that built up during the blocking scan
  while (GPSSerial.available()) gps.encode(GPSSerial.read());
}

// ================================================================
//  Lifecycle
// ================================================================
void enterNodeMode() {
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
  jcmkLastScanMs        = 0;
  jcmkStartIdx          = 0;
  jcmkEndIdx            = JCMK_NUM_CHANNELS - 1;
  jcmkAssignVer         = 0;

  // Soft WiFi reset — do NOT erase NVS credentials (eraseap=false)
  WiFi.softAPdisconnect(true);
  WiFi.disconnect(true, false);
  delay(100);
  WiFi.mode(WIFI_STA);
  delay(150);  // let the driver settle before touching the channel

  // Init ESP-Now FIRST, then lock the home channel.
  // Calling setChannel before esp_now_init() risks the driver
  // resetting the channel back during its own initialisation.
  esp_err_t err = esp_now_init();
  if (err != ESP_OK) {
    Serial.printf("[MESH] esp_now_init failed: %d\n", (int)err);
    return;
  }
  esp_now_register_recv_cb(jcmkOnRecv);

  // Lock radio to JCMK ESP-Now home channel AFTER init (matches JCMK pattern)
  delay(50);
  jcmkSetChannel(JCMK_ESPNOW_CH);

  jcmkAddPeer(JCMK_BCAST);

  meshNodeActive = true;
  Serial.println("[MESH] ESP-Now ready — searching for Core on ch 6...");
}

void exitNodeMode() {
  Serial.println("[MESH] Exiting node mode");
  if (meshNodeActive) esp_now_deinit();
  meshNodeActive = false;
  jcmkHaveCore   = false;

  // Full WiFi stack reset: OFF then STA gives a clean state after esp_now_deinit
  WiFi.mode(WIFI_OFF);
  delay(150);
  WiFi.mode(WIFI_STA);
  delay(100);

  // Drain GPS serial buffer accumulated during mesh mode scans
  while (GPSSerial.available()) gps.encode(GPSSerial.read());

  // Re-enable scanning so normal wardriving resumes immediately
  scanningEnabled = true;
}

// ================================================================
//  Loop tick
// ================================================================
void nodeModeTick() {
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

  // Broadcast core requests with exponential backoff until found
  if (!jcmkHaveCore && (now - jcmkLastReqMs >= jcmkReqInterval)) {
    jcmkLastReqMs  = now;
    jcmkSetChannel(JCMK_ESPNOW_CH);
    jcmkSendCoreRequest();
    jcmkReqInterval = (jcmkReqInterval * 2 > JCMK_REQ_MAX_MS)
                      ? JCMK_REQ_MAX_MS : jcmkReqInterval * 2;
  }

  // Heartbeat
  if (jcmkHaveCore && (now - jcmkLastHbMs >= JCMK_HB_MS)) {
    jcmkLastHbMs = now;
    jcmkSetChannel(JCMK_ESPNOW_CH);
    jcmkSendHeartbeat();
  }

  // Scan assigned channels and forward to Core
  if (jcmkHaveCore && (now - jcmkLastScanMs >= JCMK_SCAN_MS)) {
    jcmkLastScanMs = now;
    nodeDoScan();
  }
}

// ================================================================
//  OLED page renderer (page 5) — 128×64 SSD1306
//  Yellow band header (0..15): "Mesh"
//  Blue body (16..63): status rows at size 1 (8px/row)
// ================================================================
void drawPageMeshNode() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  // Header
  display.setTextSize(2);
  display.setCursor(0, 0);
  display.print("Mesh");
  display.drawFastHLine(0, 15, 128, SSD1306_WHITE);

  display.setTextSize(1);

  // Row 1 (y=16): link status
  display.setCursor(0, 17);
  if (!meshNodeActive) {
    display.print("Init error");
  } else if (!jcmkHaveCore) {
    display.print("Searching...");
  } else {
    display.print("Core linked");
  }

  // Row 2 (y=25): core MAC (xx:xx:xx:xx:xx:xx = 17 chars @ 6px = 102px — fits)
  display.setCursor(0, 26);
  if (jcmkHaveCore) {
    char macStr[18];
    snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
      jcmkCoreMac[0], jcmkCoreMac[1], jcmkCoreMac[2],
      jcmkCoreMac[3], jcmkCoreMac[4], jcmkCoreMac[5]);
    display.print(macStr);
  } else {
    display.print("--:--:--:--:--:--");
  }

  // Row 3 (y=34): channel assignment
  display.setCursor(0, 35);
  display.print("Ch:");
  if (jcmkAssignVer > 0 && jcmkStartIdx < JCMK_NUM_CHANNELS
                        && jcmkEndIdx   < JCMK_NUM_CHANNELS) {
    display.print(JCMK_CHANNELS[jcmkStartIdx]);
    display.print("-");
    display.print(JCMK_CHANNELS[jcmkEndIdx]);
  } else {
    display.print("all");
  }
  // ENOW home channel on the same row, right side
  display.setCursor(72, 35);
  display.print("ENOW:");
  display.print(JCMK_ESPNOW_CH);

  // Row 4 (y=44): networks found
  display.setCursor(0, 44);
  display.print("Found:");
  display.print(jcmkNetworksFound);

  // Row 5 (y=53): records sent to Core
  display.setCursor(0, 53);
  display.print("Sent:");
  display.print(jcmkSentCount);

  display.display();
}
