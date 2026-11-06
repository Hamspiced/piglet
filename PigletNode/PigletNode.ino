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
#include <Update.h>
#include "mbedtls/sha256.h"
#include "OtaProto.h"

#define FIRMWARE_VERSION "v3.03"

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

// ---- Piglet identification: lets a Core recognize this as a genuine
// Piglet-family node (for the board-type-matched OTA push below) rather
// than treating it as anonymous JCMK hardware. Carried in the otherwise-
// always-empty text field of CORE_REQUEST/HEARTBEAT; real JCMK hardware
// and Biscuit cores simply ignore this payload. PigletNode broadcasts its
// finds the same way regardless of what the Core reports back (no
// transmit-slot scheduling here, unlike the XIAO/T-Dongle variants) --
// the marker exists purely so the Core can identify the board type.
static const char*   PIGLET_MARKER     = "PIGLET1";
static const uint8_t PIGLET_MARKER_LEN = 7;
static const uint8_t PIGLET_MARKER_TOTAL_LEN = PIGLET_MARKER_LEN + 1;  // + board-type byte
// Fixed-width, null-padded copy of FIRMWARE_VERSION immediately follows the
// board-type byte -- lets a Core detect this node is running different
// firmware than itself (drives the Core's "Update Nodes through Core?"
// prompt / cfg.autoUpdateHeadlessNodes auto-push for PIGLET_BOARD_NODE).
// Send-side only: PigletNode has no Core mode, so it never needs to parse
// an incoming version field itself.
static const uint8_t PIGLET_VERSION_LEN = 8;
static const uint8_t PIGLET_MARKER_TOTAL_LEN_V2 = PIGLET_MARKER_TOTAL_LEN + PIGLET_VERSION_LEN;

// Writes the marker + board-type byte + null-padded version string starting
// at msg.text[0], and sets msg.len to PIGLET_MARKER_TOTAL_LEN_V2. Shared by
// sendRequest()/sendHeartbeat() below so the version encoding stays in sync
// with the Core's jcmkGetPigletVersion() parser (MeshNode.cpp).
static void fillPigletMarker(jcmk_text_msg_t& msg) {
  msg.len = PIGLET_MARKER_TOTAL_LEN_V2;
  memcpy(msg.text, PIGLET_MARKER, PIGLET_MARKER_LEN);
  msg.text[PIGLET_MARKER_LEN] = (char)PIGLET_BOARD_NODE;
  size_t vlen = strlen(FIRMWARE_VERSION);
  if (vlen > PIGLET_VERSION_LEN) vlen = PIGLET_VERSION_LEN;
  memset(msg.text + PIGLET_MARKER_TOTAL_LEN, 0, PIGLET_VERSION_LEN);
  memcpy(msg.text + PIGLET_MARKER_TOTAL_LEN, FIRMWARE_VERSION, vlen);
  msg.text[PIGLET_MARKER_TOTAL_LEN_V2] = '\0';
}

// ================================================================
//  Node state
// ================================================================
static bool     nodeActive    = false;
static bool     haveCore      = false;
static bool     plainBiscuit  = false;
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
static volatile bool plainBiscuitPending = false;
static uint8_t biscuitChannels[NUM_CHANNELS + 2];
static volatile uint8_t biscuitChannelCount = 0;

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
  fillPigletMarker(msg);
  esp_now_send(JCMK_BCAST, (uint8_t*)&msg, sizeof(msg));
}

static void sendHeartbeat() {
  jcmk_text_msg_t msg = {};
  memcpy(msg.magic, JCMK_MAGIC, 4);
  msg.type    = MSG_HEARTBEAT;
  msg.counter = ++hbCounter;
  fillPigletMarker(msg);
  esp_now_send(plainBiscuit ? coreMac : JCMK_BCAST, (uint8_t*)&msg, sizeof(msg));
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
  esp_now_send(plainBiscuit ? coreMac : JCMK_BCAST, (uint8_t*)&msg, sizeof(msg));
}

static void sendControl(uint8_t type, uint32_t counter = 0, const char* text = nullptr) {
  jcmk_text_msg_t msg = {};
  memcpy(msg.magic, JCMK_MAGIC, 4);
  msg.type = type;
  msg.counter = counter;
  if (text) {
    msg.len = strlen(text);
    memcpy(msg.text, text, msg.len);
  }
  esp_now_send(coreMac, (uint8_t*)&msg, sizeof(msg));
}

static bool hasPlainReply(const void* data, int len) {
  if (len < (int)sizeof(jcmk_text_msg_t)) return false;
  const jcmk_text_msg_t* msg = (const jcmk_text_msg_t*)data;
  size_t n = msg->len < JCMK_TEXT_MAX ? msg->len : JCMK_TEXT_MAX;
  for (size_t i = 0; i + 9 <= n; i++) {
    if (memcmp(msg->text + i, "encrypt=0", 9) == 0) return true;
  }
  return false;
}

static void biscuitSetChannels(const jcmk_text_msg_t* msg) {
  if (msg->len < 9 || msg->len > JCMK_TEXT_MAX || memcmp(msg->text, "channels=", 9)) {
    biscuitChannelCount = 0;
    return;
  }
  char text[JCMK_TEXT_MAX + 1];
  memcpy(text, msg->text, msg->len);
  text[msg->len] = '\0';
  char* end = strchr(text, ';');
  if (end) *end = '\0';
  uint8_t channels[NUM_CHANNELS + 2];
  uint8_t count = 0;
  for (char* token = text + 9; *token && count < NUM_CHANNELS + 2;) {
    char* next;
    long channel = strtol(token, &next, 10);
    if (next == token || channel < 1 || channel > 255 || (*next && *next != ',')) {
      count = 0;
      break;
    }
    if (channel == 104 || channel == 108) {
      channels[count++] = (uint8_t)channel;
    } else {
      for (uint8_t i = 0; i < NUM_CHANNELS; i++) {
        if (CHANNELS[i] == channel) { channels[count++] = (uint8_t)channel; break; }
      }
    }
    if (!*next) break;
    token = next + 1;
  }
  memcpy(biscuitChannels, channels, count);
  biscuitChannelCount = count;
  scanChOffset = 0;
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
//  OTA update from the Core (PIG_MSG_OTA_BEGIN / _DATA / _ACK)
// ================================================================
// The Core streams our app image over ESP-NOW (see OtaProto.h). The recv
// callback only validates + queues chunks; all flash writes, hashing and
// acks happen from otaTick() in loop(). While an update runs we stop
// scanning and sit on the home channel so every chunk is heard. The whole
// image is checked against the SHA-256 from OTA_BEGIN before the boot
// partition is switched, so a corrupt or truncated transfer is discarded
// and the node keeps running as-is. Needs an OTA-capable flash layout
// (two app slots -- PartitionScheme="8M with spiffs"/default_8MB).
#define OTA_QDEPTH 8
static const uint32_t OTA_IDLE_ABORT_MS = 15000;   // no chunk for this long -> abandon

struct OtaSlot { uint8_t len; uint8_t data[PIG_OTA_CHUNK]; };
static OtaSlot           otaQ[OTA_QDEPTH];
static volatile uint8_t  otaQHead = 0, otaQTail = 0;   // callback produces, loop consumes
static volatile bool     otaActive       = false;
static volatile uint8_t  otaSession      = 0;
static volatile uint16_t otaQNext        = 0;          // next seq the callback will accept
static volatile bool     otaBeginPending = false;
static volatile bool     otaReack        = false;      // stale/gap chunk seen -> re-send our ack
static pig_ota_begin_t   otaBeginMsg;
static uint16_t          otaWritten = 0, otaChunks = 0;
static uint32_t          otaSize = 0, otaBytes = 0, otaLastRxMs = 0;
static uint8_t           otaSha[32];
static mbedtls_sha256_context otaShaCtx;

static void otaAck(uint8_t status) {
  pig_ota_ack_t a = {};
  memcpy(a.magic, JCMK_MAGIC, 4);
  a.type    = PIG_MSG_OTA_ACK;
  a.session = otaSession;
  a.next    = otaWritten;
  a.status  = status;
  esp_now_send(coreMac, (uint8_t*)&a, sizeof(a));
}

static void otaFail(uint8_t status, const char* why) {
  Serial.printf("[OTA] FAILED: %s\n", why);
  Update.abort();
  mbedtls_sha256_free(&otaShaCtx);
  otaActive = false;
  otaAck(status);
}

static void otaStart() {
  otaBeginPending = false;
  const pig_ota_begin_t& b = otaBeginMsg;
  if (otaActive) {
    // Core re-sent BEGIN because our READY was lost: just repeat it.
    if (b.session == otaSession) { if (otaWritten == 0) otaAck(PIG_OTA_ST_READY); return; }
    Update.abort();
    mbedtls_sha256_free(&otaShaCtx);
    otaActive = false;
  }
  otaSession = b.session;
  otaWritten = 0;
  uint32_t size = b.size;
  if (size == 0 || ((size + PIG_OTA_CHUNK - 1) / PIG_OTA_CHUNK) != b.chunks) {
    otaAck(PIG_OTA_ST_ERR_ARGS);
    return;
  }
  // Stop any scan in flight and park on the home channel.
  WiFi.scanDelete();
  scanActive   = false;
  scanAdminWin = false;
  setChannel(ESPNOW_CH);

  if (!Update.begin(size, U_FLASH)) {
    Serial.printf("[OTA] Update.begin(%lu) failed: %s\n", (unsigned long)size, Update.errorString());
    otaAck(PIG_OTA_ST_ERR_SPACE);
    return;
  }
  mbedtls_sha256_init(&otaShaCtx);
  mbedtls_sha256_starts(&otaShaCtx, 0);
  memcpy(otaSha, b.sha256, 32);
  otaChunks   = b.chunks;
  otaSize     = size;
  otaBytes    = 0;
  otaQHead    = otaQTail = 0;
  otaQNext    = 0;
  otaReack    = false;
  otaLastRxMs = millis();
  otaActive   = true;
  Serial.printf("[OTA] Start: %lu bytes, %u chunks\n", (unsigned long)size, (unsigned)b.chunks);
  otaAck(PIG_OTA_ST_READY);
}

static void otaFinish() {
  uint8_t h[32];
  mbedtls_sha256_finish(&otaShaCtx, h);
  mbedtls_sha256_free(&otaShaCtx);
  if (otaBytes != otaSize) { otaFail(PIG_OTA_ST_ERR_ARGS, "size mismatch"); return; }
  if (memcmp(h, otaSha, 32) != 0) {
    Update.abort();
    otaActive = false;
    Serial.println("[OTA] FAILED: SHA-256 mismatch — update discarded");
    otaAck(PIG_OTA_ST_ERR_SHA);
    return;
  }
  if (!Update.end(true)) { otaFail(PIG_OTA_ST_ERR_WRITE, Update.errorString()); return; }
  otaActive = false;
  Serial.println("[OTA] Verified — rebooting into the new image");
  for (uint8_t i = 0; i < 3; i++) { otaAck(PIG_OTA_ST_DONE); delay(30); }
  delay(200);
  ESP.restart();
}

static void otaTick() {
  if (otaBeginPending) otaStart();
  if (!otaActive) return;
  uint32_t now = millis();
  bool progressed = false;
  while (otaQTail != otaQHead) {
    OtaSlot& sl = otaQ[otaQTail];
    if (Update.write(sl.data, sl.len) != sl.len) { otaFail(PIG_OTA_ST_ERR_WRITE, "flash write"); return; }
    mbedtls_sha256_update(&otaShaCtx, sl.data, sl.len);
    otaBytes += sl.len;
    otaWritten++;
    otaQTail = (otaQTail + 1) % OTA_QDEPTH;
    progressed = true;
  }
  if (progressed || otaReack) {
    otaReack = false;
    otaAck(PIG_OTA_ST_OK);
  }
  if (progressed) {
    otaLastRxMs = now;
    lastAdminMs = now;                 // we are talking to the Core: don't time out
  }
  if (otaWritten == otaChunks) { otaFinish(); return; }
  if (now - otaLastRxMs > OTA_IDLE_ABORT_MS) {
    Serial.println("[OTA] Timed out waiting for chunks — abandoning update");
    Update.abort();
    mbedtls_sha256_free(&otaShaCtx);
    otaActive = false;                 // scanning resumes; Core can retry
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
    plainBiscuitPending = hasPlainReply(data, len);
    coreFoundPending = true;

  } else if (haveCore && memcmp(info->src_addr, coreMac, 6) == 0
             && len >= (int)sizeof(jcmk_text_msg_t) && type == MSG_ADMIN) {
    const jcmk_text_msg_t* msg = (const jcmk_text_msg_t*)data;
    lastAdminMs = millis();
    sendControl(15, msg->len ? (uint8_t)msg->text[0] : msg->counter & 0xFF);

  } else if (haveCore && memcmp(info->src_addr, coreMac, 6) == 0
             && len >= (int)sizeof(jcmk_text_msg_t) && type == 10) {
    lastAdminMs = millis();
    if (plainBiscuit) biscuitSetChannels((const jcmk_text_msg_t*)data);
    sendControl(11);

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

  } else if (type == PIG_MSG_OTA_BEGIN && haveCore && memcmp(info->src_addr, coreMac, 6) == 0
             && len >= (int)sizeof(pig_ota_begin_t)) {
    // ---- Core wants to update us. Queue it for loop(); never touch flash from here.
    if (!otaBeginPending) { memcpy(&otaBeginMsg, data, sizeof(otaBeginMsg)); otaBeginPending = true; }

  } else if (type == PIG_MSG_OTA_DATA && otaActive && haveCore
             && memcmp(info->src_addr, coreMac, 6) == 0
             && len > (int)offsetof(pig_ota_data_t, data)) {
    const pig_ota_data_t* d = (const pig_ota_data_t*)data;
    if (d->session == otaSession && d->len >= 1 && d->len <= PIG_OTA_CHUNK
        && len >= (int)offsetof(pig_ota_data_t, data) + (int)d->len) {
      if (d->seq == otaQNext) {
        uint8_t nh = (uint8_t)((otaQHead + 1) % OTA_QDEPTH);
        if (nh != otaQTail) {
          otaQ[otaQHead].len = d->len;
          memcpy(otaQ[otaQHead].data, d->data, d->len);
          otaQHead = nh;
          otaQNext++;
        }
      } else {
        otaReack = true;   // stale or out-of-order: tell the Core where we are
      }
    }

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
  plainBiscuit = false;
  biscuitChannelCount = 0;
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
  uint8_t numCh = biscuitChannelCount ? biscuitChannelCount :
                  ((endIdx >= startIdx) ? (endIdx - startIdx + 1) : 0);
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
    if (!biscuitChannelCount && chIdx >= NUM_CHANNELS) { scanChOffset++; return; }
    uint8_t channel = biscuitChannelCount ? biscuitChannels[scanChOffset] : CHANNELS[chIdx];

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
      String ssid = WiFi.SSID(i);
      String auth = authStr(WiFi.encryptionType(i));
      if (plainBiscuit) {
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
      String line = WiFi.BSSIDstr(i) + "," + ssid + ","
                  + auth + ","
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
    plainBiscuit = plainBiscuitPending;
    reqInterval = REQ_INIT_MS;
    lastAdminMs = now;
    addPeer(coreMac);
    if (plainBiscuit) {
      setChannel(ESPNOW_CH);
      sendControl(7, 0, "Piglet:" FIRMWARE_VERSION);
      scanAdminWin = true;
      scanAdminMs = now;
    }
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
  if (haveCore && !otaActive) scanTick();   // scanning is paused during an OTA update
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
// BTN_PIN is GPIO0, which doubles as the ESP32 BOOT/flash-mode strapping
// pin. The 2 s continuous-hold requirement already makes this fairly
// immune to brief post-flash reset noise, but a short settle window is
// still applied for defense-in-depth consistency with the other Piglet
// firmware variants; set once near the end of setup().
static const uint32_t BTN_BOOT_SETTLE_MS = 1500;
static uint32_t btnBootSettleUntilMs = 0;

static void buttonTick() {
  static bool     pressing      = false;
  static bool     triggered     = false;
  static uint32_t pressStartMs  = 0;

  if (millis() < btnBootSettleUntilMs) return;

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

  // Arm the button boot-settle guard now, right as loop() is about to start —
  // see buttonTick() for the rationale.
  btnBootSettleUntilMs = millis() + BTN_BOOT_SETTLE_MS;

  Serial.println("[NODE] Ready — searching for Core on ch 6...");
}

// ================================================================
//  loop()
// ================================================================
void loop() {
  nodeTick();
  otaTick();
  ledTick();
  buttonTick();
  statusTick();
  delay(otaActive ? 1 : 10);
}
