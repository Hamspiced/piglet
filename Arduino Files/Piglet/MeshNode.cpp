#include "MeshNode.h"
#include "Globals.h"
#include "Config.h"
#include "GPS.h"
#include "SDUtils.h"
#include "Display.h"
#include "WardriveGoBLE.h"
#include "YieldSched.h"
#include "OtaProto.h"
#include <esp_now.h>
#include "esp_wifi.h"
#include <Update.h>
#include "mbedtls/sha256.h"
#include "esp_heap_caps.h"

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
static const uint32_t NODE_ADMIN_WIN_MS   = 500;   // ch-6 window after each full cycle
#define JCMK_TEXT_MAX 200

// ---- Piglet-to-Piglet identification + transmit-slot scheduling ----
// A short marker carried in the otherwise-always-empty text/len fields of the
// existing CORE_REQUEST/CORE_REPLY/HEARTBEAT messages. Neither real JCMK
// hardware nor Biscuit nodes look at this payload, so it's fully backward
// compatible; it only ever activates the new scheduling path when BOTH sides
// of a pairing are confirmed to be genuine Piglet devices.
static const char*   PIGLET_MARKER     = "PIGLET1";
static const uint8_t PIGLET_MARKER_LEN = 7;
// Board-type byte (PigletBoardType, OtaProto.h) immediately follows the
// marker text, letting a Core pick the right /Firmware/<Board>.ota.bin
// image for a connected node -- every Piglet variant is ESP32-C5, so the
// image's own chip_id can't disambiguate boards the way it could for a
// mixed-silicon fleet. See coreOtaStart() below.
static const uint8_t PIGLET_MARKER_TOTAL_LEN = PIGLET_MARKER_LEN + 1;
#define PIGLET_THIS_BOARD_TYPE PIGLET_BOARD_XIAO
// Fixed-width, null-padded copy of FIRMWARE_VERSION immediately follows the
// board-type byte, letting a Core detect a connected Piglet/TDongleC5_Piglet
// node running different firmware than itself (see the on-device "Update
// Nodes through Core?" prompt in coreModeTick()/drawPageMeshNode()). A node
// still on the shorter pre-version marker reports an empty version string --
// treated as "unknown", never flagged as a mismatch.
static const uint8_t PIGLET_VERSION_LEN = 8;
static const uint8_t PIGLET_MARKER_TOTAL_LEN_V2 = PIGLET_MARKER_TOTAL_LEN + PIGLET_VERSION_LEN;

// Deterministic transmit-slot width for Piglet-to-Piglet nodes. Each
// confirmed-Piglet node gets an exclusive [slotIndex*SLOT_MS, +SLOT_MS) window
// within a repeating cycle (cycle length = SLOT_MS * active-Piglet-node-count),
// so scan results are sent without colliding with other nodes' transmissions.
static const uint32_t JCMK_SLOT_MS = 200;

// If a Node hasn't heard anything from its Core in this long, assume it's gone
// and return to searching (mirrors Biscuit's own CORE_TIMEOUT_MS behavior).
static const uint32_t JCMK_CORE_TIMEOUT_MS = 30000;

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
uint32_t jcmkSendFailCount = 0;

static uint32_t jcmkHbCounter   = 0;
// Separate, always-incrementing counter for TEXT (scan-result) messages.
// A single channel scan can yield several networks, and jcmkSendText() used
// to stamp every one of them with the *current* jcmkHbCounter value instead
// of its own -- meaning every TEXT message sent between two heartbeats
// carried an identical, stale counter. If a Core treats (sender MAC,
// counter) as a duplicate-suppression key (a common anti-retry pattern),
// it would process the first message with that counter (the heartbeat,
// which is what keeps a node showing as "connected") and silently drop
// every TEXT message sharing it as an apparent duplicate -- which looks
// exactly like a node staying registered/alive while reporting 0 networks.
static uint32_t jcmkTextCounter = 0;
static uint32_t jcmkLastHbMs    = 0;
static uint32_t jcmkLastReqMs   = 0;
static uint32_t jcmkReqInterval = JCMK_REQ_INIT_MS;

// Per-channel async scan state
static bool     nodeScanActive   = false;
static uint8_t  nodeScanChOffset = 0;
static bool     nodeScanAdminWin = false;
static uint32_t nodeScanAdminMs  = 0;

// Channel list the Wardrive Go phone app has explicitly narrowed scanning to (via
// `-ch`/`ch`), refreshed each nodeDoScanTick() call. Empty unless the bridge is active
// AND the app asked for fewer than every supported channel -- see wardriveGoChannelOverride().
static uint8_t  wgOverrideChannels[48];
static uint8_t  wgOverrideCount = 0;

// Pending core-found event — written in ESP-Now callback, consumed in loop
static volatile bool  jcmkCoreFoundPending    = false;
static uint8_t        jcmkCoreMacPending[6]   = {0};
static volatile bool  jcmkCoreIsPigletPending = false;
static volatile bool  biscuitPlainReplyPending = false;
static uint8_t biscuitChannels[JCMK_NUM_CHANNELS + 2];
static volatile uint8_t biscuitChannelCount = 0;

// Piglet-to-Piglet transmit-slot state (only meaningful when jcmkCoreIsPiglet)
static bool     jcmkCoreIsPiglet  = false;
static uint8_t  jcmkSlotIndex     = 0;
static uint8_t  jcmkSlotCount     = 1;
static uint32_t jcmkCycleEpochMs  = 0;
static uint32_t jcmkCoreLastSeenMs = 0;  // for JCMK_CORE_TIMEOUT_MS detection

// Ring buffer of scan-result lines awaiting this node's transmit slot.
// Scanning (nodeDoScanTick) keeps running at full pace regardless of buffer
// state; only sending already-found results is deferred to the slot window.
#define JCMK_PENDING_MAX 64
struct JcmkPendingLine { char text[96]; };
static JcmkPendingLine jcmkPendingBuf[JCMK_PENDING_MAX];
static uint8_t         jcmkPendingHead = 0, jcmkPendingTail = 0;

static bool jcmkPendingEmpty() { return jcmkPendingHead == jcmkPendingTail; }

static bool jcmkPendingPush(const String& line) {
  uint8_t next = (jcmkPendingTail + 1) % JCMK_PENDING_MAX;
  if (next == jcmkPendingHead) return false;  // full — drop rather than block scanning
  size_t n = line.length();
  if (n >= sizeof(jcmkPendingBuf[0].text)) n = sizeof(jcmkPendingBuf[0].text) - 1;
  memcpy(jcmkPendingBuf[jcmkPendingTail].text, line.c_str(), n);
  jcmkPendingBuf[jcmkPendingTail].text[n] = '\0';
  jcmkPendingTail = next;
  return true;
}

static bool jcmkPendingPop(String& out) {
  if (jcmkPendingEmpty()) return false;
  out = jcmkPendingBuf[jcmkPendingHead].text;
  jcmkPendingHead = (jcmkPendingHead + 1) % JCMK_PENDING_MAX;
  return true;
}

// ================================================================
//  Node-side OTA receiver state (PIG_MSG_OTA_BEGIN / _DATA / _ACK)
//  Lets this Piglet be pushed a firmware update while acting as a Node
//  under another Piglet (or TDongleC5_Piglet) Core. The ESP-Now callback
//  only validates + queues chunks (ISR-safe, no flash writes); nodeOtaTick()
//  in nodeModeTick() drives Update.begin/write/end + incremental SHA-256.
//  See the functions right after biscuitSendControl() below, and the
//  Core-side engine (coreOta*) further down this file.
// ================================================================
#define NODE_OTA_QDEPTH 8
struct NodeOtaSlot { uint8_t len; uint8_t data[PIG_OTA_CHUNK]; };
static NodeOtaSlot        nodeOtaQ[NODE_OTA_QDEPTH];
static volatile uint8_t   nodeOtaQHead = 0, nodeOtaQTail = 0;   // callback produces, loop consumes
static volatile bool      nodeOtaActive       = false;
static volatile uint8_t   nodeOtaSession      = 0;
static volatile uint16_t  nodeOtaQNext        = 0;              // next seq the callback will accept
static volatile bool      nodeOtaBeginPending = false;
static volatile bool      nodeOtaReack        = false;          // stale/gap chunk seen -> re-send our ack
static pig_ota_begin_t    nodeOtaBeginMsg;
static uint16_t           nodeOtaWritten = 0, nodeOtaChunks = 0;
static uint32_t           nodeOtaSize = 0, nodeOtaBytes = 0, nodeOtaLastRxMs = 0;
static uint8_t            nodeOtaSha[32];
static mbedtls_sha256_context nodeOtaShaCtx;
static const uint32_t NODE_OTA_IDLE_ABORT_MS = 15000;  // no chunk for this long -> abandon

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
static uint8_t  corePigletSlotCount = 0;  // active Piglet-confirmed node count; recomputed in coreReassignChannels()

// Per-5GHz-channel yield tracking (see YieldSched.h), fed from every TEXT
// record the Core receives regardless of which node reported it. Folded into
// an EWMA every ~4s and used only to ORDER the channel list a Biscuit node is
// given -- the channel SET/COUNT a node is assigned is unaffected, so this is
// a no-op for real-JCMK/Piglet-to-Piglet nodes (which never see this list;
// they keep the packed binary ADMIN message, unchanged).
#define CORE_5G_COUNT 26  // JCMK_NUM_CHANNELS(40) - 14 2.4GHz entries
static ChanStat  core5gStat[CORE_5G_COUNT];
static uint16_t  core5gHits[CORE_5G_COUNT];
static uint32_t  coreYieldLastMs = 0;
static SchedCfg  coreSchedCfg;

static const uint32_t CORE_HB_MS        = 5000;
static const uint32_t CORE_NODE_TIMEOUT = 90000;  // 90 s — generous for many-node ESP-Now collisions

// Ring buffers: ESP-Now callback → main loop
#define CORE_REQ_QUEUE  16
#define CORE_TEXT_QUEUE 192  // sized for burst from 12 nodes × ~15 networks each

struct CorReqSlot  { uint8_t mac[6]; bool isBiscuit; bool isPiglet; uint8_t boardType; char version[9]; };
struct CorTextSlot { char    line[JCMK_TEXT_MAX + 1]; };

static CorReqSlot          coreReqBuf[CORE_REQ_QUEUE];
static volatile uint8_t    coreReqHead = 0, coreReqTail = 0;

static CorTextSlot         coreTextBuf[CORE_TEXT_QUEUE];
static volatile uint8_t    coreTextHead = 0, coreTextTail = 0;

// ================================================================
//  Local helpers
// ================================================================
// True if a received jcmk_text_msg_t carries the Piglet identification marker.
// `len` is the raw ESP-Now payload length (as delivered to the recv callback).
static bool jcmkHasPigletMarker(const void* data, int len) {
  if (len < (int)sizeof(jcmk_text_msg_t)) return false;
  const jcmk_text_msg_t* tm = (const jcmk_text_msg_t*)data;
  if (tm->len < PIGLET_MARKER_LEN) return false;
  return memcmp(tm->text, PIGLET_MARKER, PIGLET_MARKER_LEN) == 0;
}

// Returns the sender's declared PigletBoardType if the marker AND the
// board-type byte that follows it are both present, else PIGLET_BOARD_UNKNOWN
// (a pre-v3.0 Piglet node that only sends the bare 7-byte marker, or
// non-Piglet hardware -- jcmkHasPigletMarker() already gates isPiglet itself).
static uint8_t jcmkGetPigletBoardType(const void* data, int len) {
  if (len < (int)sizeof(jcmk_text_msg_t)) return PIGLET_BOARD_UNKNOWN;
  const jcmk_text_msg_t* tm = (const jcmk_text_msg_t*)data;
  if (tm->len < PIGLET_MARKER_TOTAL_LEN) return PIGLET_BOARD_UNKNOWN;
  if (memcmp(tm->text, PIGLET_MARKER, PIGLET_MARKER_LEN) != 0) return PIGLET_BOARD_UNKNOWN;
  return (uint8_t)tm->text[PIGLET_MARKER_LEN];
}

// Writes the sender's declared firmware version into `out` (size >= 9) if the
// marker AND the version bytes that follow the board-type byte are both
// present, else writes an empty string -- a pre-version Piglet node (or
// non-Piglet hardware) simply reports "unknown", which never triggers the
// on-device "Update Nodes through Core?" mismatch prompt.
static void jcmkGetPigletVersion(const void* data, int len, char* out) {
  out[0] = '\0';
  if (len < (int)sizeof(jcmk_text_msg_t)) return;
  const jcmk_text_msg_t* tm = (const jcmk_text_msg_t*)data;
  if (tm->len < PIGLET_MARKER_TOTAL_LEN_V2) return;
  if (memcmp(tm->text, PIGLET_MARKER, PIGLET_MARKER_LEN) != 0) return;
  memcpy(out, tm->text + PIGLET_MARKER_TOTAL_LEN, PIGLET_VERSION_LEN);
  out[PIGLET_VERSION_LEN] = '\0';
}

// True if a received CORE_REPLY indicates a Biscuit Core running in
// unencrypted-reply mode (carries "encrypt=0" somewhere in its text field).
static bool biscuitHasPlainReply(const void* data, int len) {
  if (len < (int)sizeof(jcmk_text_msg_t)) return false;
  const jcmk_text_msg_t* msg = (const jcmk_text_msg_t*)data;
  size_t n = msg->len < JCMK_TEXT_MAX ? msg->len : JCMK_TEXT_MAX;
  for (size_t i = 0; i + 9 <= n; i++) {
    if (memcmp(msg->text + i, "encrypt=0", 9) == 0) return true;
  }
  return false;
}

// Parses a Biscuit "channels=1,2,...;dwell=N" config message into
// biscuitChannels[]/biscuitChannelCount, restarting the scan cycle from the
// new list. Only used for plain-reply Biscuit pairings.
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
  uint8_t channels[JCMK_NUM_CHANNELS + 2];
  uint8_t count = 0;
  for (char* token = text + 9; *token && count < JCMK_NUM_CHANNELS + 2;) {
    char* next;
    long channel = strtol(token, &next, 10);
    if (next == token || channel < 1 || channel > 255 || (*next && *next != ',')) {
      count = 0;
      break;
    }
    if (channel == 104 || channel == 108) {
      channels[count++] = (uint8_t)channel;
    } else {
      for (uint8_t i = 0; i < JCMK_NUM_CHANNELS; i++) {
        if (JCMK_CHANNELS[i] == channel) { channels[count++] = (uint8_t)channel; break; }
      }
    }
    if (!*next) break;
    token = next + 1;
  }
  memcpy(biscuitChannels, channels, count);
  biscuitChannelCount = count;
  nodeScanChOffset = 0;
}

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
  // Do NOT use promiscuous mode here: on IDF 5.x, disabling promiscuous after
  // a prior STA connection causes the driver to revert to the home router channel.
  // Direct esp_wifi_set_channel() works correctly when the STA is not connected.
  esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
}

// ESP-Now send-status callback -- the only way to learn whether esp_now_send()
// actually made it onto the air. esp_now_send() itself only reports whether the
// driver *accepted* the request, not whether the frame was transmitted; a
// counter that only tracks calls made (like jcmkSentCount) can look perfectly
// healthy locally while every frame is silently failing at the radio layer.
static void jcmkOnSent(const esp_now_send_info_t* txInfo, esp_now_send_status_t status) {
  if (status != ESP_NOW_SEND_SUCCESS) {
    jcmkSendFailCount++;
    // txInfo->des_addr tells us whether this failure was for a unicast (Core
    // MAC) or broadcast (FF:FF:FF:FF:FF:FF) send -- a FAIL status here means
    // the frame was accepted by the driver and put on the air, but no
    // link-layer ACK came back (out of range, interference, or the peer
    // wasn't actually listening on this channel at that instant).
    if (txInfo && txInfo->des_addr) {
      const uint8_t* d = txInfo->des_addr;
      Serial.printf("[MESH] Send FAILED (no ACK) to %02X:%02X:%02X:%02X:%02X:%02X\n",
        d[0], d[1], d[2], d[3], d[4], d[5]);
    }
  }
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

// Writes the marker + board-type byte + null-padded version string starting
// at msg.text[0], and sets msg.len to PIGLET_MARKER_TOTAL_LEN_V2. Shared by
// every marker-sending function below so the version encoding stays in sync.
static void jcmkFillPigletMarker(jcmk_text_msg_t& msg) {
  msg.len = PIGLET_MARKER_TOTAL_LEN_V2;
  memcpy(msg.text, PIGLET_MARKER, PIGLET_MARKER_LEN);
  msg.text[PIGLET_MARKER_LEN] = (char)PIGLET_THIS_BOARD_TYPE;
  size_t vlen = strlen(FIRMWARE_VERSION);
  if (vlen > PIGLET_VERSION_LEN) vlen = PIGLET_VERSION_LEN;
  memset(msg.text + PIGLET_MARKER_TOTAL_LEN, 0, PIGLET_VERSION_LEN);
  memcpy(msg.text + PIGLET_MARKER_TOTAL_LEN, FIRMWARE_VERSION, vlen);
  msg.text[PIGLET_MARKER_TOTAL_LEN_V2] = '\0';
}

static void jcmkSendCoreRequest() {
  jcmk_text_msg_t msg = {};
  memcpy(msg.magic, JCMK_MAGIC, 4);
  msg.type    = JCMK_MSG_CORE_REQUEST;
  msg.counter = 0;
  // Marker + board-type byte + version identify this as a genuine Piglet
  // node (which board, which firmware) to a Piglet Core; real JCMK hardware
  // and Biscuit Cores simply ignore this payload.
  jcmkFillPigletMarker(msg);
  esp_now_send(JCMK_BCAST, (uint8_t*)&msg, sizeof(msg));
}

static void jcmkSendHeartbeat() {
  jcmk_text_msg_t msg = {};
  memcpy(msg.magic, JCMK_MAGIC, 4);
  msg.type    = JCMK_MSG_HEARTBEAT;
  msg.counter = ++jcmkHbCounter;
  // Carry the marker here too so a Core can learn isPiglet/version even if it
  // first (re)discovers this node via a heartbeat rather than a fresh CORE_REQUEST.
  jcmkFillPigletMarker(msg);
  // Unicast only for a confirmed Piglet-to-Piglet pairing, where we control
  // peer registration on both ends and gain real link-layer retry/ACK.
  // Broadcast for everything else (real JCMK hardware / Biscuit): field
  // testing against real Biscuit hardware showed unicast frames getting a
  // 100% "no ACK" failure rate even though esp_now_send() itself always
  // accepted them -- i.e. the Biscuit's ESP-Now stack doesn't ack unicast
  // from a sender it hasn't itself registered as a peer. Broadcast frames
  // don't require the receiver to have the sender peered at all, which is
  // exactly why the discovery handshake (also broadcast) works fine.
  const uint8_t* dest = jcmkCoreIsPiglet ? jcmkCoreMac : JCMK_BCAST;
  esp_err_t err = esp_now_send(dest, (uint8_t*)&msg, sizeof(msg));
  if (err != ESP_OK) {
    // esp_now_send() itself rejected the call (peer not found, channel
    // mismatch, queue full, etc.) -- distinct from a FAIL reported later by
    // jcmkOnSent(), which means the frame *did* go out but got no ACK.
    uint8_t pri; wifi_second_chan_t sec; esp_wifi_get_channel(&pri, &sec);
    Serial.printf("[MESH] HEARTBEAT send() rejected: err=%d ch=%d dest=%02X:%02X:%02X:%02X:%02X:%02X\n",
      (int)err, pri, dest[0], dest[1], dest[2], dest[3], dest[4], dest[5]);
  }
}

static void jcmkSendText(const String& s) {
  jcmk_text_msg_t msg = {};
  memcpy(msg.magic, JCMK_MAGIC, 4);
  msg.type    = JCMK_MSG_TEXT;
  msg.counter = ++jcmkTextCounter;
  uint16_t slen = (uint16_t)((s.length() < JCMK_TEXT_MAX) ? s.length() : JCMK_TEXT_MAX);
  msg.len = slen;
  memcpy(msg.text, s.c_str(), slen);
  msg.text[slen] = '\0';
  // Always send full struct size — Biscuit Pro drops variable-length packets < 212 bytes.
  // Unicast only for confirmed Piglet-to-Piglet pairings (see jcmkSendHeartbeat
  // comment above for why broadcast is required for real JCMK/Biscuit hardware).
  const uint8_t* dest = jcmkCoreIsPiglet ? jcmkCoreMac : JCMK_BCAST;
  esp_err_t err = esp_now_send(dest, (uint8_t*)&msg, sizeof(msg));
  if (err != ESP_OK) {
    uint8_t pri; wifi_second_chan_t sec; esp_wifi_get_channel(&pri, &sec);
    Serial.printf("[MESH] TEXT send() rejected: err=%d ch=%d dest=%02X:%02X:%02X:%02X:%02X:%02X\n",
      (int)err, pri, dest[0], dest[1], dest[2], dest[3], dest[4], dest[5]);
  }
}

// Generic Biscuit protocol-control reply, always unicast to the Core. Used
// for the type=15 (admin ack) and type=11 (config ack) handshake messages a
// plain-reply Biscuit Core expects during node registration.
static void biscuitSendControl(uint8_t type, uint32_t counter = 0, const char* text = nullptr) {
  jcmk_text_msg_t msg = {};
  memcpy(msg.magic, JCMK_MAGIC, 4);
  msg.type = type;
  msg.counter = counter;
  if (text) {
    msg.len = strlen(text);
    memcpy(msg.text, text, msg.len);
  }
  esp_now_send(jcmkCoreMac, (uint8_t*)&msg, sizeof(msg));
}

// ================================================================
//  Node-side OTA receiver (PIG_MSG_OTA_BEGIN / _DATA / _ACK)
//  Ported from olosnoks/warhog's WarHogNode otaTick()/otaStart()/otaFinish().
//  The recv callback (further down) only enqueues chunks; all flash writes,
//  hashing and acks happen here from nodeOtaTick(), called every
//  nodeModeTick(). Scanning is paused for the duration (see nodeModeTick).
// ================================================================
static void nodeOtaAck(uint8_t status) {
  pig_ota_ack_t a = {};
  memcpy(a.magic, JCMK_MAGIC, 4);
  a.type    = PIG_MSG_OTA_ACK;
  a.session = nodeOtaSession;
  a.next    = nodeOtaWritten;
  a.status  = status;
  esp_now_send(jcmkCoreMac, (uint8_t*)&a, sizeof(a));
}

static void nodeOtaFail(uint8_t status, const char* why) {
  Serial.printf("[OTA] FAILED: %s\n", why);
  Update.abort();
  mbedtls_sha256_free(&nodeOtaShaCtx);
  nodeOtaActive = false;
  nodeOtaAck(status);
}

static void nodeOtaStart() {
  nodeOtaBeginPending = false;
  const pig_ota_begin_t& b = nodeOtaBeginMsg;
  if (nodeOtaActive) {
    // Core re-sent BEGIN because our READY was lost: just repeat it.
    if (b.session == nodeOtaSession) { if (nodeOtaWritten == 0) nodeOtaAck(PIG_OTA_ST_READY); return; }
    Update.abort();
    mbedtls_sha256_free(&nodeOtaShaCtx);
    nodeOtaActive = false;
  }
  nodeOtaSession = b.session;
  nodeOtaWritten = 0;
  uint32_t size = b.size;
  if (size == 0 || ((size + PIG_OTA_CHUNK - 1) / PIG_OTA_CHUNK) != b.chunks) {
    nodeOtaAck(PIG_OTA_ST_ERR_ARGS);
    return;
  }
  // Stop any scan in flight and park on the home channel.
  WiFi.scanDelete();
  nodeScanActive   = false;
  nodeScanAdminWin = false;
  jcmkSetChannel(JCMK_ESPNOW_CH);

  if (!Update.begin(size, U_FLASH)) {
    Serial.printf("[OTA] Update.begin(%lu) failed: %s\n", (unsigned long)size, Update.errorString());
    nodeOtaAck(PIG_OTA_ST_ERR_SPACE);
    return;
  }
  mbedtls_sha256_init(&nodeOtaShaCtx);
  mbedtls_sha256_starts(&nodeOtaShaCtx, 0);
  memcpy(nodeOtaSha, b.sha256, 32);
  nodeOtaChunks   = b.chunks;
  nodeOtaSize     = size;
  nodeOtaBytes    = 0;
  nodeOtaQHead    = nodeOtaQTail = 0;
  nodeOtaQNext    = 0;
  nodeOtaReack    = false;
  nodeOtaLastRxMs = millis();
  nodeOtaActive   = true;
  Serial.printf("[OTA] Start: %lu bytes, %u chunks\n", (unsigned long)size, (unsigned)b.chunks);
  nodeOtaAck(PIG_OTA_ST_READY);
}

static void nodeOtaFinish() {
  uint8_t h[32];
  mbedtls_sha256_finish(&nodeOtaShaCtx, h);
  mbedtls_sha256_free(&nodeOtaShaCtx);
  if (nodeOtaBytes != nodeOtaSize) { nodeOtaFail(PIG_OTA_ST_ERR_ARGS, "size mismatch"); return; }
  if (memcmp(h, nodeOtaSha, 32) != 0) {
    Update.abort();
    nodeOtaActive = false;
    Serial.println("[OTA] FAILED: SHA-256 mismatch — update discarded");
    nodeOtaAck(PIG_OTA_ST_ERR_SHA);
    return;
  }
  if (!Update.end(true)) { nodeOtaFail(PIG_OTA_ST_ERR_WRITE, Update.errorString()); return; }
  nodeOtaActive = false;
  Serial.println("[OTA] Verified — rebooting into the new image");
  for (uint8_t i = 0; i < 3; i++) { nodeOtaAck(PIG_OTA_ST_DONE); delay(30); }
  delay(200);
  ESP.restart();
}

// Called every nodeModeTick(). No-op unless a transfer is pending/active.
static void nodeOtaTick() {
  if (nodeOtaBeginPending) nodeOtaStart();
  if (!nodeOtaActive) return;
  uint32_t now = millis();
  bool progressed = false;
  while (nodeOtaQTail != nodeOtaQHead) {
    NodeOtaSlot& sl = nodeOtaQ[nodeOtaQTail];
    if (Update.write(sl.data, sl.len) != sl.len) { nodeOtaFail(PIG_OTA_ST_ERR_WRITE, "flash write"); return; }
    mbedtls_sha256_update(&nodeOtaShaCtx, sl.data, sl.len);
    nodeOtaBytes += sl.len;
    nodeOtaWritten++;
    nodeOtaQTail = (nodeOtaQTail + 1) % NODE_OTA_QDEPTH;
    progressed = true;
  }
  if (progressed || nodeOtaReack) {
    nodeOtaReack = false;
    nodeOtaAck(PIG_OTA_ST_OK);
  }
  if (progressed) {
    nodeOtaLastRxMs = now;
    jcmkCoreLastSeenMs = now;   // we are talking to the Core: don't time out
  }
  if (nodeOtaWritten == nodeOtaChunks) { nodeOtaFinish(); return; }
  if (now - nodeOtaLastRxMs > NODE_OTA_IDLE_ABORT_MS) {
    Serial.println("[OTA] Timed out waiting for chunks — abandoning update");
    Update.abort();
    mbedtls_sha256_free(&nodeOtaShaCtx);
    nodeOtaActive = false;      // scanning resumes; Core can retry
  }
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
  // Marker + board-type byte + version tell a Piglet node that this Core is
  // also a genuine Piglet (and which firmware it runs), so it can safely
  // switch to slot-scheduled transmit; ignored by third-party nodes.
  jcmkFillPigletMarker(msg);
  esp_now_send(mac, (uint8_t*)&msg, sizeof(msg));
}

// Maps a 5GHz channel number to its index in core5gStat[]/core5gHits[]
// (JCMK_CHANNELS[14..39]), or -1 if `ch` isn't a known 5GHz channel.
static int core5gIndexOf(uint8_t ch) {
  for (uint8_t i = 14; i < JCMK_NUM_CHANNELS; i++)
    if (JCMK_CHANNELS[i] == ch) return (int)(i - 14);
  return -1;
}

// Folds accumulated per-channel hit counts into the yield EWMA once per 4s
// window. Called every coreModeTick(); a no-op between windows.
static void coreYieldUpdate(uint32_t now) {
  if (coreYieldLastMs == 0) { coreYieldLastMs = now; return; }
  if (now - coreYieldLastMs < 4000) return;
  float dt = (float)(now - coreYieldLastMs) / 1000.0f;
  for (uint8_t i = 0; i < CORE_5G_COUNT; i++) {
    uint8_t h = core5gHits[i] > 255 ? 255 : (uint8_t)core5gHits[i];
    sched_on_yield(core5gStat, i, h, h, dt, coreSchedCfg);
    core5gHits[i] = 0;
  }
  coreYieldLastMs = now;
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
  // (not indices) in a comma-separated list, which Biscuit's wire format
  // allows in any order. The SET of channels in [startIdx, endIdx] is exactly
  // what today's plain contiguous split assigned this node -- unchanged --
  // but the 5GHz ones among them are reordered by descending measured yield
  // (see core5gStat / YieldSched.h) so the most productive channels this
  // node has access to are listed first. 2.4GHz channels (always worth
  // scanning) are kept in natural order ahead of the ranked 5GHz ones.
  uint8_t end = (endIdx < JCMK_NUM_CHANNELS) ? endIdx : (JCMK_NUM_CHANNELS - 1);
  uint8_t lo[JCMK_NUM_CHANNELS]; uint8_t loN = 0;
  uint8_t hi[JCMK_NUM_CHANNELS]; uint8_t hiN = 0;
  ChanStat hiStat[JCMK_NUM_CHANNELS];
  for (uint8_t j = startIdx; j <= end; j++) {
    uint8_t ch = JCMK_CHANNELS[j];
    if (ch <= 14) {
      lo[loN++] = ch;
    } else {
      int gi = core5gIndexOf(ch);
      hi[hiN] = ch;
      hiStat[hiN] = (gi >= 0) ? core5gStat[gi] : ChanStat{};
      hiN++;
    }
  }
  uint8_t order[JCMK_NUM_CHANNELS];
  if (hiN > 0) sched_rank(hiStat, hi, hiN, millis(), coreSchedCfg, order);

  jcmk_text_msg_t cfgMsg = {};
  memcpy(cfgMsg.magic, JCMK_MAGIC, 4);
  cfgMsg.type    = 10;  // MSG_CONFIG_UPDATE
  cfgMsg.counter = 0;

  String chList = "channels=";
  bool first = true;
  for (uint8_t j = 0; j < loN; j++) {
    if (!first) chList += ",";
    chList += String(lo[j]);
    first = false;
  }
  for (uint8_t j = 0; j < hiN; j++) {
    if (!first) chList += ",";
    chList += String(hi[order[j]]);
    first = false;
  }
  chList += ";dwell=";
  chList += String((uint32_t)NODE_SCAN_DWELL_MS);

  uint16_t slen = (chList.length() < JCMK_TEXT_MAX)
                ? (uint16_t)chList.length() : (uint16_t)JCMK_TEXT_MAX;
  cfgMsg.len = slen;
  memcpy(cfgMsg.text, chList.c_str(), slen);
  cfgMsg.text[slen] = '\0';
  esp_now_send(mac, (uint8_t*)&cfgMsg, sizeof(cfgMsg));

  Serial.printf("[CORE] Biscuit role+config sent (ch %d-%d, dwell %dms, yield-ranked)\n",
    JCMK_CHANNELS[startIdx],
    JCMK_CHANNELS[end],
    (int)NODE_SCAN_DWELL_MS);
}

// ================================================================
//  Core-side OTA engine (PIG_MSG_OTA_BEGIN / _DATA / _ACK)
//  Ported from olosnoks/warhog's coreOta* functions. Pushes a connected
//  node's app image over the existing ESP-NOW link -- no USB. Images are
//  staged once per boot from the Core's own SD card (/Firmware/<Board>.ota.bin,
//  one per PigletBoardType) into PSRAM, since mid-session SD reads are not
//  reliable once Wi-Fi/BLE are both running. coreOtaStart() just arms a
//  transfer; coreOtaTick() (called from coreModeTick()) drives it.
// ================================================================
static const uint32_t CORE_OTA_BEGIN_RETRY_MS   = 400;      // resend OTA_BEGIN this often
static const uint32_t CORE_OTA_START_TIMEOUT_MS = 20000;    // node never answered
static const uint32_t CORE_OTA_ACK_TIMEOUT_MS   = 250;      // no ack progress -> resend window
static const uint32_t CORE_OTA_STALL_MS         = 10000;    // no ack progress at all -> give up
static const uint32_t CORE_OTA_RESULT_SHOW_MS   = 15000;    // keep Done/Failed visible this long
static const uint32_t CORE_OTA_MAX_IMAGE        = 0x330000; // size of an ota_0/ota_1 slot in default_8MB
static const uint16_t CORE_OTA_CHIP_ESP32C5     = 23;       // esp_image_header_t.chip_id for the C5

// Per-board-type image, preloaded from the Core's SD card at boot.
struct CoreOtaImage {
  uint8_t     boardType;    // PigletBoardType this file is for
  const char* path;         // /Firmware/<Board>.ota.bin
  const char* label;        // short tag for status messages
  uint8_t*    image;        // whole image in PSRAM, null if not loaded
  uint32_t    size;
  uint16_t    chunks;
  uint8_t     sha[32];
  char        failMsg[28];  // why this image is not loaded
};
static CoreOtaImage coreOtaImages[] = {
  { PIGLET_BOARD_NODE,    "/Firmware/PigletNode.ota.bin",       "Node",    nullptr, 0, 0, {0}, {0} },
  { PIGLET_BOARD_XIAO,    "/Firmware/Piglet.ota.bin",           "XIAO",    nullptr, 0, 0, {0}, {0} },
  { PIGLET_BOARD_TDONGLE, "/Firmware/TDongleC5_Piglet.ota.bin", "TDongle", nullptr, 0, 0, {0}, {0} },
};
static const int CORE_OTA_IMAGE_COUNT = (int)(sizeof(coreOtaImages) / sizeof(coreOtaImages[0]));

static struct {
  CoreOtaPhase phase   = CoreOtaPhase::Idle;
  int8_t       node    = -1;
  uint8_t      mac[6]  = {0};
  uint8_t      session = 0;
  uint8_t*     image   = nullptr;   // whole node image, in PSRAM (null = none loaded)
  uint32_t     size    = 0;
  uint16_t     chunks  = 0;
  uint8_t      sha[32] = {0};
  char         loadMsg[28] = {0};
  uint16_t     base    = 0;         // chunks the node has acknowledged
  uint16_t     nextSend = 0;
  uint32_t     startMs = 0, lastBeginMs = 0, ackMs = 0, progressMs = 0, endMs = 0;
  char         msg[28] = {0};
} coreOta;

// Written by the ESP-NOW callback, consumed by coreOtaTick().
static volatile bool     coreOtaGotReady  = false;
static volatile uint16_t coreOtaAckNext   = 0;
static volatile uint8_t  coreOtaFinalStat = 0xFF;   // PIG_OTA_ST_DONE / PIG_OTA_ST_ERR_*; 0xFF = none yet

static bool coreOtaBusy() {
  return coreOta.phase == CoreOtaPhase::Starting || coreOta.phase == CoreOtaPhase::Sending;
}

static void coreOtaSetMsg(const char* m) { snprintf(coreOta.msg, sizeof(coreOta.msg), "%s", m); }
static void coreOtaLoadFail(const char* m) {
  snprintf(coreOta.loadMsg, sizeof(coreOta.loadMsg), "%s", m);
  Serial.printf("[OTA] no image loaded: %s\n", m);
}

// Load one board's image file from SD into PSRAM, validating its ESP image
// magic + chip_id. A missing file is not an error here (that board may simply
// not be part of this mesh) -- it only matters once coreOtaStart() wants it.
static void coreOtaLoadOne(CoreOtaImage& im) {
  if (im.image) { heap_caps_free(im.image); im.image = nullptr; }
  im.size = 0; im.chunks = 0; im.failMsg[0] = 0;
  if (!SD.exists(im.path)) { snprintf(im.failMsg, sizeof(im.failMsg), "NO IMAGE ON SD"); return; }
  File f = SD.open(im.path, FILE_READ);
  if (!f) { snprintf(im.failMsg, sizeof(im.failMsg), "CANNOT OPEN IMAGE"); return; }
  uint32_t size = (uint32_t)f.size();
  if (size < 1024 || size > CORE_OTA_MAX_IMAGE) {
    f.close(); snprintf(im.failMsg, sizeof(im.failMsg), "BAD IMAGE SIZE"); return;
  }
  uint8_t* img = (uint8_t*)heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!img) { f.close(); snprintf(im.failMsg, sizeof(im.failMsg), "OUT OF MEMORY"); return; }
  uint32_t got = 0;
  uint8_t buf[4096];
  while (got < size) {
    uint32_t want = size - got; if (want > sizeof(buf)) want = (uint32_t)sizeof(buf);
    int n = f.read(buf, want);
    if (n <= 0) break;
    memcpy(img + got, buf, (size_t)n);
    got += (uint32_t)n;
  }
  f.close();
  if (got != size) { heap_caps_free(img); snprintf(im.failMsg, sizeof(im.failMsg), "SD READ ERROR"); return; }
  if (img[0] != 0xE9) { heap_caps_free(img); snprintf(im.failMsg, sizeof(im.failMsg), "NOT AN APP IMAGE"); return; }
  uint16_t chip = (uint16_t)(img[12] | (img[13] << 8));
  if (chip != CORE_OTA_CHIP_ESP32C5) {
    heap_caps_free(img); snprintf(im.failMsg, sizeof(im.failMsg), "NOT A C5 IMAGE"); return;
  }
  mbedtls_sha256(img, size, im.sha, 0);
  im.image  = img;
  im.size   = size;
  im.chunks = (uint16_t)((size + PIG_OTA_CHUNK - 1) / PIG_OTA_CHUNK);
  Serial.printf("[OTA] %s image loaded: %lu bytes, %u chunks\n", im.label, (unsigned long)size, (unsigned)im.chunks);
}

// True if a loaded (PSRAM-resident) image is available for pushing to a node
// of the given board type. Used by the update-prompt/auto-push logic below
// to avoid offering/attempting a push with nothing to actually send.
static bool coreOtaHasImage(uint8_t boardType) {
  for (int i = 0; i < CORE_OTA_IMAGE_COUNT; i++)
    if (coreOtaImages[i].boardType == boardType) return coreOtaImages[i].image != nullptr;
  return false;
}

// Call once from setup(), right after the SD card mounts. Loads every board's
// image present on SD and leaves a short per-board summary for the Web UI.
void coreOtaPreload() {
  coreOta.loadMsg[0] = 0;
  if (!sdOk) { coreOtaLoadFail("NO SD CARD"); return; }
  if (!SD.exists("/Firmware")) SD.mkdir("/Firmware");
  int loaded = 0;
  String summary;
  for (int i = 0; i < CORE_OTA_IMAGE_COUNT; i++) {
    coreOtaLoadOne(coreOtaImages[i]);
    if (coreOtaImages[i].image) loaded++;
    if (summary.length()) summary += " ";
    summary += coreOtaImages[i].label;
    summary += ":";
    summary += coreOtaImages[i].image ? "ok" : "-";
  }
  if (loaded == 0) coreOtaLoadFail("NO FIRMWARE IMAGES ON SD");
  else snprintf(coreOta.loadMsg, sizeof(coreOta.loadMsg), "%s", summary.c_str());
}

static void coreOtaClose(CoreOtaPhase endPhase, const char* msg) {
  coreOta.phase = endPhase;
  coreOta.endMs = millis();
  coreOtaSetMsg(msg);
}

// ESP-NOW callback context: record only, no sends.
static void coreOtaOnAck(const uint8_t* mac, const uint8_t* data, int len) {
  if (len < (int)sizeof(pig_ota_ack_t)) return;
  if (coreOta.phase != CoreOtaPhase::Starting && coreOta.phase != CoreOtaPhase::Sending) return;
  if (memcmp(mac, coreOta.mac, 6) != 0) return;
  const pig_ota_ack_t* a = (const pig_ota_ack_t*)data;
  if (a->session != coreOta.session) return;
  if (coreOta.node >= 0) coreNodes[coreOta.node].lastHbMs = millis();   // an ack proves the node is alive
  if (a->status == PIG_OTA_ST_READY) coreOtaGotReady = true;
  else if (a->status >= PIG_OTA_ST_DONE) coreOtaFinalStat = a->status;
  if (a->next > coreOtaAckNext) coreOtaAckNext = a->next;
}

bool coreOtaStart(uint8_t nodeIdx) {
  if (coreOtaBusy()) return false;
  coreOta.node  = (int8_t)nodeIdx;
  coreOta.phase = CoreOtaPhase::Failed;      // until proven otherwise; keeps msg visible
  coreOta.endMs = millis();
  if (!meshCoreActive || nodeIdx >= CORE_MAX_NODES || !coreNodes[nodeIdx].active) {
    coreOtaSetMsg("NODE NOT CONNECTED"); return false;
  }
  uint8_t bt = coreNodes[nodeIdx].boardType;
  if (bt == PIGLET_BOARD_UNKNOWN) { coreOtaSetMsg("BOARD TYPE UNKNOWN"); return false; }
  CoreOtaImage* im = nullptr;
  for (int i = 0; i < CORE_OTA_IMAGE_COUNT; i++)
    if (coreOtaImages[i].boardType == bt) { im = &coreOtaImages[i]; break; }
  if (!im || !im->image) {
    coreOtaSetMsg(im && im->failMsg[0] ? im->failMsg : "NO IMAGE FOR NODE");
    return false;
  }
  coreOta.image  = im->image;
  coreOta.size   = im->size;
  coreOta.chunks = im->chunks;
  memcpy(coreOta.sha, im->sha, 32);

  memcpy(coreOta.mac, coreNodes[nodeIdx].mac, 6);
  coreOta.session  = (uint8_t)(esp_random() & 0xFF);
  coreOta.base     = coreOta.nextSend = 0;
  coreOtaGotReady  = false;
  coreOtaAckNext   = 0;
  coreOtaFinalStat = 0xFF;
  coreOtaSetMsg("");
  coreOta.startMs     = millis();
  coreOta.lastBeginMs = 0;
  coreOta.phase       = CoreOtaPhase::Starting;
  Serial.printf("[OTA] node %u: %lu bytes, %u chunks, session %u\n", (unsigned)nodeIdx,
                (unsigned long)coreOta.size, (unsigned)coreOta.chunks, (unsigned)coreOta.session);
  return true;
}

void coreOtaAbort() {
  if (!coreOtaBusy()) return;
  coreOtaClose(CoreOtaPhase::Failed, "CANCELLED");   // the node abandons an idle update after 15 s
}

CoreOtaStatus coreOtaStatus() {
  CoreOtaStatus st = {};
  st.phase      = coreOta.phase;
  st.nodeIdx    = coreOta.node;
  st.totalBytes = coreOta.size;
  uint32_t b    = (uint32_t)coreOta.base * PIG_OTA_CHUNK;
  st.sentBytes  = b > coreOta.size ? coreOta.size : b;
  snprintf(st.msg, sizeof(st.msg), "%s", coreOta.msg);
  snprintf(st.loadMsg, sizeof(st.loadMsg), "%s", coreOta.loadMsg);
  return st;
}

static bool coreOtaSendBegin() {
  pig_ota_begin_t b = {};
  memcpy(b.magic, JCMK_MAGIC, 4);
  b.type    = PIG_MSG_OTA_BEGIN;
  b.session = coreOta.session;
  b.size    = coreOta.size;
  b.chunks  = coreOta.chunks;
  memcpy(b.sha256, coreOta.sha, 32);
  return esp_now_send(coreOta.mac, (uint8_t*)&b, sizeof(b)) == ESP_OK;
}

// Returns false if the chunk could not be sent right now (retry next tick).
static bool coreOtaSendChunk(uint16_t seq) {
  pig_ota_data_t d;
  memcpy(d.magic, JCMK_MAGIC, 4);
  d.type    = PIG_MSG_OTA_DATA;
  d.session = coreOta.session;
  d.seq     = seq;
  uint32_t off = (uint32_t)seq * PIG_OTA_CHUNK;
  uint32_t n   = coreOta.size - off;
  if (n > PIG_OTA_CHUNK) n = PIG_OTA_CHUNK;
  memcpy(d.data, coreOta.image + off, n);
  d.len = (uint8_t)n;
  return esp_now_send(coreOta.mac, (uint8_t*)&d, offsetof(pig_ota_data_t, data) + n) == ESP_OK;
}

static const char* coreOtaErrText(uint8_t st) {
  switch (st) {
    case PIG_OTA_ST_ERR_SPACE: return "NODE HAS NO OTA SLOT";   // old flash layout: needs one reflash
    case PIG_OTA_ST_ERR_WRITE: return "NODE FLASH WRITE ERR";
    case PIG_OTA_ST_ERR_SHA:   return "NODE: SHA MISMATCH";
    default:                   return "NODE REJECTED UPDATE";
  }
}

// Called every coreModeTick(). No-op unless a transfer is pending/active.
static void coreOtaTick() {
  uint32_t now = millis();
  if (!coreOtaBusy()) {
    if ((coreOta.phase == CoreOtaPhase::Done || coreOta.phase == CoreOtaPhase::Failed) &&
        now - coreOta.endMs > CORE_OTA_RESULT_SHOW_MS) {
      coreOta.phase = CoreOtaPhase::Idle;
      coreOta.node  = -1;
    }
    return;
  }
  if (coreOta.node < 0 || !coreNodes[coreOta.node].active) {
    coreOtaClose(CoreOtaPhase::Failed, "NODE LOST");
    return;
  }

  switch (coreOta.phase) {
    case CoreOtaPhase::Starting:
      if (coreOtaFinalStat != 0xFF && coreOtaFinalStat != PIG_OTA_ST_DONE) {
        coreOtaClose(CoreOtaPhase::Failed, coreOtaErrText(coreOtaFinalStat));
      } else if (coreOtaGotReady) {
        coreOta.phase = CoreOtaPhase::Sending;
        coreOta.base = coreOta.nextSend = 0;
        coreOta.ackMs = coreOta.progressMs = now;
        Serial.println("[OTA] node ready — sending image");
      } else if (now - coreOta.startMs > CORE_OTA_START_TIMEOUT_MS) {
        coreOtaClose(CoreOtaPhase::Failed, "NODE DID NOT REPLY");
      } else if (coreOta.lastBeginMs == 0 || now - coreOta.lastBeginMs >= CORE_OTA_BEGIN_RETRY_MS) {
        coreOta.lastBeginMs = now;
        coreOtaSendBegin();
      }
      break;

    case CoreOtaPhase::Sending: {
      uint8_t fin = coreOtaFinalStat;
      if (fin == PIG_OTA_ST_DONE) { coreOta.base = coreOta.chunks; coreOtaClose(CoreOtaPhase::Done, "UPDATED - REBOOTING"); return; }
      if (fin != 0xFF)            { coreOtaClose(CoreOtaPhase::Failed, coreOtaErrText(fin)); return; }

      uint16_t acked = coreOtaAckNext;
      if (acked > coreOta.chunks) acked = coreOta.chunks;
      if (acked > coreOta.base) { coreOta.base = acked; coreOta.ackMs = coreOta.progressMs = now; }
      if (coreOta.nextSend < coreOta.base) coreOta.nextSend = coreOta.base;

      if (now - coreOta.progressMs > CORE_OTA_STALL_MS) { coreOtaClose(CoreOtaPhase::Failed, "TRANSFER STALLED"); return; }
      // Lost frame or ack: go back to the first unacknowledged chunk.
      if (coreOta.nextSend > coreOta.base && now - coreOta.ackMs > CORE_OTA_ACK_TIMEOUT_MS) {
        coreOta.nextSend = coreOta.base;
        coreOta.ackMs    = now;
      }
      while (coreOta.nextSend < coreOta.chunks && coreOta.nextSend < (uint16_t)(coreOta.base + PIG_OTA_WINDOW)) {
        if (!coreOtaSendChunk(coreOta.nextSend)) break;   // busy radio -> retry next tick
        coreOta.nextSend++;
      }
      break;
    }
    default: break;
  }
}

// ================================================================
//  On-device "Update Nodes through Core?" prompt (XIAO nodes) and
//  automatic push to out-of-date headless PigletNode nodes
//  (cfg.autoUpdateHeadlessNodes). Both share the single-transfer-at-a-time
//  coreOta engine above; neither ever starts a push while coreOtaBusy().
//  Rendered by drawPageMeshNode() below; the mesh-page button handler in
//  Piglet.ino calls coreUpdatePromptConfirm() when a single press should be
//  treated as "confirm" (gated on coreUpdatePromptStatus().promptActive).
// ================================================================
static const uint32_t CORE_UPDATE_PROMPT_MS = 30000;  // confirm window
static uint8_t  pendingUpdateNodes[CORE_MAX_NODES];
static uint8_t  pendingUpdateCount  = 0;
static bool     updatePromptActive  = false;
static uint32_t updatePromptStartMs = 0;
static int8_t   updateExecutingPos  = -1;  // index into pendingUpdateNodes[], -1 = not executing

static bool coreUpdateAlreadyQueued(uint8_t nodeIdx) {
  for (uint8_t i = 0; i < pendingUpdateCount; i++) if (pendingUpdateNodes[i] == nodeIdx) return true;
  return false;
}

// Scans the roster for newly-mismatched XIAO nodes (queued for the on-screen
// prompt) and headless PigletNode auto-push candidates (pushed immediately,
// no prompt -- PigletNode has no UI of its own). Called every coreModeTick().
static void coreUpdateDetect() {
  for (uint8_t i = 0; i < CORE_MAX_NODES; i++) {
    CoreNodeInfo& n = coreNodes[i];
    if (!n.active || !n.isPiglet || n.version[0] == '\0') continue;  // unknown version -> never flagged
    if (strncmp(n.version, FIRMWARE_VERSION, sizeof(n.version)) == 0) continue;  // matches Core -- fine

    if (n.boardType == PIGLET_BOARD_XIAO) {
      if (n.updateDeclined || coreUpdateAlreadyQueued(i)) continue;
      if (!coreOtaHasImage(PIGLET_BOARD_XIAO)) continue;
      if (pendingUpdateCount < CORE_MAX_NODES) pendingUpdateNodes[pendingUpdateCount++] = i;
    } else if (n.boardType == PIGLET_BOARD_NODE && cfg.autoUpdateHeadlessNodes) {
      if (n.autoUpdateAttempted || coreOtaBusy()) continue;
      if (!coreOtaHasImage(PIGLET_BOARD_NODE)) continue;
      n.autoUpdateAttempted = true;  // set before starting -- never retry-loop a failing node
      Serial.printf("[CORE] Auto-updating headless node %d (%s -> %s)\n", i, n.version, FIRMWARE_VERSION);
      coreOtaStart(i);
    }
  }
}

// Drives the 30 s confirm window and, once confirmed, steps the queue one
// node at a time through the shared coreOta engine. Called every
// coreModeTick(), after coreOtaTick() so a just-finished push is reflected
// immediately. A no-op whenever there's nothing pending/executing.
static void coreUpdateProcessTick() {
  uint32_t now = millis();

  if (updateExecutingPos >= 0) {
    if (coreOtaBusy()) return;  // still sending (or just started) -- wait
    updateExecutingPos++;
    if (updateExecutingPos >= (int)pendingUpdateCount) {
      pendingUpdateCount = 0;
      updateExecutingPos = -1;
    } else {
      coreOtaStart(pendingUpdateNodes[updateExecutingPos]);
    }
    return;
  }

  if (updatePromptActive) {
    if (now - updatePromptStartMs >= CORE_UPDATE_PROMPT_MS) {
      for (uint8_t i = 0; i < pendingUpdateCount; i++) {
        uint8_t idx = pendingUpdateNodes[i];
        if (idx < CORE_MAX_NODES) coreNodes[idx].updateDeclined = true;
      }
      pendingUpdateCount = 0;
      updatePromptActive = false;
    }
    return;
  }

  if (pendingUpdateCount > 0 && !coreOtaBusy()) {
    updatePromptActive  = true;
    updatePromptStartMs = now;
  }
}

CoreUpdatePromptStatus coreUpdatePromptStatus() {
  CoreUpdatePromptStatus st = {};
  st.promptActive = updatePromptActive;
  st.promptRemainingMs = updatePromptActive
    ? (uint32_t)(CORE_UPDATE_PROMPT_MS - (millis() - updatePromptStartMs)) : 0;
  st.pendingCount = pendingUpdateCount;
  st.pushActive   = coreOtaBusy();
  CoreOtaStatus ota = coreOtaStatus();
  st.pushNodeIdx  = (ota.nodeIdx >= 0) ? (uint8_t)ota.nodeIdx : 0;  // valid only when pushActive
  return st;
}

// Called from the mesh-page button handler when a single press lands while
// coreUpdatePromptStatus().promptActive is true.
void coreUpdatePromptConfirm() {
  if (!updatePromptActive || pendingUpdateCount == 0) return;
  updatePromptActive = false;
  updateExecutingPos = 0;
  coreOtaStart(pendingUpdateNodes[0]);
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
      Serial.printf("[CORE] RX CORE_REQUEST from %02X:%02X:%02X:%02X:%02X:%02X len=%d\n",
        info->src_addr[0],info->src_addr[1],info->src_addr[2],
        info->src_addr[3],info->src_addr[4],info->src_addr[5], len);
      coreSendReply(info->src_addr);  // reply immediately (safe from callback)
      uint8_t next = (coreReqTail + 1) % CORE_REQ_QUEUE;
      if (next != coreReqHead) {
        memcpy(coreReqBuf[coreReqTail].mac, info->src_addr, 6);
        // Biscuit Node always sends full-size packets (212 bytes); JCMK sends 5 bytes.
        coreReqBuf[coreReqTail].isBiscuit = (len >= (int)sizeof(jcmk_text_msg_t));
        coreReqBuf[coreReqTail].isPiglet  = jcmkHasPigletMarker(data, len);
        coreReqBuf[coreReqTail].boardType = jcmkGetPigletBoardType(data, len);
        jcmkGetPigletVersion(data, len, coreReqBuf[coreReqTail].version);
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
            // TEXT carries scan-line data, not the marker -- a node rediscovered
            // via TEXT (rare; HEARTBEAT/CORE_REQUEST arrive far more often) is
            // conservatively treated as non-Piglet until its next heartbeat.
            coreReqBuf[coreReqTail].isPiglet  = false;
            coreReqBuf[coreReqTail].boardType = PIGLET_BOARD_UNKNOWN;
            coreReqBuf[coreReqTail].version[0] = '\0';
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
          // Refresh version directly here (ISR-safe memcpy, no flash/SD
          // access) -- a node that was just pushed a firmware update and
          // rebooted needs this to clear the mismatch on its very next
          // heartbeat, not just at (re)registration time.
          jcmkGetPigletVersion(data, len, coreNodes[i].version);
          found = true; break;
        }
      }
      if (!found) {
        uint8_t nxt = (coreReqTail + 1) % CORE_REQ_QUEUE;
        if (nxt != coreReqHead) {
          memcpy(coreReqBuf[coreReqTail].mac, info->src_addr, 6);
          coreReqBuf[coreReqTail].isBiscuit = (len >= (int)sizeof(jcmk_text_msg_t));
          coreReqBuf[coreReqTail].isPiglet  = jcmkHasPigletMarker(data, len);
          coreReqBuf[coreReqTail].boardType = jcmkGetPigletBoardType(data, len);
          jcmkGetPigletVersion(data, len, coreReqBuf[coreReqTail].version);
          coreReqTail = nxt;
        }
      }
    } else if (type == PIG_MSG_OTA_ACK) {
      coreOtaOnAck(info->src_addr, data, len);
    }
  } else {
    // ---- Node role (existing) ----
    if (type == JCMK_MSG_CORE_REPLY && !jcmkHaveCore && !jcmkCoreFoundPending) {
      memcpy(jcmkCoreMacPending, info->src_addr, 6);
      jcmkCoreIsPigletPending = jcmkHasPigletMarker(data, len);
      biscuitPlainReplyPending = biscuitHasPlainReply(data, len);
      jcmkCoreFoundPending = true;
    } else if (type == JCMK_MSG_HEARTBEAT && jcmkHaveCore
               && memcmp(info->src_addr, jcmkCoreMac, 6) == 0) {
      // Core's periodic broadcast: liveness signal for timeout detection, and
      // (for Piglet pairings) the shared timing anchor for slot scheduling.
      jcmkCoreLastSeenMs = millis();
      if (jcmkCoreIsPiglet) jcmkCycleEpochMs = millis();
    } else if (jcmkHaveCore && memcmp(info->src_addr, jcmkCoreMac, 6) == 0
               && len >= (int)sizeof(jcmk_text_msg_t) && type == JCMK_MSG_ADMIN) {
      // Plain-reply Biscuit admin handshake step: ack with type=15, echoing
      // back the first text byte (or low byte of counter as a fallback).
      const jcmk_text_msg_t* msg = (const jcmk_text_msg_t*)data;
      jcmkCoreLastSeenMs = millis();
      biscuitSendControl(15, msg->len ? (uint8_t)msg->text[0] : msg->counter & 0xFF);
    } else if (jcmkHaveCore && memcmp(info->src_addr, jcmkCoreMac, 6) == 0
               && len >= (int)sizeof(jcmk_text_msg_t) && type == 10) {
      // Plain-reply Biscuit channel-config message (MSG_CONFIG_UPDATE) --
      // parse the assigned channel list, then ack with type=11.
      jcmkCoreLastSeenMs = millis();
      if (biscuitPlainReplyPending) biscuitSetChannels((const jcmk_text_msg_t*)data);
      biscuitSendControl(11);
    } else if (type == JCMK_MSG_ADMIN && len >= (int)sizeof(jcmk_admin_msg_t)) {
      jcmkCoreLastSeenMs = millis();
      if (jcmkCoreIsPiglet) jcmkCycleEpochMs = millis();
      const jcmk_admin_msg_t* adm = (const jcmk_admin_msg_t*)data;
      if (adm->assignment_version != jcmkAssignVer) {
        jcmkAssignVer = adm->assignment_version;
        jcmkStartIdx  = adm->start_channel_idx;
        jcmkEndIdx    = adm->end_channel_idx;
      }
      // Transmit-slot fields (node_index/node_count) are only meaningful when
      // paired with a confirmed Piglet Core; refreshed every ADMIN regardless
      // of assignment_version since Core recomputes them on every send.
      if (jcmkCoreIsPiglet) {
        jcmkSlotIndex = adm->node_index;
        jcmkSlotCount = (adm->node_count > 0) ? adm->node_count : 1;
      }
    } else if (type == PIG_MSG_OTA_BEGIN && jcmkHaveCore && memcmp(info->src_addr, jcmkCoreMac, 6) == 0
               && len >= (int)sizeof(pig_ota_begin_t)) {
      // ---- Core wants to update us. Queue it for the loop; never touch flash from here.
      if (!nodeOtaBeginPending) { memcpy(&nodeOtaBeginMsg, data, sizeof(nodeOtaBeginMsg)); nodeOtaBeginPending = true; }
    } else if (type == PIG_MSG_OTA_DATA && nodeOtaActive && jcmkHaveCore
               && memcmp(info->src_addr, jcmkCoreMac, 6) == 0
               && len > (int)offsetof(pig_ota_data_t, data)) {
      const pig_ota_data_t* d = (const pig_ota_data_t*)data;
      if (d->session == nodeOtaSession && d->len >= 1 && d->len <= PIG_OTA_CHUNK
          && len >= (int)offsetof(pig_ota_data_t, data) + (int)d->len) {
        if (d->seq == nodeOtaQNext) {
          uint8_t nh = (uint8_t)((nodeOtaQHead + 1) % NODE_OTA_QDEPTH);
          if (nh != nodeOtaQTail) {
            nodeOtaQ[nodeOtaQHead].len = d->len;
            memcpy(nodeOtaQ[nodeOtaQHead].data, d->data, d->len);
            nodeOtaQHead = nh;
            nodeOtaQNext++;
          }
        } else {
          nodeOtaReack = true;   // stale or out-of-order: tell the Core where we are
        }
      }
    }
  }
}

// Forward declaration -- defined further down this file; meshChOverrideSet()/
// meshChOverrideClear() below need to re-apply channel assignments immediately.
static void coreReassignChannels();

// ================================================================
//  Manual per-node channel-range override (Web UI) -- see MeshNode.h.
// ================================================================
#define MESH_CHOVR_MAX CORE_MAX_NODES
struct MeshChanOverride { uint8_t mac[6]; uint8_t startIdx; uint8_t endIdx; };
static MeshChanOverride meshChOverrides[MESH_CHOVR_MAX];
static uint8_t          meshChOverrideCount = 0;
static const char*      MESH_CHOVR_PATH = "/mesh_channel_overrides.cfg";

static bool meshChannelToIndex(int channel, uint8_t& idxOut) {
  for (uint8_t i = 0; i < JCMK_NUM_CHANNELS; i++) {
    if (JCMK_CHANNELS[i] == channel) { idxOut = i; return true; }
  }
  return false;
}

static bool meshMacParse(const String& s, uint8_t* macOut) {
  unsigned int v[6];
  if (sscanf(s.c_str(), "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) return false;
  for (int i = 0; i < 6; i++) macOut[i] = (uint8_t)v[i];
  return true;
}

static void meshChOverrideSave() {
  if (!sdOk) return;
  if (SD.exists(MESH_CHOVR_PATH)) SD.remove(MESH_CHOVR_PATH);
  File f = SD.open(MESH_CHOVR_PATH, FILE_WRITE);
  if (!f) return;
  f.println("# Manual mesh channel overrides: MAC,startChannel,endChannel");
  for (uint8_t i = 0; i < meshChOverrideCount; i++) {
    MeshChanOverride& o = meshChOverrides[i];
    char line[40];
    snprintf(line, sizeof(line), "%02X:%02X:%02X:%02X:%02X:%02X,%d,%d",
      o.mac[0], o.mac[1], o.mac[2], o.mac[3], o.mac[4], o.mac[5],
      JCMK_CHANNELS[o.startIdx], JCMK_CHANNELS[o.endIdx]);
    f.println(line);
  }
  f.close();
}

// Call once from enterCoreMode() -- populates the in-memory table from SD.
static void meshChOverrideLoad() {
  meshChOverrideCount = 0;
  if (!sdOk || !SD.exists(MESH_CHOVR_PATH)) return;
  File f = SD.open(MESH_CHOVR_PATH, FILE_READ);
  if (!f) return;
  while (f.available() && meshChOverrideCount < MESH_CHOVR_MAX) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() == 0 || line[0] == '#') continue;
    int c1 = line.indexOf(',');
    int c2 = (c1 >= 0) ? line.indexOf(',', c1 + 1) : -1;
    if (c1 < 0 || c2 < 0) continue;
    uint8_t mac[6];
    if (!meshMacParse(line.substring(0, c1), mac)) continue;
    uint8_t si, ei;
    int chStart = line.substring(c1 + 1, c2).toInt();
    int chEnd   = line.substring(c2 + 1).toInt();
    if (!meshChannelToIndex(chStart, si) || !meshChannelToIndex(chEnd, ei)) continue;
    if (ei < si) { uint8_t t = si; si = ei; ei = t; }
    memcpy(meshChOverrides[meshChOverrideCount].mac, mac, 6);
    meshChOverrides[meshChOverrideCount].startIdx = si;
    meshChOverrides[meshChOverrideCount].endIdx   = ei;
    meshChOverrideCount++;
  }
  f.close();
  Serial.printf("[CORE] Loaded %d channel override(s)\n", (int)meshChOverrideCount);
}

static bool meshChOverrideFind(const uint8_t* mac, uint8_t& startIdx, uint8_t& endIdx) {
  for (uint8_t i = 0; i < meshChOverrideCount; i++) {
    if (memcmp(meshChOverrides[i].mac, mac, 6) == 0) {
      startIdx = meshChOverrides[i].startIdx;
      endIdx   = meshChOverrides[i].endIdx;
      return true;
    }
  }
  return false;
}

// ---- Public API, declared in MeshNode.h for WebUI.cpp ----
uint8_t meshChOverrideGetCount() { return meshChOverrideCount; }

bool meshChOverrideGetAt(uint8_t idx, uint8_t* macOut, uint16_t& chStartOut, uint16_t& chEndOut) {
  if (idx >= meshChOverrideCount) return false;
  memcpy(macOut, meshChOverrides[idx].mac, 6);
  chStartOut = JCMK_CHANNELS[meshChOverrides[idx].startIdx];
  chEndOut   = JCMK_CHANNELS[meshChOverrides[idx].endIdx];
  return true;
}

bool meshChOverrideSet(const uint8_t* mac, int chStart, int chEnd) {
  uint8_t si, ei;
  if (!meshChannelToIndex(chStart, si) || !meshChannelToIndex(chEnd, ei)) return false;
  if (ei < si) { uint8_t t = si; si = ei; ei = t; }
  for (uint8_t i = 0; i < meshChOverrideCount; i++) {
    if (memcmp(meshChOverrides[i].mac, mac, 6) == 0) {
      meshChOverrides[i].startIdx = si;
      meshChOverrides[i].endIdx   = ei;
      meshChOverrideSave();
      if (meshCoreActive) coreReassignChannels();
      return true;
    }
  }
  if (meshChOverrideCount >= MESH_CHOVR_MAX) return false;
  memcpy(meshChOverrides[meshChOverrideCount].mac, mac, 6);
  meshChOverrides[meshChOverrideCount].startIdx = si;
  meshChOverrides[meshChOverrideCount].endIdx   = ei;
  meshChOverrideCount++;
  meshChOverrideSave();
  if (meshCoreActive) coreReassignChannels();
  return true;
}

bool meshChOverrideClear(const uint8_t* mac) {
  for (uint8_t i = 0; i < meshChOverrideCount; i++) {
    if (memcmp(meshChOverrides[i].mac, mac, 6) == 0) {
      for (uint8_t j = i; j < meshChOverrideCount - 1; j++) meshChOverrides[j] = meshChOverrides[j + 1];
      meshChOverrideCount--;
      meshChOverrideSave();
      if (meshCoreActive) coreReassignChannels();
      return true;
    }
  }
  return false;
}

// ================================================================
//  Core mode helpers (main loop only — not ISR-safe)
// ================================================================
static void coreFindOrAddNode(const uint8_t* mac, bool isBiscuit, bool isPiglet, uint8_t boardType, const char* version) {
  for (uint8_t i = 0; i < CORE_MAX_NODES; i++) {
    if (coreNodes[i].active && memcmp(coreNodes[i].mac, mac, 6) == 0) {
      coreNodes[i].lastHbMs = millis();
      // A node can go from "not yet confirmed Piglet" to confirmed once its
      // first HEARTBEAT/CORE_REQUEST with the marker arrives; never downgrade.
      if (isPiglet) coreNodes[i].isPiglet = true;
      if (coreNodes[i].boardType == PIGLET_BOARD_UNKNOWN) coreNodes[i].boardType = boardType;
      // A changed version (e.g. just pushed a firmware update and rebooted)
      // needs a fresh mismatch evaluation -- clear any stale decline/attempt.
      if (version[0] && strncmp(coreNodes[i].version, version, sizeof(coreNodes[i].version)) != 0) {
        strncpy(coreNodes[i].version, version, sizeof(coreNodes[i].version) - 1);
        coreNodes[i].version[sizeof(coreNodes[i].version) - 1] = '\0';
        coreNodes[i].updateDeclined      = false;
        coreNodes[i].autoUpdateAttempted = false;
      }
      return;  // already registered
    }
  }
  for (uint8_t i = 0; i < CORE_MAX_NODES; i++) {
    if (!coreNodes[i].active) {
      coreNodes[i].active    = true;
      coreNodes[i].lastHbMs  = millis();
      strncpy(coreNodes[i].version, version, sizeof(coreNodes[i].version) - 1);
      coreNodes[i].version[sizeof(coreNodes[i].version) - 1] = '\0';
      coreNodes[i].updateDeclined      = false;
      coreNodes[i].autoUpdateAttempted = false;
      coreNodes[i].recordsRx = 0;
      coreNodes[i].isBiscuit = isBiscuit;
      coreNodes[i].isPiglet  = isPiglet;
      coreNodes[i].boardType = boardType;
      coreNodes[i].slotIndex = 0;  // assigned by coreReassignChannels() below
      memcpy(coreNodes[i].mac, mac, 6);
      coreNodeCount++;
      jcmkAddPeer(mac);
      // Re-send CORE_REPLY now that the peer is registered.
      // The initial reply from jcmkOnRecv() may have failed because the node's
      // MAC wasn't yet in the peer list when esp_now_send() was called.
      coreSendReply(mac);
      Serial.printf("[CORE] New %s node %d: %02X:%02X:%02X:%02X:%02X:%02X\n",
        isPiglet ? "Piglet" : (isBiscuit ? "Biscuit" : "JCMK"),
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

  // Manual overrides win over the even split for any matching MAC -- applied
  // after the split so an overridden node's range doesn't skew how the
  // remaining channels are divided among everyone else.
  for (uint8_t n = 0; n < count; n++) {
    uint8_t si, ei;
    if (meshChOverrideFind(coreNodes[slots[n]].mac, si, ei)) {
      coreNodes[slots[n]].startIdx = si;
      coreNodes[slots[n]].endIdx   = ei;
    }
  }
  coreAssignVer++;

  // Assign deterministic transmit-slot indices among Piglet-confirmed nodes
  // only -- JCMK/Biscuit nodes don't participate in slot scheduling and are
  // completely unaffected by this (their node_index/node_count keep the
  // legacy per-all-nodes numbering, which both already ignore anyway).
  uint8_t pigletIdx = 0;
  for (uint8_t n = 0; n < count; n++) {
    if (coreNodes[slots[n]].isPiglet) coreNodes[slots[n]].slotIndex = pigletIdx++;
  }
  corePigletSlotCount = pigletIdx;

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
      msg.assignment_version = coreAssignVer;
      msg.start_channel_idx  = coreNodes[slot].startIdx;
      msg.end_channel_idx    = coreNodes[slot].endIdx;
      if (coreNodes[slot].isPiglet) {
        msg.node_index = coreNodes[slot].slotIndex;
        msg.node_count = corePigletSlotCount;
      } else {
        msg.node_index = n;      // legacy semantics for non-Piglet peers (unused by them)
        msg.node_count = count;
      }
      esp_now_send(coreNodes[slot].mac, (uint8_t*)&msg, sizeof(msg));
    }
    delay(10);
  }
  Serial.printf("[CORE] Reassigned channels: %d nodes v%d (%d Piglet slot%s)\n",
    count, coreAssignVer, corePigletSlotCount, corePigletSlotCount == 1 ? "" : "s");
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
      msg.assignment_version = coreAssignVer;
      msg.start_channel_idx  = coreNodes[i].startIdx;
      msg.end_channel_idx    = coreNodes[i].endIdx;
      if (coreNodes[i].isPiglet) {
        msg.node_index = coreNodes[i].slotIndex;
        msg.node_count = corePigletSlotCount;
      } else {
        msg.node_index = n;
        msg.node_count = coreNodeCount;
      }
      esp_now_send(coreNodes[i].mac, (uint8_t*)&msg, sizeof(msg));
    }
    n++;
    delay(10);
  }
}

static void coreSendHeartbeatToAll() {
  // Broadcast a single heartbeat instead of per-node unicast.
  // At 12 nodes, unicast was 12 × 212-byte sends with 10 ms gaps = 120+ ms of
  // blocking channel time. One broadcast reaches all nodes simultaneously and
  // keeps ch 6 clear for incoming TEXT bursts.
  jcmk_text_msg_t msg = {};
  memcpy(msg.magic, JCMK_MAGIC, 4);
  msg.type    = JCMK_MSG_HEARTBEAT;
  msg.counter = ++coreHbCounter;
  msg.len     = 0;
  esp_now_send(JCMK_BCAST, (uint8_t*)&msg, sizeof(msg));
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
  wardriveGoPushAp(bssid, ssid, auth, ch, rssi);
  if (appendWigleRow(bssid, ssid, auth, iso8601NowUTC(), ch, rssi, lat, lon, altM, accM)) {
    coreRecordsRx++;
  }
  // Feed the yield tracker regardless of source node -- see core5gStat above.
  if (ch > 14) {
    int gi = core5gIndexOf((uint8_t)ch);
    if (gi >= 0) core5gHits[gi]++;
  }
}

// ================================================================
//  Per-channel async scan (JCMK startNextNodeAssignedScan pattern).
//  Scans one assigned channel per call at NODE_SCAN_DWELL_MS dwell,
//  cycles through the full assigned range, then enters a ch-6 admin
//  window (heartbeat + NODE_ADMIN_WIN_MS) before the next cycle.
// ================================================================
static void nodeDoScanTick() {
  // The Wardrive Go app's channel selection takes effect here, not just as a filter
  // on what gets reported to it -- but a real Biscuit Core's channel assignment still
  // wins if both happen to be active at once (biscuitChannelCount checked first below).
  bool useWgChannels = !biscuitChannelCount &&
                       wardriveGoChannelOverride(wgOverrideChannels, &wgOverrideCount);

  uint8_t numCh = biscuitChannelCount ? biscuitChannelCount :
                  useWgChannels ? wgOverrideCount :
                  ((jcmkEndIdx >= jcmkStartIdx) ? (jcmkEndIdx - jcmkStartIdx + 1) : 0);
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
      // Random jitter (0-200 ms) staggers admin windows so nodes with similar
      // channel counts don't all transmit on ch 6 at the same instant.
      nodeScanAdminMs  = millis() + (uint32_t)(esp_random() % 200);
      return;
    }

    uint8_t chIdx = jcmkStartIdx + nodeScanChOffset;
    if (!biscuitChannelCount && !useWgChannels && chIdx >= JCMK_NUM_CHANNELS) { nodeScanChOffset++; return; }
    uint8_t channel = biscuitChannelCount ? biscuitChannels[nodeScanChOffset]
                     : useWgChannels      ? wgOverrideChannels[nodeScanChOffset]
                                          : JCMK_CHANNELS[chIdx];

    // Skip 5 GHz channels on 2.4 GHz-only hardware (S3, C6).
    // wardriverIsC5() returns true only for C5 (dual-band); all others are 2.4 GHz only.
    if (channel > 14 && !wardriverIsC5()) {
      nodeScanChOffset++;  // hop over without attempting the scan
      return;
    }

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

    if (!jcmkCoreIsPiglet) {
      // Legacy immediate-send path: non-Piglet Core (real JCMK hardware or
      // Biscuit Pro), or Core type not yet confirmed. Unchanged from before.
      jcmkSetChannel(JCMK_ESPNOW_CH);
      for (int i = 0; i < n; i++) {
        String bssid = WiFi.BSSIDstr(i);
        String ssid  = WiFi.SSID(i);
        String auth  = meshAuthModeToString(WiFi.encryptionType(i));
        int    ch    = WiFi.channel(i);
        int    rssi  = WiFi.RSSI(i);
        // Pushed before the Biscuit bracket-notation rewrite below so the
        // phone always gets the plain WiGLE-style auth token (OPEN/WPA2/...).
        wardriveGoPushAp(bssid, ssid, auth, ch, rssi);
        if (biscuitPlainReplyPending) {
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
        String line  = bssid + "," + ssid + "," + auth + ","
                     + String(ch) + "," + String(rssi) + ",W";
        // Log the exact payload content -- we've verified scanning finds
        // networks and sends are attempted/delivered at the radio level, but
        // never actually inspected what's inside the message being sent.
        Serial.printf("[MESH] TX: %s\n", line.c_str());
        jcmkSendText(line);
        jcmkSentCount++;
      }
    } else {
      // Piglet-to-Piglet: buffer results instead of sending immediately.
      // Scanning keeps running at full pace, untouched by other nodes'
      // schedules -- jcmkPigletSlotTick() flushes this buffer only during
      // this node's own deterministic transmit slot.
      for (int i = 0; i < n; i++) {
        String bssid = WiFi.BSSIDstr(i);
        String ssid  = WiFi.SSID(i);
        String auth  = meshAuthModeToString(WiFi.encryptionType(i));
        int    ch    = WiFi.channel(i);
        int    rssi  = WiFi.RSSI(i);
        wardriveGoPushAp(bssid, ssid, auth, ch, rssi);
        String line  = bssid + "," + ssid + "," + auth + ","
                     + String(ch) + "," + String(rssi) + ",W";
        jcmkPendingPush(line);
      }
    }
  }
  WiFi.scanDelete();
  nodeScanActive = false;
  nodeScanChOffset++;
}

// ================================================================
//  Piglet-to-Piglet deterministic transmit-slot flush (see MeshNode plan).
//  Only ever active when jcmkCoreIsPiglet is true; otherwise a no-op, so
//  non-Piglet pairings are completely unaffected. Never interrupts an
//  in-flight scan (only runs in the natural gap between channel scans),
//  so a node's own scanning loop is never slowed down by this.
// ================================================================
static void jcmkPigletSlotTick() {
  if (!jcmkCoreIsPiglet) return;
  if (nodeScanActive) return;      // never interrupt a live async scan
  if (jcmkPendingEmpty()) return;

  uint32_t slotCount = (jcmkSlotCount > 0) ? jcmkSlotCount : 1;
  uint32_t cycleMs   = JCMK_SLOT_MS * slotCount;
  uint32_t cyclePos  = (millis() - jcmkCycleEpochMs) % cycleMs;
  uint32_t slotStart = (uint32_t)jcmkSlotIndex * JCMK_SLOT_MS;

  if (cyclePos < slotStart || cyclePos >= slotStart + JCMK_SLOT_MS) return;  // not my turn yet

  jcmkSetChannel(JCMK_ESPNOW_CH);
  String line;
  uint8_t sent = 0;
  while (sent < 16 && jcmkPendingPop(line)) {
    jcmkSendText(line);
    jcmkSentCount++;
    sent++;
  }
}

// ================================================================
//  Lifecycle
// ================================================================
void enterNodeMode() {
  Serial.println("[MESH] Entering node mode");
  meshNodeActive        = false;
  jcmkHaveCore          = false;
  jcmkCoreFoundPending  = false;
  jcmkCoreIsPigletPending = false;
  jcmkCoreIsPiglet      = false;
  jcmkSlotIndex         = 0;
  jcmkSlotCount         = 1;
  jcmkCycleEpochMs      = 0;
  jcmkCoreLastSeenMs    = 0;
  jcmkPendingHead       = 0;
  jcmkPendingTail       = 0;
  jcmkNetworksFound     = 0;
  jcmkSentCount         = 0;
  jcmkSendFailCount     = 0;
  jcmkHbCounter         = 0;
  jcmkTextCounter       = 0;
  jcmkLastHbMs          = 0;
  jcmkLastReqMs         = 0;
  jcmkReqInterval       = JCMK_REQ_INIT_MS;
  jcmkStartIdx          = 0;
  jcmkEndIdx            = JCMK_NUM_CHANNELS - 1;
  jcmkAssignVer         = 0;
  nodeScanActive        = false;
  nodeScanChOffset      = 0;
  nodeScanAdminWin      = false;
  biscuitChannelCount   = 0;

  // Mesh mode owns the WiFi stack — prevent stopAPIfAllowed() from firing
  // WiFi.disconnect(true,true) after esp_now_init() would kill the ESP-Now driver.
  apWindowActive = false;

  // Full WiFi deinit → reinit: clears previous AP channel/state from driver.
  // WiFi.disconnect(wifioff=true) only calls esp_wifi_stop() — it leaves the
  // driver in a tainted state that breaks ESP-Now receive on the next start.
  // WiFi.mode(WIFI_OFF) calls esp_wifi_deinit() for a true clean slate.
  WiFi.softAPdisconnect(true);
  WiFi.setAutoReconnect(false);
  WiFi.persistent(false);
  WiFi.mode(WIFI_OFF);  // full deinit — clears previous AP channel state
  delay(200);
  WiFi.mode(WIFI_STA);  // clean reinit
  delay(200);

  // Init ESP-Now FIRST, then lock the home channel.
  // Calling setChannel before esp_now_init() risks the driver
  // resetting the channel back during its own initialisation.
  esp_err_t err = esp_now_init();
  if (err != ESP_OK) {
    Serial.printf("[MESH] esp_now_init failed: %d\n", (int)err);
    return;
  }
  esp_now_register_recv_cb(jcmkOnRecv);
  esp_now_register_send_cb(jcmkOnSent);

  // Lock radio to JCMK ESP-Now home channel AFTER init (matches JCMK pattern)
  delay(50);
  jcmkSetChannel(JCMK_ESPNOW_CH);
  // Verify the channel actually stuck
  { uint8_t pri; wifi_second_chan_t sec; esp_wifi_get_channel(&pri, &sec);
    Serial.printf("[MESH] Channel after set: %d (target=%d)%s\n", pri, JCMK_ESPNOW_CH,
      (pri == JCMK_ESPNOW_CH) ? " OK" : " *** MISMATCH — trying again");
    if (pri != JCMK_ESPNOW_CH) { delay(50); jcmkSetChannel(JCMK_ESPNOW_CH);
      esp_wifi_get_channel(&pri, &sec);
      Serial.printf("[MESH] Channel after retry: %d\n", pri); }
  }

  jcmkAddPeer(JCMK_BCAST);

  // Random stagger before first scan (JCMK begin() pattern: delay(random(100,5000))).
  // Prevents multiple nodes starting cycles in sync and flooding simultaneously.
  delay(random(200, 3000));

  meshNodeActive = true;
  Serial.println("[MESH] ESP-Now ready — searching for Core on ch 6...");

  // Wardrive Go BLE bridge -- only ever active while on this page, gated on
  // the config toggle. Relays this node's own finds (see nodeDoScanTick()).
  if (cfg.wardriveGoBridge) wardriveGoBleBegin(false);
}

void exitNodeMode() {
  Serial.println("[MESH] Exiting node mode");
  if (meshNodeActive) esp_now_deinit();
  meshNodeActive = false;
  jcmkHaveCore   = false;

  wardriveGoBleStop();  // no-op if it was never begun

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
  meshChOverrideLoad();

  // Mesh mode owns the WiFi stack — prevent stopAPIfAllowed() from firing
  // WiFi.disconnect(true,true) after esp_now_init() would kill the ESP-Now driver.
  apWindowActive = false;

  // Full WiFi deinit → reinit: clears previous AP channel/state from driver.
  WiFi.softAPdisconnect(true);
  WiFi.setAutoReconnect(false);
  WiFi.persistent(false);
  WiFi.mode(WIFI_OFF);  // full deinit — clears previous AP channel state
  delay(200);
  WiFi.mode(WIFI_STA);  // clean reinit
  delay(200);

  esp_err_t err = esp_now_init();
  if (err != ESP_OK) {
    Serial.printf("[CORE] esp_now_init failed: %d\n", (int)err);
    return;
  }
  esp_now_register_recv_cb(jcmkOnRecv);
  esp_now_register_send_cb(jcmkOnSent);
  delay(50);
  jcmkSetChannel(JCMK_ESPNOW_CH);
  // Verify the channel actually stuck
  { uint8_t pri; wifi_second_chan_t sec; esp_wifi_get_channel(&pri, &sec);
    Serial.printf("[CORE] Channel after set: %d (target=%d)%s\n", pri, JCMK_ESPNOW_CH,
      (pri == JCMK_ESPNOW_CH) ? " OK" : " *** MISMATCH — trying again");
    if (pri != JCMK_ESPNOW_CH) { delay(50); jcmkSetChannel(JCMK_ESPNOW_CH);
      esp_wifi_get_channel(&pri, &sec);
      Serial.printf("[CORE] Channel after retry: %d\n", pri); }
  }
  jcmkAddPeer(JCMK_BCAST);

  meshCoreActive = true;
  Serial.println("[CORE] Ready — listening for nodes on ch 6");

  // Wardrive Go BLE bridge -- only ever active while on this page, gated on
  // the config toggle. Relays every node-forwarded AP (coreParseAndLogText())
  // plus this Core's own GPS.
  if (cfg.wardriveGoBridge) wardriveGoBleBegin(true);
}

void exitCoreMode() {
  Serial.println("[CORE] Exiting Core mode");
  if (meshCoreActive) esp_now_deinit();
  meshCoreActive = false;
  coreNodeCount  = 0;

  wardriveGoBleStop();  // no-op if it was never begun

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

  wardriveGoBleTick();  // no-op unless the bridge was begun (cfg.wardriveGoBridge)

  // Relay this Core's own GPS fix ~1 Hz, independent of node traffic.
  {
    static uint32_t wgGpsMs = 0;
    if (gpsHasFix && now - wgGpsMs >= 1000) {
      wgGpsMs = now;
      int sats = gps.satellites.isValid() ? (int)gps.satellites.value() : 0;
      wardriveGoPushGps(gps.location.lat(), gps.location.lng(),
                         gps.altitude.isValid() ? gps.altitude.meters() : 0.0, sats);
    }
  }

  // 1. Process pending CORE_REQUEST queue (new node registrations)
  while (coreReqHead != coreReqTail) {
    uint8_t i = coreReqHead;
    coreReqHead = (coreReqHead + 1) % CORE_REQ_QUEUE;
    coreFindOrAddNode(coreReqBuf[i].mac, coreReqBuf[i].isBiscuit, coreReqBuf[i].isPiglet, coreReqBuf[i].boardType, coreReqBuf[i].version);
    coreReassignChannels();
  }

  // 2. Process pending TEXT records
  while (coreTextHead != coreTextTail) {
    uint8_t i = coreTextHead;
    coreTextHead = (coreTextHead + 1) % CORE_TEXT_QUEUE;
    coreParseAndLogText(coreTextBuf[i].line);
  }

  // Fold this window's per-channel hit counts into the yield EWMA (no-op
  // between 4s windows) -- see core5gStat / coreSendBiscuitRoleAndConfig.
  coreYieldUpdate(now);

  // Drive any in-flight push of firmware to a connected node (no-op unless
  // coreOtaStart() has armed one) -- see the Core-side OTA engine above.
  coreOtaTick();

  // Queue newly-mismatched nodes for the on-device update prompt / headless
  // auto-push, then advance the prompt countdown and step the confirmed
  // queue through coreOtaStart() one node at a time. See the "Update Nodes
  // through Core?" block above -- both are no-ops when nothing is pending.
  coreUpdateDetect();
  coreUpdateProcessTick();

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

  wardriveGoBleTick();  // no-op unless the bridge was begun (cfg.wardriveGoBridge)
  wardriveGoSetGpsPresent(gps.charsProcessed() > 0);

  // Relay this node's own GPS fix ~1 Hz, independent of ESP-NOW pairing state.
  {
    static uint32_t wgGpsMs = 0;
    if (gpsHasFix && now - wgGpsMs >= 1000) {
      wgGpsMs = now;
      int sats = gps.satellites.isValid() ? (int)gps.satellites.value() : 0;
      wardriveGoPushGps(gps.location.lat(), gps.location.lng(),
                         gps.altitude.isValid() ? gps.altitude.meters() : 0.0, sats);
    }
  }

  // Consume pending core-found event from the ESP-Now callback
  if (jcmkCoreFoundPending) {
    jcmkCoreFoundPending = false;
    memcpy(jcmkCoreMac, jcmkCoreMacPending, 6);
    jcmkHaveCore       = true;
    jcmkCoreIsPiglet   = jcmkCoreIsPigletPending;
    jcmkCoreLastSeenMs = now;
    jcmkCycleEpochMs   = now;
    jcmkReqInterval    = JCMK_REQ_INIT_MS;
    jcmkAddPeer(jcmkCoreMac);
    if (biscuitPlainReplyPending) {
      // Identify ourselves to the Biscuit Core ("Piglet:<version>") and give
      // it a short ch-6 admin window to send its role/channel-config handshake.
      jcmkSetChannel(JCMK_ESPNOW_CH);
      biscuitSendControl(7, 0, "Piglet:" FIRMWARE_VERSION);
      nodeScanAdminWin = true;
      nodeScanAdminMs = now;
    }
    Serial.printf("[MESH] Core: %02X:%02X:%02X:%02X:%02X:%02X%s\n",
      jcmkCoreMac[0], jcmkCoreMac[1], jcmkCoreMac[2],
      jcmkCoreMac[3], jcmkCoreMac[4], jcmkCoreMac[5],
      jcmkCoreIsPiglet ? " (Piglet — slot scheduling active)" : "");
  }

  // Core-timeout detection: if the Core goes silent, stop scanning/sending
  // into the void and return to actively searching for a (possibly new) Core.
  if (jcmkHaveCore && (now - jcmkCoreLastSeenMs >= JCMK_CORE_TIMEOUT_MS)) {
    Serial.println("[MESH] Core timed out — returning to search");
    if (esp_now_is_peer_exist(jcmkCoreMac)) esp_now_del_peer(jcmkCoreMac);
    jcmkHaveCore     = false;
    jcmkCoreIsPiglet = false;
    jcmkAssignVer    = 0;
    jcmkStartIdx     = 0;
    jcmkEndIdx       = JCMK_NUM_CHANNELS - 1;
    jcmkSlotIndex    = 0;
    jcmkSlotCount    = 1;
    jcmkPendingHead  = jcmkPendingTail = 0;  // drop any buffered-but-unsent results
    nodeScanActive   = false;
    nodeScanAdminWin = false;
    biscuitChannelCount = 0;
    jcmkReqInterval  = JCMK_REQ_INIT_MS;
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

  // Per-channel async scan — runs continuously while connected to Core, OR
  // while the Wardrive Go bridge is enabled even with no Core at all, so a
  // solo Node (no mesh coordinator in range) still scans and feeds the phone
  // app instead of sitting idle on "Searching..." forever. The CORE_REQUEST
  // backoff above keeps announcing in the background either way, so a real
  // coordinator can still be picked up later without needing a restart.
  // jcmkPigletSlotTick() only ever acts for confirmed-Piglet pairings, and
  // only in the gap between scans, so it never affects scan timing itself.
  // Drive any in-flight firmware push from a Piglet/TDongleC5_Piglet Core
  // (no-op unless one is pending/active) -- see the Node-side OTA receiver
  // above. Scanning/sending are paused for the duration (nodeOtaStart()
  // parks the radio on the home channel), same as warhog's WarHogNode.
  nodeOtaTick();

  if ((jcmkHaveCore || cfg.wardriveGoBridge) && !nodeOtaActive) {
    jcmkPigletSlotTick();
    nodeDoScanTick();
  }
}

// Short label for the "WG: ..." indicator that replaces the Hold hint when
// cfg.wardriveGoBridge is on. Only three states are shown: NimBLE doesn't
// expose a separate "streaming" state since subscribe implies streaming.
static const char* wgStatusLabel() {
  if (!wardriveGoBridgeRunning())   return "WG:off";
  if (wardriveGoBridgeConnected())  return "WG:on";
  return "WG:adv";
}

// Scrolling window into a longer string, for display space too small to show it all
// at once (e.g. a multi-channel Wardrive Go `-ch` selection). Advances by one
// character roughly every redraw (the Mesh page redraws at a ~500ms cadence), with a
// gap before it repeats so it reads as a continuous loop rather than jumping.
static const char* scrollText(const char* full, uint8_t windowChars) {
  static char out[24];
  size_t flen = strlen(full);
  if (windowChars >= sizeof(out)) windowChars = sizeof(out) - 1;
  if (flen <= windowChars) {
    strncpy(out, full, sizeof(out) - 1); out[sizeof(out) - 1] = '\0';
    return out;
  }
  char padded[160];
  snprintf(padded, sizeof(padded), "%s   ", full);
  size_t plen = strlen(padded);
  uint32_t pos = (millis() / 500) % plen;
  for (uint8_t i = 0; i < windowChars; i++) out[i] = padded[(pos + i) % plen];
  out[windowChars] = '\0';
  return out;
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
    CoreUpdatePromptStatus ups = coreUpdatePromptStatus();
    if (ups.promptActive || ups.pushActive) {
      // A pending node-firmware mismatch takes over the whole page -- this
      // replaces the normal Core status layout rather than overlaying it,
      // since both need the full 128x64 to stay readable. The mesh-page
      // button handler (Piglet.ino pollButton()) consumes a single press as
      // confirm while ups.promptActive, instead of cycling to the next page.
      display.setTextSize(1);
      display.setCursor(0, 0);
      display.print(ups.pushActive ? "Updating Node" : "Update Node?");
      display.drawFastHLine(0, 11, 128, SSD1306_WHITE);

      uint8_t idx = ups.pushActive ? ups.pushNodeIdx : pendingUpdateNodes[0];
      display.setCursor(0, 16);
      if (idx < CORE_MAX_NODES && coreNodes[idx].active) {
        uint8_t* m = coreNodes[idx].mac;
        char buf[22];
        snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
                 m[0], m[1], m[2], m[3], m[4], m[5]);
        display.print(buf);
      } else {
        display.print("--:--:--:--:--:--");
      }

      if (!ups.pushActive && ups.pendingCount > 1) {
        // Accurate only before a push starts -- pendingCount is the whole
        // batch, not a "remaining" count, so it's not shown mid-push.
        display.setCursor(0, 26);
        char qb[22];
        snprintf(qb, sizeof(qb), "+%d more queued", ups.pendingCount - 1);
        display.print(qb);
      }

      if (ups.pushActive) {
        CoreOtaStatus ota = coreOtaStatus();
        display.setCursor(0, 38);
        display.print("Updating...");
        oledProgressBar(0, 48, 128, 10,
                         ota.totalBytes ? (float)ota.sentBytes / (float)ota.totalBytes : 0.0f);
      } else {
        display.setCursor(0, 38);
        uint32_t secLeft = (ups.promptRemainingMs + 999) / 1000;
        char cb[24];
        snprintf(cb, sizeof(cb), "Press=Yes  (%lus)", (unsigned long)secLeft);
        display.print(cb);
        display.setCursor(0, 53);
        display.print("Ignore = Skip");
      }

      display.display();
      return;
    }

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

    // Row 5 (y=53): GPS + hint (or WG status if the bridge is enabled)
    display.setCursor(0, 53);
    display.print(gpsHasFix ? "GPS:FIX" : "GPS:---");
    display.setCursor(62, 53);
    display.print(cfg.wardriveGoBridge ? wgStatusLabel() : "Hold=Node");

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

    // Row 3 (y=35): channel assignment + ENOW ch (or the Wardrive Go app's own
    // channel selection, scrolled, if it has narrowed one)
    display.setCursor(0, 35);
    display.print("Ch:");
    {
      uint8_t wgCh[48]; uint8_t wgN = 0;
      if (wardriveGoChannelOverride(wgCh, &wgN)) {
        char list[160]; int len = 0;
        for (uint8_t i = 0; i < wgN && len < (int)sizeof(list) - 5; i++)
          len += snprintf(list + len, sizeof(list) - len, i ? ",%u" : "%u", (unsigned)wgCh[i]);
        display.print(scrollText(list, 9));
      } else if (jcmkAssignVer > 0 && jcmkStartIdx < JCMK_NUM_CHANNELS
                                    && jcmkEndIdx   < JCMK_NUM_CHANNELS) {
        display.print(JCMK_CHANNELS[jcmkStartIdx]);
        display.print("-");
        display.print(JCMK_CHANNELS[jcmkEndIdx]);
      } else {
        display.print("all");
      }
    }
    display.setCursor(72, 35);
    display.print("ENOW:");
    display.print(JCMK_ESPNOW_CH);

    // Row 4 (y=44): networks found + Core hint (or WG status if enabled)
    display.setCursor(0, 44);
    display.print("Found:");
    display.print(jcmkNetworksFound);
    display.setCursor(70, 44);
    display.print(cfg.wardriveGoBridge ? wgStatusLabel() : "Hold=Core");

    // Row 5 (y=53): records sent + radio-level send failures.
    // "Sent" only means esp_now_send() was called; "Fail" reflects the
    // actual delivery-status callback, so a healthy Sent count with a
    // climbing Fail count means the radio itself is dropping the frames.
    display.setCursor(0, 53);
    display.print("Sent:");
    display.print(jcmkSentCount);
    display.setCursor(70, 53);
    display.print("Fail:");
    display.print(jcmkSendFailCount);
  }

  display.display();
}
