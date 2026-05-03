#include "MeshNode.h"
#include "Globals.h"
#include "GPS.h"
#include "SDUtils.h"
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
static const uint32_t JCMK_HB_MS          = 5000;
static const uint32_t NODE_SCAN_DWELL_MS  = 80;    // ms per channel (JCMK CHANNEL_TIMER)
static const uint32_t NODE_ADMIN_WIN_MS   = 300;   // ch-6 window after each full cycle
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

// Per-channel async scan state
static bool     nodeScanActive   = false;
static uint8_t  nodeScanChOffset = 0;
static bool     nodeScanAdminWin = false;
static uint32_t nodeScanAdminMs  = 0;

// Pending core-found event — written in ESP-Now callback, consumed in loop
static volatile bool  jcmkCoreFoundPending = false;
static uint8_t        jcmkCoreMacPending[6] = {0};

// ================================================================
//  Core mode state
// ================================================================
bool         meshCoreActive = false;
uint32_t     coreRecordsRx  = 0;
uint8_t      coreNodeCount  = 0;
CoreNodeInfo coreNodes[CORE_MAX_NODES] = {};

static uint8_t  coreAssignVer  = 0;
static uint32_t coreLastHbMs   = 0;
static uint32_t coreHbCounter  = 0;

static const uint32_t CORE_HB_MS        = 5000;
static const uint32_t CORE_NODE_TIMEOUT = 45000;  // 45 s — accounts for blocking scan latency

// Ring buffers: ESP-Now callback → main loop
#define CORE_REQ_QUEUE   4
#define CORE_TEXT_QUEUE 64   // large enough for burst from two nodes per cycle

struct CorReqSlot  { uint8_t mac[6]; bool isBiscuit; };
struct CorTextSlot { char    line[JCMK_TEXT_MAX + 1]; };

static CorReqSlot          coreReqBuf[CORE_REQ_QUEUE];
static volatile uint8_t    coreReqHead = 0, coreReqTail = 0;

static CorTextSlot         coreTextBuf[CORE_TEXT_QUEUE];
static volatile uint8_t    coreTextHead = 0, coreTextTail = 0;

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

// Node-mode sends always use full jcmk_text_msg_t (212 bytes).
// Biscuit Pro drops any packet < 212 bytes; JCMK Cores only check len >= 5
// so full-size packets are backward-compatible with both coordinators.

static void jcmkSendCoreRequest() {
  jcmk_text_msg_t msg = {};
  memcpy(msg.magic, JCMK_MAGIC, 4);
  msg.type    = JCMK_MSG_CORE_REQUEST;
  msg.counter = 0;
  msg.len     = 0;
  esp_now_send(JCMK_BCAST, (uint8_t*)&msg, sizeof(msg));
}

static void jcmkSendHeartbeat() {
  jcmk_text_msg_t msg = {};
  memcpy(msg.magic, JCMK_MAGIC, 4);
  msg.type    = JCMK_MSG_HEARTBEAT;
  msg.counter = ++jcmkHbCounter;
  msg.len     = 0;
  esp_now_send(JCMK_BCAST, (uint8_t*)&msg, sizeof(msg));
}

static void jcmkSendText(const String& s) {
  jcmk_text_msg_t msg = {};
  memcpy(msg.magic, JCMK_MAGIC, 4);
  msg.type    = JCMK_MSG_TEXT;
  msg.counter = jcmkHbCounter;
  uint16_t slen = (uint16_t)((s.length() < JCMK_TEXT_MAX) ? s.length() : JCMK_TEXT_MAX);
  msg.len = slen;
  memcpy(msg.text, s.c_str(), slen);
  msg.text[slen] = '\0';
  // Always send full struct size — Biscuit Pro drops variable-length packets < 212 bytes.
  esp_now_send(JCMK_BCAST, (uint8_t*)&msg, sizeof(msg));
}

// ================================================================
//  Core mode helpers (forward declarations used in jcmkOnRecv)
// ================================================================
static void coreSendReply(const uint8_t* mac) {
  // Use full-size jcmk_text_msg_t (212 bytes) instead of jcmk_req_msg_t (5 bytes).
  // Biscuit Node drops any packet < sizeof(enow_text_msg_t) = 212 bytes.
  // JCMK nodes only check len >= 5, so this is fully backward-compatible.
  jcmk_text_msg_t msg = {};
  memcpy(msg.magic, JCMK_MAGIC, 4);
  msg.type    = JCMK_MSG_CORE_REPLY;
  msg.counter = 0;
  msg.len     = 0;
  esp_now_send(mac, (uint8_t*)&msg, sizeof(msg));
}

// Send Biscuit Node a role assignment (MSG_ROLE_ASSIGN, type=5) then a channel
// config (MSG_CONFIG_UPDATE, type=10). Both packets must be full size (212 bytes).
// Called from coreReassignChannels / coreResendAdminToAll for isBiscuit nodes.
static void coreSendBiscuitRoleAndConfig(const uint8_t* mac, uint8_t startIdx, uint8_t endIdx) {
  // Step 1: role assignment — text[0] = 1 (ROLE_WIFI)
  // Biscuit MSG_ROLE_ASSIGN = type 5, same numeric value as JCMK_MSG_ADMIN.
  // After receiving this, Biscuit transitions from STATE_WAITING_ROLE to STATE_SCANNING.
  jcmk_text_msg_t roleMsg = {};
  memcpy(roleMsg.magic, JCMK_MAGIC, 4);
  roleMsg.type    = JCMK_MSG_ADMIN;  // = 5 = MSG_ROLE_ASSIGN on Biscuit
  roleMsg.counter = 0;
  roleMsg.len     = 1;
  roleMsg.text[0] = 1;  // ROLE_WIFI
  esp_now_send(mac, (uint8_t*)&roleMsg, sizeof(roleMsg));
  delay(10);

  // Step 2: channel config — "channels=1,2,...;dwell=80"
  // Biscuit MSG_CONFIG_UPDATE = type 10. Payload uses actual channel numbers
  // (not indices) in comma-separated list.
  jcmk_text_msg_t cfgMsg = {};
  memcpy(cfgMsg.magic, JCMK_MAGIC, 4);
  cfgMsg.type    = 10;  // MSG_CONFIG_UPDATE
  cfgMsg.counter = 0;

  String chList = "channels=";
  uint8_t end = (endIdx < JCMK_NUM_CHANNELS) ? endIdx : (JCMK_NUM_CHANNELS - 1);
  for (uint8_t j = startIdx; j <= end; j++) {
    if (j > startIdx) chList += ",";
    chList += String(JCMK_CHANNELS[j]);
  }
  chList += ";dwell=";
  chList += String((uint32_t)NODE_SCAN_DWELL_MS);

  uint16_t slen = (chList.length() < JCMK_TEXT_MAX)
                ? (uint16_t)chList.length() : (uint16_t)JCMK_TEXT_MAX;
  cfgMsg.len = slen;
  memcpy(cfgMsg.text, chList.c_str(), slen);
  cfgMsg.text[slen] = '\0';
  esp_now_send(mac, (uint8_t*)&cfgMsg, sizeof(cfgMsg));

  Serial.printf("[CORE] Biscuit role+config sent (ch %d-%d, dwell %dms)\n",
    JCMK_CHANNELS[startIdx],
    JCMK_CHANNELS[end],
    (int)NODE_SCAN_DWELL_MS);
}

// ESP-Now receive callback — handles both Node and Core roles
static void jcmkOnRecv(const esp_now_recv_info_t* info,
                        const uint8_t* data, int len) {
  if (len < 5) return;
  if (data[0] != 'E' || data[1] != 'N' || data[2] != 'O' || data[3] != 'W') return;
  uint8_t type = data[4];

  if (meshCoreActive) {
    // ---- Core role: handle requests from nodes ----
    if (type == JCMK_MSG_CORE_REQUEST) {
      coreSendReply(info->src_addr);  // reply immediately (safe from callback)
      uint8_t next = (coreReqTail + 1) % CORE_REQ_QUEUE;
      if (next != coreReqHead) {
        memcpy(coreReqBuf[coreReqTail].mac, info->src_addr, 6);
        // Biscuit Node always sends full-size packets (212 bytes); JCMK sends 5 bytes.
        coreReqBuf[coreReqTail].isBiscuit = (len >= (int)sizeof(jcmk_text_msg_t));
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
      // This handles reconnect: node has jcmkHaveCore=true and skips CORE_REQUEST,
      // but the Core can register it from any received packet (JCMK touchNode pattern).
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
          found = true; break;
        }
      }
      if (!found) {
        uint8_t nxt = (coreReqTail + 1) % CORE_REQ_QUEUE;
        if (nxt != coreReqHead) {
          memcpy(coreReqBuf[coreReqTail].mac, info->src_addr, 6);
          coreReqBuf[coreReqTail].isBiscuit = (len >= (int)sizeof(jcmk_text_msg_t));
          coreReqTail = nxt;
        }
      }
    }
  } else {
    // ---- Node role (existing) ----
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
}

// ================================================================
//  Core mode helpers (main loop only — not ISR-safe)
// ================================================================
static void coreFindOrAddNode(const uint8_t* mac, bool isBiscuit) {
  for (uint8_t i = 0; i < CORE_MAX_NODES; i++) {
    if (coreNodes[i].active && memcmp(coreNodes[i].mac, mac, 6) == 0) {
      coreNodes[i].lastHbMs = millis();
      return;  // already registered
    }
  }
  for (uint8_t i = 0; i < CORE_MAX_NODES; i++) {
    if (!coreNodes[i].active) {
      coreNodes[i].active    = true;
      coreNodes[i].lastHbMs  = millis();
      coreNodes[i].recordsRx = 0;
      coreNodes[i].isBiscuit = isBiscuit;
      memcpy(coreNodes[i].mac, mac, 6);
      coreNodeCount++;
      jcmkAddPeer(mac);
      Serial.printf("[CORE] New %s node %d: %02X:%02X:%02X:%02X:%02X:%02X\n",
        isBiscuit ? "Biscuit" : "JCMK",
        i, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
      return;
    }
  }
  Serial.println("[CORE] Max nodes reached");
}

static void coreReassignChannels() {
  // Build list of active slot indices
  uint8_t slots[CORE_MAX_NODES];
  uint8_t count = 0;
  for (uint8_t i = 0; i < CORE_MAX_NODES; i++)
    if (coreNodes[i].active) slots[count++] = i;
  if (count == 0) return;

  // Split JCMK_CHANNELS[] evenly; last node gets the remainder
  uint8_t perNode  = JCMK_NUM_CHANNELS / count;
  uint8_t startIdx = 0;
  for (uint8_t n = 0; n < count; n++) {
    coreNodes[slots[n]].startIdx = startIdx;
    coreNodes[slots[n]].endIdx   = (n < count - 1)
                                   ? (startIdx + perNode - 1)
                                   : (JCMK_NUM_CHANNELS - 1);
    startIdx += perNode;
  }
  coreAssignVer++;

  // Send assignment to each node using its protocol:
  // - Biscuit: MSG_ROLE_ASSIGN (type=5, text[0]=ROLE_WIFI) + MSG_CONFIG_UPDATE (type=10, channel string)
  // - JCMK:    jcmk_admin_msg_t (type=5, packed binary channel indices)
  for (uint8_t n = 0; n < count; n++) {
    uint8_t slot = slots[n];
    if (coreNodes[slot].isBiscuit) {
      coreSendBiscuitRoleAndConfig(coreNodes[slot].mac,
                                   coreNodes[slot].startIdx,
                                   coreNodes[slot].endIdx);
    } else {
      jcmk_admin_msg_t msg;
      memcpy(msg.magic, JCMK_MAGIC, 4);
      msg.type               = JCMK_MSG_ADMIN;
      msg.node_count         = count;
      msg.assignment_version = coreAssignVer;
      msg.node_index         = n;
      msg.start_channel_idx  = coreNodes[slot].startIdx;
      msg.end_channel_idx    = coreNodes[slot].endIdx;
      esp_now_send(coreNodes[slot].mac, (uint8_t*)&msg, sizeof(msg));
    }
    delay(10);
  }
  Serial.printf("[CORE] Reassigned channels: %d nodes v%d\n", count, coreAssignVer);
}

// Re-send the current ADMIN assignment to every node.
// Called periodically so nodes that missed the update (e.g. mid-scan) recover.
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
      jcmk_admin_msg_t msg;
      memcpy(msg.magic, JCMK_MAGIC, 4);
      msg.type               = JCMK_MSG_ADMIN;
      msg.node_count         = coreNodeCount;
      msg.assignment_version = coreAssignVer;
      msg.node_index         = n;
      msg.start_channel_idx  = coreNodes[i].startIdx;
      msg.end_channel_idx    = coreNodes[i].endIdx;
      esp_now_send(coreNodes[i].mac, (uint8_t*)&msg, sizeof(msg));
    }
    n++;
    delay(10);
  }
}

static void coreSendHeartbeatToAll() {
  // Use full-size jcmk_text_msg_t (212 bytes) so Biscuit Node accepts it
  // (Biscuit drops packets < 212 bytes). JCMK nodes only need len >= 5.
  jcmk_text_msg_t msg = {};
  memcpy(msg.magic, JCMK_MAGIC, 4);
  msg.type    = JCMK_MSG_HEARTBEAT;
  msg.counter = ++coreHbCounter;
  msg.len     = 0;
  for (uint8_t i = 0; i < CORE_MAX_NODES; i++)
    if (coreNodes[i].active)
      esp_now_send(coreNodes[i].mac, (uint8_t*)&msg, sizeof(msg));
}

static void coreParseAndLogText(const char* line) {
  // JCMK format: BSSID,SSID,AUTH,CHANNEL,RSSI,W
  String s(line);
  int p0 = s.indexOf(',');                if (p0 < 0) return;
  int p1 = s.indexOf(',', p0 + 1);       if (p1 < 0) return;
  int p2 = s.indexOf(',', p1 + 1);       if (p2 < 0) return;
  int p3 = s.indexOf(',', p2 + 1);       if (p3 < 0) return;

  String bssid = s.substring(0,      p0);
  String ssid  = s.substring(p0 + 1, p1);
  String auth  = s.substring(p1 + 1, p2);
  int    ch    = s.substring(p2 + 1, p3).toInt();
  int    rssi  = s.substring(p3 + 1).toInt();  // toInt() stops at next comma

  double lat = 0, lon = 0, altM = 0, accM = 0;
  if (gpsHasFix) {
    lat  = gps.location.lat();
    lon  = gps.location.lng();
    altM = gps.altitude.meters();
    accM = gps.hdop.hdop();
  }
  appendWigleRow(bssid, ssid, auth, iso8601NowUTC(), ch, rssi, lat, lon, altM, accM);
  coreRecordsRx++;
}

// ================================================================
//  Per-channel async scan (JCMK startNextNodeAssignedScan pattern).
//  Scans one assigned channel per call at NODE_SCAN_DWELL_MS dwell,
//  cycles through the full assigned range, then enters a ch-6 admin
//  window (heartbeat + NODE_ADMIN_WIN_MS) before the next cycle.
// ================================================================
static void nodeDoScanTick() {
  uint8_t numCh = (jcmkEndIdx >= jcmkStartIdx)
                ? (jcmkEndIdx - jcmkStartIdx + 1) : 0;
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
      while (GPSSerial.available()) gps.encode(GPSSerial.read());
      if (jcmkHaveCore) { jcmkSendHeartbeat(); jcmkLastHbMs = millis(); }
      nodeScanAdminWin = true;
      nodeScanAdminMs  = millis();
      return;
    }

    uint8_t chIdx = jcmkStartIdx + nodeScanChOffset;
    if (chIdx >= JCMK_NUM_CHANNELS) { nodeScanChOffset++; return; }
    uint8_t channel = JCMK_CHANNELS[chIdx];

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
    // Return to ch 6 before any ESP-Now send (JCMK sendBroadcastStringPlain pattern).
    // The scan left the radio on the scan channel; Core listens only on ch 6.
    jcmkSetChannel(JCMK_ESPNOW_CH);
    for (int i = 0; i < n; i++) {
      String bssid = WiFi.BSSIDstr(i);
      String ssid  = WiFi.SSID(i);
      String auth  = meshAuthModeToString(WiFi.encryptionType(i));
      int    ch    = WiFi.channel(i);
      int    rssi  = WiFi.RSSI(i);
      String line  = bssid + "," + ssid + "," + auth + ","
                   + String(ch) + "," + String(rssi) + ",W";
      jcmkSendText(line);
      jcmkSentCount++;
    }
  }
  WiFi.scanDelete();
  nodeScanActive = false;
  nodeScanChOffset++;
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
  jcmkStartIdx          = 0;
  jcmkEndIdx            = JCMK_NUM_CHANNELS - 1;
  jcmkAssignVer         = 0;
  nodeScanActive        = false;
  nodeScanChOffset      = 0;
  nodeScanAdminWin      = false;

  // Mesh mode owns the WiFi stack — prevent stopAPIfAllowed() from firing
  // WiFi.disconnect(true,true) after esp_now_init() would kill the ESP-Now driver.
  apWindowActive = false;

  // Soft WiFi reset — do NOT erase NVS credentials (eraseap=false)
  WiFi.softAPdisconnect(true);
  WiFi.disconnect(true, false);
  // Prevent STA auto-reconnect from moving the radio off ch 6 while ESP-Now runs.
  // A background reconnect to the home AP changes the radio channel and silently
  // kills ESP-Now receive (Core stops seeing node heartbeats → 20 s timeout).
  WiFi.setAutoReconnect(false);
  WiFi.persistent(false);
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

  // Random stagger before first scan (JCMK begin() pattern: delay(random(100,5000))).
  // Prevents multiple nodes starting cycles in sync and flooding simultaneously.
  delay(random(200, 3000));

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
  WiFi.setAutoReconnect(true);  // restore auto-reconnect for normal wardriving
  WiFi.mode(WIFI_STA);
  delay(100);

  // Drain GPS serial buffer accumulated during mesh mode scans
  while (GPSSerial.available()) gps.encode(GPSSerial.read());

  // Re-enable scanning so normal wardriving resumes immediately
  scanningEnabled = true;
}

// ================================================================
//  Core mode lifecycle
// ================================================================
void enterCoreMode() {
  Serial.println("[CORE] Entering Core mode");

  meshCoreActive = false;
  coreRecordsRx  = 0;
  coreNodeCount  = 0;
  coreAssignVer  = 0;
  coreHbCounter  = 0;
  coreLastHbMs   = 0;
  coreReqHead = coreReqTail = 0;
  coreTextHead = coreTextTail = 0;
  memset(coreNodes, 0, sizeof(coreNodes));

  // Mesh mode owns the WiFi stack — prevent stopAPIfAllowed() from firing
  // WiFi.disconnect(true,true) after esp_now_init() would kill the ESP-Now driver.
  apWindowActive = false;

  WiFi.softAPdisconnect(true);
  WiFi.disconnect(true, false);
  // Prevent STA auto-reconnect from moving the radio off ch 6 while ESP-Now runs.
  WiFi.setAutoReconnect(false);
  WiFi.persistent(false);
  delay(100);
  WiFi.mode(WIFI_STA);
  delay(150);

  esp_err_t err = esp_now_init();
  if (err != ESP_OK) {
    Serial.printf("[CORE] esp_now_init failed: %d\n", (int)err);
    return;
  }
  esp_now_register_recv_cb(jcmkOnRecv);
  delay(50);
  jcmkSetChannel(JCMK_ESPNOW_CH);
  jcmkAddPeer(JCMK_BCAST);

  meshCoreActive = true;
  Serial.println("[CORE] Ready — listening for nodes on ch 6");
}

void exitCoreMode() {
  Serial.println("[CORE] Exiting Core mode");
  if (meshCoreActive) esp_now_deinit();
  meshCoreActive = false;
  coreNodeCount  = 0;

  WiFi.mode(WIFI_OFF);
  delay(150);
  WiFi.setAutoReconnect(true);  // restore auto-reconnect for normal wardriving
  WiFi.mode(WIFI_STA);
  delay(100);

  while (GPSSerial.available()) gps.encode(GPSSerial.read());
  scanningEnabled = true;
}

void coreModeTick() {
  if (!meshCoreActive) return;
  uint32_t now = millis();

  // 1. Process pending CORE_REQUEST queue (new node registrations)
  while (coreReqHead != coreReqTail) {
    uint8_t i = coreReqHead;
    coreReqHead = (coreReqHead + 1) % CORE_REQ_QUEUE;
    coreFindOrAddNode(coreReqBuf[i].mac, coreReqBuf[i].isBiscuit);
    coreReassignChannels();
  }

  // 2. Process pending TEXT records
  while (coreTextHead != coreTextTail) {
    uint8_t i = coreTextHead;
    coreTextHead = (coreTextHead + 1) % CORE_TEXT_QUEUE;
    coreParseAndLogText(coreTextBuf[i].line);
  }

  // 3. Periodic heartbeat + ADMIN refresh to all connected nodes.
  // Nodes that missed the ADMIN while scanning will recover within one cycle.
  if (now - coreLastHbMs >= CORE_HB_MS) {
    coreLastHbMs = now;
    coreSendHeartbeatToAll();
    coreResendAdminToAll();
  }

  // 4. Node timeout check
  bool changed = false;
  for (uint8_t i = 0; i < CORE_MAX_NODES; i++) {
    if (coreNodes[i].active && (now - coreNodes[i].lastHbMs) > CORE_NODE_TIMEOUT) {
      Serial.printf("[CORE] Node %d timed out\n", i);
      esp_now_del_peer(coreNodes[i].mac);
      memset(&coreNodes[i], 0, sizeof(CoreNodeInfo));
      coreNodeCount--;
      changed = true;
    }
  }
  if (changed) coreReassignChannels();
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

  // Per-channel async scan — runs continuously while connected to Core
  if (jcmkHaveCore) nodeDoScanTick();
}

// ================================================================
//  OLED page renderer (page 5) — handles both Node and Core mode
// ================================================================
void drawPageMeshNode() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(2);
  display.setCursor(0, 0);

  if (meshCoreActive) {
    // ---- Core mode ----
    display.print("CORE");
    // Node count in small text next to header
    display.setTextSize(1);
    char nb[5]; snprintf(nb, sizeof(nb), "[%d]", coreNodeCount);
    display.setCursor(52, 5);
    display.print(nb);
    display.drawFastHLine(0, 15, 128, SSD1306_WHITE);

    display.setTextSize(1);

    // Row 1 (y=17): status
    display.setCursor(0, 17);
    if (coreNodeCount == 0) {
      display.print("Waiting for nodes");
    } else {
      char buf[22];
      snprintf(buf, sizeof(buf), "%d node%s connected",
               coreNodeCount, coreNodeCount == 1 ? "" : "s");
      display.print(buf);
    }

    // Rows 2-3 (y=26, y=35): first 2 active node slots
    uint8_t shown = 0;
    for (uint8_t i = 0; i < CORE_MAX_NODES && shown < 2; i++) {
      int y = (shown == 0) ? 26 : 35;
      display.setCursor(0, y);
      if (coreNodes[i].active) {
        uint8_t* m = coreNodes[i].mac;
        uint8_t si = coreNodes[i].startIdx, ei = coreNodes[i].endIdx;
        char buf[22];
        snprintf(buf, sizeof(buf), "%02X:%02X:%02X %d-%d",
                 m[3], m[4], m[5],
                 (si < JCMK_NUM_CHANNELS) ? JCMK_CHANNELS[si] : 0,
                 (ei < JCMK_NUM_CHANNELS) ? JCMK_CHANNELS[ei] : 0);
        display.print(buf);
        shown++;
      }
    }
    if (shown == 0) {
      display.setCursor(0, 26); display.print("--:--:-- ------");
      display.setCursor(0, 35); display.print("--:--:-- ------");
    } else if (shown == 1) {
      display.setCursor(0, 35); display.print("--:--:-- ------");
    }

    // Row 4 (y=44): total records received
    display.setCursor(0, 44);
    char rbuf[22];
    snprintf(rbuf, sizeof(rbuf), "Rcvd: %lu", (unsigned long)coreRecordsRx);
    display.print(rbuf);

    // Row 5 (y=53): GPS + hint
    display.setCursor(0, 53);
    display.print(gpsHasFix ? "GPS:FIX" : "GPS:---");
    display.setCursor(62, 53);
    display.print("Hold=Node");

  } else {
    // ---- Node mode (original layout) ----
    display.print("Mesh");
    display.drawFastHLine(0, 15, 128, SSD1306_WHITE);
    display.setTextSize(1);

    // Row 1 (y=17): link status
    display.setCursor(0, 17);
    if (!meshNodeActive) {
      display.print("Init error");
    } else if (!jcmkHaveCore) {
      display.print("Searching...");
    } else {
      display.print("Core linked");
    }

    // Row 2 (y=26): core MAC
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

    // Row 3 (y=35): channel assignment + ENOW ch
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
    display.setCursor(72, 35);
    display.print("ENOW:");
    display.print(JCMK_ESPNOW_CH);

    // Row 4 (y=44): networks found
    display.setCursor(0, 44);
    display.print("Found:");
    display.print(jcmkNetworksFound);

    // Row 5 (y=53): records sent + hint
    display.setCursor(0, 53);
    display.print("Sent:");
    display.print(jcmkSentCount);
    display.setCursor(66, 53);
    display.print("Hold=Core");
  }

  display.display();
}
