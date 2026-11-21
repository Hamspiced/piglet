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
#include <atomic>
#include "../../src/RecordProtocol.h"
#include "esp_wifi.h"

#define FIRMWARE_VERSION "v2.65"

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
static bool     plainBiscuit  = false;
static uint8_t  coreMac[6]   = {0};
static uint8_t  startIdx      = 0;
static uint8_t  endIdx        = NUM_CHANNELS - 1;  // default = all channels
static uint8_t  assignVer     = 0;
static uint32_t netFound      = 0;
static uint32_t netSent       = 0; // queue attempts, not confirmed delivery
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
static uint32_t scanAdminDurationMs = ADMIN_WIN_MS;

// Pending pairing event (published and consumed by the main loop)
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

// A single in-flight frame owns the home channel until its callback completes.
// Receive callbacks queue input events; the main loop owns radio/state work.
struct PendingTx { uint8_t mac[6]; jcmk_text_msg_t msg; };
static QueueHandle_t txQueue = nullptr, txResults = nullptr;
static bool txInFlight = false, txTimeoutReported = false;
static uint32_t txStartedMs = 0;
static PendingTx activeTx;
static uint32_t txQueueDrops = 0;
static bool searchResetRequested = false;
static std::atomic<bool> txCallbackFault{false};
static std::atomic<uint32_t> rxQueueDrops{0};
#ifdef PIGLET_STABILIZATION_TEST
static std::atomic<bool> dropNextTxCallback{false}, txCallbackDropped{false};
#endif
static uint32_t recordSession = 0;
static QueueHandle_t recordAcks = nullptr;
static bool recordAwaitAck = false;
static uint8_t recordAttempts = 0;
static uint32_t recordAckWaitMs = 0;
static bool enqueueTx(const uint8_t* mac, const uint8_t* data, size_t len) {
  if (!txQueue || len != sizeof(jcmk_text_msg_t)) return false;
  PendingTx item; memcpy(item.mac, mac, 6); memcpy(&item.msg, data, len);
  if (xQueueSend(txQueue, &item, 0) != pdTRUE) { ++txQueueDrops; return false; }
  return true;
}
static void onSent(const esp_now_send_info_t*, esp_now_send_status_t status) {
#ifdef PIGLET_STABILIZATION_TEST
  if (dropNextTxCallback.exchange(false)) { txCallbackDropped.store(true); return; }
#endif
  if (!txResults || xQueueSend(txResults, &status, 0) != pdTRUE) txCallbackFault.store(true);
}
static bool txPending() { return txInFlight || recordAwaitAck || (txQueue && uxQueueMessagesWaiting(txQueue)); }
static void txTick() {
  if (txCallbackFault.exchange(false)) {
    Serial.println("[NODE] TX callback queue fault: restarting; queued delivery unconfirmed");
    delay(10); ESP.restart(); return;
  }
#ifdef PIGLET_STABILIZATION_TEST
  if (txCallbackDropped.exchange(false)) Serial.println("[NODE] TEST suppressed driver callback");
#endif
  RecordProtocol::Packet ack;
  bool acknowledged = false;
  while (recordAcks && xQueueReceive(recordAcks, &ack, 0) == pdTRUE) {
    if (recordAwaitAck && ack.session == recordSession && ack.sequence == activeTx.msg.counter) {
#ifdef PIGLET_RECORD_ACK_TEST
      static bool droppedFirst = false;
      if (ack.sequence == 2 && !droppedFirst) {
        droppedFirst = true; Serial.println("[NODE] TEST ignore first RECORD_ACK seq=2"); continue;
      }
      if (ack.sequence == 3) { Serial.println("[NODE] TEST ignore all RECORD_ACK seq=3"); continue; }
#endif
      acknowledged = true;
    }
  }
  if (acknowledged) {
    recordAwaitAck = false;
    Serial.printf("[NODE] RECORD_ACK seq=%lu attempts=%u\n", (unsigned long)activeTx.msg.counter, recordAttempts);
  }
  esp_now_send_status_t status;
  if (txInFlight && xQueueReceive(txResults, &status, 0) == pdTRUE) {
    if (activeTx.msg.type == MSG_TEXT)
      Serial.printf("[NODE] TX_DONE seq=%lu success=%d\n", (unsigned long)activeTx.msg.counter, status == ESP_NOW_SEND_SUCCESS);
    txInFlight = false;
  }
  // Never reuse an ambiguous callback or hop channels before completion.
  if (txInFlight) {
    if (!txTimeoutReported && millis() - txStartedMs >= 1000) {
      txTimeoutReported = true; Serial.println("[NODE] TX stalled: holding home channel awaiting callback");
    }
    if (millis() - txStartedMs >= 3000) {
      Serial.printf("[NODE] TX watchdog restart; seq=%lu queued=%u delivery unconfirmed\n", (unsigned long)activeTx.msg.counter, (unsigned)uxQueueMessagesWaiting(txQueue));
      delay(10); ESP.restart();
    }
    return;
  }
  if (searchResetRequested) return; // Retire an in-flight callback before discarding old work.
  bool retry = false;
  if (recordAwaitAck) {
    if (millis() - recordAckWaitMs < RecordProtocol::ACK_WAIT_MS) return;
    if (recordAttempts >= RecordProtocol::MAX_ATTEMPTS) {
      Serial.printf("[NODE] RECORD_EXPIRED seq=%lu attempts=%u\n", (unsigned long)activeTx.msg.counter, recordAttempts);
      recordAwaitAck = false;
    } else retry = true;
  }
  if (!retry) {
    if (scanActive || !txQueue || xQueueReceive(txQueue, &activeTx, 0) != pdTRUE) return;
    recordAttempts = 0;
  }
  setChannel(ESPNOW_CH);
  txInFlight = true; txTimeoutReported = false; txStartedMs = millis();
  esp_err_t result;
  if (activeTx.msg.type == MSG_TEXT) {
    RecordProtocol::Packet packet = {};
    memcpy(packet.magic, "ENOW", 4); packet.type = RecordProtocol::DATA;
    packet.session = recordSession; packet.sequence = activeTx.msg.counter;
    packet.len = activeTx.msg.len; memcpy(packet.text, activeTx.msg.text, packet.len);
    ++recordAttempts; recordAwaitAck = true; recordAckWaitMs = millis();
    result = esp_now_send(activeTx.mac, (uint8_t*)&packet, sizeof(packet));
    Serial.printf("[NODE] RECORD seq=%lu attempt=%u accepted=%d queueDrops=%lu\n", (unsigned long)activeTx.msg.counter, recordAttempts, result == ESP_OK, (unsigned long)txQueueDrops);
  } else result = esp_now_send(activeTx.mac, (uint8_t*)&activeTx.msg, sizeof(activeTx.msg));
  if (result != ESP_OK) txInFlight = false;
}

static void sendRequest() {
  jcmk_text_msg_t msg = {};
  memcpy(msg.magic, JCMK_MAGIC, 4);
  msg.type = MSG_CORE_REQUEST;
  msg.len = snprintf(msg.text, sizeof(msg.text), "CTRL2:%08lX", (unsigned long)recordSession);
  enqueueTx(JCMK_BCAST, (uint8_t*)&msg, sizeof(msg));
}

static void sendHeartbeat() {
  jcmk_text_msg_t msg = {};
  memcpy(msg.magic, JCMK_MAGIC, 4);
  msg.type    = MSG_HEARTBEAT;
  msg.counter = ++hbCounter;
  enqueueTx(plainBiscuit ? coreMac : JCMK_BCAST, (uint8_t*)&msg, sizeof(msg));
}

static uint32_t recordSequence = 0;
#ifdef PIGLET_SD_READBACK_TEST
static bool sdReadbackArmed = false;
#ifdef PIGLET_COMMIT_FAILURE_TEST
static uint32_t sdReadbackLimit = 1;
#elif defined(PIGLET_SD_POWER_TEST)
static uint32_t sdReadbackLimit = 49;
#else
static uint32_t sdReadbackLimit = 129;
#endif
#endif
static void sendText(const String& s) {
#ifdef PIGLET_SD_READBACK_TEST
  if (!sdReadbackArmed || recordSequence >= sdReadbackLimit) return;
#endif

  ++netSent;
  jcmk_text_msg_t msg = {};
  memcpy(msg.magic, JCMK_MAGIC, 4);
  msg.type    = MSG_TEXT;
  msg.counter = ++recordSequence;
  uint16_t slen = (uint16_t)((s.length() < JCMK_TEXT_MAX) ? s.length() : JCMK_TEXT_MAX);
  msg.len = slen;
  memcpy(msg.text, s.c_str(), slen);
  msg.text[slen] = '\0';
  // Always full struct size — Biscuit Pro drops packets < 212 bytes.
  bool queued = enqueueTx(coreMac, (uint8_t*)&msg, sizeof(msg));
  Serial.printf("[NODE] RECORD_QUEUE seq=%lu queued=%d truncated=%d\n", (unsigned long)msg.counter, queued, s.length() > JCMK_TEXT_MAX);
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
  enqueueTx(coreMac, (uint8_t*)&msg, sizeof(msg));
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
//  ESP-Now receive callback (Wi-Fi task — queue only)
// ================================================================
static QueueHandle_t configQueue = nullptr;
static uint32_t windowToken = 0;
static uint32_t appliedToken = 0;
struct RxEvent { uint8_t mac[6]; uint16_t length; uint8_t data[sizeof(RecordProtocol::Packet)]; };
static QueueHandle_t receiveQueue = nullptr;
// Wi-Fi task callbacks only publish complete events. Main loop owns pairing,
// liveness, channel assignments, and all transmit state.
static void onRecv(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
  if (len < 5 || len > (int)sizeof(RxEvent::data)) return;
  RxEvent event = {}; memcpy(event.mac, info->src_addr, 6); event.length = len; memcpy(event.data,data,len);
  if (!receiveQueue || xQueueSend(receiveQueue,&event,0) != pdTRUE) rxQueueDrops.fetch_add(1);
}
static void processReceive(const uint8_t* mac, const uint8_t* data, int len) {
  if (len < 5) return;
  if (data[0]!='E' || data[1]!='N' || data[2]!='O' || data[3]!='W') return;
  uint8_t type = data[4];
  if (type != MSG_CORE_REPLY && (!haveCore || memcmp(mac,coreMac,6))) return;
  if (type == RecordProtocol::ACK && haveCore && !memcmp(mac, coreMac, 6) && len == sizeof(RecordProtocol::Packet)) {
    const auto* ack = (const RecordProtocol::Packet*)data;
    if (ack->session == recordSession && ack->len == 0 && recordAcks && xQueueSend(recordAcks, data, 0) != pdTRUE) Serial.println("[NODE] record ACK queue full");
    return;
  }
  if (type == 113 && haveCore && !memcmp(mac, coreMac, 6) && len == sizeof(jcmk_text_msg_t)) {
    if (configQueue && xQueueSend(configQueue, data, 0) != pdTRUE) Serial.println("[NODE] config queue full");
    return;
  }

  if (type == MSG_CORE_REPLY && !haveCore && !coreFoundPending) {
    memcpy(coreMacPending, mac, 6);
    plainBiscuitPending = hasPlainReply(data, len);
    coreFoundPending = true;

  } else if (haveCore && memcmp(mac, coreMac, 6) == 0
             && len >= (int)sizeof(jcmk_text_msg_t) && type == MSG_ADMIN) {
    const jcmk_text_msg_t* msg = (const jcmk_text_msg_t*)data;
    lastAdminMs = millis();
    sendControl(15, msg->len ? (uint8_t)msg->text[0] : msg->counter & 0xFF);

  } else if (haveCore && memcmp(mac, coreMac, 6) == 0
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
    if (adm->start_channel_idx > adm->end_channel_idx || adm->end_channel_idx >= NUM_CHANNELS) return;
    if (adm->assignment_version != assignVer) {
      assignVer = adm->assignment_version;
      startIdx  = adm->start_channel_idx;
      endIdx    = adm->end_channel_idx;
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
static void receiveTick() {
  static uint32_t lastDrops = 0;
  uint32_t drops=rxQueueDrops.load();
  if (drops != lastDrops) { Serial.printf("[NODE] RX queue drops=%lu\n", (unsigned long)drops); lastDrops=drops; }
  RxEvent event;
  // Bound work so incoming traffic cannot starve TX/watchdog processing.
  for (unsigned n=0;n<32 && xQueueReceive(receiveQueue,&event,0)==pdTRUE;++n)
    processReceive(event.mac,event.data,event.length);
}
static void resetToSearching() { searchResetRequested = true; }
static void completeSearchReset() {
  if (!searchResetRequested || txInFlight) return;
  searchResetRequested = false;
  Serial.printf("[NODE] reset discarding queued=%u awaitingACK=%d as unconfirmed\n", (unsigned)uxQueueMessagesWaiting(txQueue), recordAwaitAck);
  xQueueReset(txQueue); xQueueReset(txResults); xQueueReset(recordAcks); xQueueReset(configQueue); xQueueReset(receiveQueue);
  recordAwaitAck = false; recordAttempts = 0;
  if (scanActive) esp_wifi_scan_stop();
  WiFi.scanDelete();
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
  if (!scanActive && txPending()) return;
  uint8_t numCh = biscuitChannelCount ? biscuitChannelCount :
                  ((endIdx >= startIdx) ? (endIdx - startIdx + 1) : 0);
  if (numCh == 0) return;

  // Admin window: radio is on ch 6, waiting before next cycle
  if (scanAdminWin) {
    jcmk_text_msg_t msg;
    while (configQueue && xQueueReceive(configQueue, &msg, 0) == pdTRUE) {
      if (msg.counter != windowToken || !windowToken) continue;
      if (appliedToken != windowToken) {
        biscuitSetChannels(&msg);
        if (!biscuitChannelCount) continue;
        appliedToken = windowToken;
        Serial.printf("[NODE] control applied token=%lu channels=%u\n", (unsigned long)windowToken, biscuitChannelCount);
      }
      lastAdminMs = millis();
      sendControl(114, windowToken);
    }
    if (millis() - scanAdminMs >= scanAdminDurationMs) {
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
      if (++windowToken == 0) ++windowToken;
      sendControl(112, windowToken);
      lastHbMs     = millis();
      scanAdminWin = true;
      // Random jitter (0-200 ms) staggers admin windows across nodes
      // Keep elapsed-time subtraction wrap-safe by using an actual start time.
      scanAdminMs = millis();
      scanAdminDurationMs = ADMIN_WIN_MS + (uint32_t)(esp_random() % 200);
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
  if (!nodeActive || searchResetRequested) return;
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
      scanAdminDurationMs = ADMIN_WIN_MS;
    }
    setChannel(ESPNOW_CH);
    if (++windowToken == 0) ++windowToken;
    sendControl(112, windowToken);
    scanAdminWin = true; scanAdminMs = millis(); scanAdminDurationMs = ADMIN_WIN_MS;
    Serial.printf("[NODE] Core found: %02X:%02X:%02X:%02X:%02X:%02X\n",
      coreMac[0], coreMac[1], coreMac[2], coreMac[3], coreMac[4], coreMac[5]);
  }

  // --- Core timeout: re-search if ADMIN/HB silent for CORE_TIMEOUT_MS ---
  if (haveCore && lastAdminMs > 0 && (millis() - lastAdminMs) >= CORE_TIMEOUT_MS) {
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
                  "ch %d-%d (v%d) | found %lu | reportAttempts %lu\n",
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
  receiveQueue = xQueueCreate(32, sizeof(RxEvent));
  recordAcks = xQueueCreate(4, sizeof(RecordProtocol::Packet));
  recordSession = esp_random(); if (!recordSession) recordSession = 1;
  configQueue = xQueueCreate(4, sizeof(jcmk_text_msg_t));
  txQueue = xQueueCreate(128, sizeof(PendingTx));
  txResults = xQueueCreate(2, sizeof(esp_now_send_status_t));
  Serial.begin(115200);
  delay(200);
  Serial.println();
  Serial.println("=== PigletNode Boot ===");
  Serial.printf("JCMK channels: %d  |  home ch: %d\n", NUM_CHANNELS, ESPNOW_CH);

  if (!receiveQueue || !configQueue || !txQueue || !txResults || !recordAcks) {
    Serial.println("[NODE] queue allocation failed"); while (true) delay(1000);
  }
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

  WiFi.setScanActiveMinTime(SCAN_DWELL_MS);
  Serial.printf("[NODE] scan requested-active min=%lu max=%lu ms\n", (unsigned long)SCAN_DWELL_MS, (unsigned long)SCAN_DWELL_MS);

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
  esp_now_register_send_cb(onSent);

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
#ifdef PIGLET_STABILIZATION_TEST
  while (Serial.available()) {
    char command=Serial.read();
    if(command=='w') { dropNextTxCallback.store(true); Serial.println("[NODE] TEST callback loss armed"); }
    if(command=='r') resetToSearching();
  }
#endif
#ifdef PIGLET_SD_READBACK_TEST
  while (Serial.available()) {
    char command = Serial.read();
    if (command == 'v') { sdReadbackArmed = true; Serial.printf("[NODE] SDREAD armed limit=%lu\n", (unsigned long)sdReadbackLimit); }
#ifdef PIGLET_SD_POWER_TEST
    if (command == '+' && sdReadbackLimit <= 127) sdReadbackLimit += 2;
#endif
  }
#endif
  receiveTick();
  txTick();
  completeSearchReset();
  nodeTick();
  ledTick();
  buttonTick();
  statusTick();
  delay(10);
}
