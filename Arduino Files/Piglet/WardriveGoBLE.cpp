#include "WardriveGoBLE.h"
#include "Globals.h"      // FIRMWARE_VERSION
#include <NimBLEDevice.h>
#include <stdarg.h>
#include <string.h>

// ================================================================
//  iosESP GATT UUIDs (protocol v1.5)
// ================================================================
static const char* WG_SERVICE_UUID = "47686F73-7445-5350-4272-696467655376";
#define WG_CH_RX    "0147686F-7374-4553-5042-726964675852"   // write / write-NR
#define WG_CH_TX    "0247686F-7374-4553-5042-726964675854"   // notify
#define WG_CH_CTRL  "0347686F-7374-4553-5042-72694C525443"   // write / write-NR
#define WG_CH_OTA   "0447686F-7374-4553-5042-72694F544144"   // write-NR (stubbed)

#define WG_FW_VERSION "piglet-" FIRMWARE_VERSION
#ifdef CONFIG_IDF_TARGET
#define WG_CHIP CONFIG_IDF_TARGET      // "esp32s3", "esp32c5", ...
#else
#define WG_CHIP "esp32"
#endif

// Frame header (little-endian): GB ver type status flags id[4] len[2]
#define FR_MAGIC_0 0x47
#define FR_MAGIC_1 0x42
#define FR_VERSION 0x01
#define FR_CMD     0x01
#define FR_ACK     0x02
#define FR_DATA    0x03
#define FR_HEADER  12

// ================================================================
//  Module state
// ================================================================
static bool                  wgActive     = false;
static bool                  wgNotifyOn   = false;   // phone subscribed to TX
static bool                  wgStreaming  = false;   // between WD:BEGIN and WD:END
static bool                  wgGpsPresent = false;
static NimBLECharacteristic* chTx         = nullptr;
static char                  wgName[16]   = "iosESP";
static uint32_t              wgReplyId    = 0;        // id stamped on DATA frames
static volatile uint16_t     wgConnected  = 0;

static uint32_t wgApSweep   = 0;
static uint32_t wgSweeps    = 0;
static uint32_t wgLastStat  = 0;
static bool     wgRebootReq = false;

// Channels advertised as our scan plan (2.4 GHz; mesh feeds all bands, but
// the app only needs a plausible plan string for its UI).
static const char* WG_CHANS = "1,2,3,4,5,6,7,8,9,10,11,12,13";

// Inbound command ring (write cb enqueues raw bytes; tick drains/replies).
#define WG_CMD_SLOT_LEN 256
#define WG_CMD_RING     8
struct WgCmdSlot { uint8_t data[WG_CMD_SLOT_LEN]; uint16_t len; };
static WgCmdSlot         wgCmdRing[WG_CMD_RING];
static volatile uint8_t  wgCmdHead = 0;
static volatile uint8_t  wgCmdTail = 0;

// Outbound frame ring (binary DATA/ACK frames; tick notifies). Drop-on-full:
// both apps de-dup by BSSID, so a lossy stream of re-sightings still
// converges on a complete map -- no backlog/buffering needed.
#define WG_TX_SLOT_LEN 200
#define WG_TX_RING     64
#define WG_TX_PER_TICK 48
struct WgTxSlot { uint8_t data[WG_TX_SLOT_LEN]; uint8_t len; };
static WgTxSlot          wgTxRing[WG_TX_RING];
static volatile uint8_t  wgTxHead = 0;
static volatile uint8_t  wgTxTail = 0;

static volatile uint32_t wgEnq = 0, wgDropFull = 0, wgSent = 0;

// ================================================================
//  Frame building / enqueue
// ================================================================
static void wgEnqueueFrame(uint8_t type, uint32_t id, const uint8_t* payload, size_t plen) {
  if (!wgActive || !wgNotifyOn) return;
  if (plen > WG_TX_SLOT_LEN - FR_HEADER) plen = WG_TX_SLOT_LEN - FR_HEADER;
  uint8_t next = (wgTxTail + 1) % WG_TX_RING;
  if (next == wgTxHead) { wgDropFull++; return; }      // ring full -> drop
  uint8_t* f = wgTxRing[wgTxTail].data;
  f[0] = FR_MAGIC_0; f[1] = FR_MAGIC_1; f[2] = FR_VERSION; f[3] = type;
  f[4] = 0; f[5] = 0;
  f[6] = id & 0xff; f[7] = (id >> 8) & 0xff; f[8] = (id >> 16) & 0xff; f[9] = (id >> 24) & 0xff;
  f[10] = plen & 0xff; f[11] = (plen >> 8) & 0xff;
  if (plen) memcpy(f + FR_HEADER, payload, plen);
  wgTxRing[wgTxTail].len = (uint8_t)(FR_HEADER + plen);
  wgTxTail = next;
  wgEnq++;
}

// Emit one WD: text line (newline appended) as a DATA frame stamped with the
// current reply id.
static void wgEmit(const char* fmt, ...) {
  char line[200];
  va_list ap; va_start(ap, fmt);
  int n = vsnprintf(line, sizeof(line) - 1, fmt, ap);
  va_end(ap);
  if (n <= 0) return;
  if (n > (int)sizeof(line) - 2) n = (int)sizeof(line) - 2;
  line[n++] = '\n';
  wgEnqueueFrame(FR_DATA, wgReplyId, (const uint8_t*)line, (size_t)n);
}

// ================================================================
//  WD: output helpers
// ================================================================
static void wgSendCaps() {
  wgEmit("WD:CAPS fw=%s chip=%s bands=2g ota=0 bt=0 gps=%d cap=0 chans=%s",
         WG_FW_VERSION, WG_CHIP, wgGpsPresent ? 1 : 0, WG_CHANS);
}

void wardriveGoSetGpsPresent(bool present) {
  if (present == wgGpsPresent) return;
  wgGpsPresent = present;
  // CAPS goes out exactly once per stream (ieSendHandshake below); if the
  // answer changes under us, re-send CAPS alone -- the app accepts it at any
  // time, unlike WD:BEGIN, which must never be replayed mid-stream.
  if (wgActive && wgNotifyOn) wgSendCaps();
}

// The full WD: scan-start handshake, sent exactly once per stream start.
static void wgSendHandshake() {
  wgSendCaps();
  wgEmit("WD:MODE mode=wifi");
  wgEmit("WD:BEGIN type=wifi interval=0 channel=auto");
  wgEmit("WD:PLAN chans=%s", WG_CHANS);
}

static void wgStartStreaming() {
  // Already live: just re-confirm the mode. Replaying WD:BEGIN makes the app
  // treat it as a stream restart and sit forever on "Starting stream...", so
  // the handshake must be sent exactly once per stream.
  if (wgStreaming) { wgEmit("WD:MODE mode=wifi"); return; }
  wgStreaming = true;
  wgApSweep = 0; wgSweeps = 0; wgLastStat = millis();
  wgSendHandshake();
}

static void wgStopStreaming() {
  if (!wgStreaming) return;
  wgStreaming = false;
  wgEmit("WD:END reason=stop");
}

// Map this firmware's plain auth tokens (meshAuthModeToString()) to a WD
// auth token. Called BEFORE any Biscuit bracket-notation rewrite, so these
// are always the plain forms, never "[WPA2_PSK]" etc.
static const char* wgAuthToken(const String& auth) {
  if (auth == "OPEN")     return "OPEN";
  if (auth == "WEP")      return "WEP";
  if (auth == "WPA")      return "WPA";
  if (auth == "WPA2")     return "WPA2";
  if (auth == "WPA3")     return "WPA3";
  if (auth == "WPAWPA2")  return "WPA2_WPA3";
  if (auth == "WPA2EAP")  return "WPA2";
  if (auth == "WPA2WPA3") return "WPA2_WPA3";
  return "UNKNOWN";
}

// ================================================================
//  Command parsing
// ================================================================
static void wgCmdHelp() {
  wgEmit("commands: wdstream start|stop, start, stop, caps, info, status, ch, reboot");
}
static void wgCmdInfo() {
  wgEmit("iosESP %s chip=%s name=%s uptime=%lus", WG_FW_VERSION, WG_CHIP, wgName,
         (unsigned long)(millis() / 1000));
}
static void wgCmdStatus() {
  wgEmit("WD:STATUS aps=%lu scans=%lu", (unsigned long)wgApSweep, (unsigned long)wgSweeps);
}

static void wgHandleText(const char* text, uint16_t tlen) {
  char cmd[WG_CMD_SLOT_LEN];
  size_t n = tlen < sizeof(cmd) - 1 ? tlen : sizeof(cmd) - 1;
  memcpy(cmd, text, n); cmd[n] = '\0';

  // Trim leading/trailing whitespace + CR/LF.
  char* p = cmd;
  while (*p == ' ' || *p == '\t') p++;
  size_t plen = strlen(p);
  while (plen && (p[plen-1] == ' ' || p[plen-1] == '\r' || p[plen-1] == '\n' || p[plen-1] == '\t'))
    p[--plen] = '\0';
  if (!*p) return;

  // First word + remainder.
  char word[16];
  size_t wlen = strcspn(p, " ");
  if (wlen >= sizeof(word)) wlen = sizeof(word) - 1;
  memcpy(word, p, wlen); word[wlen] = '\0';
  const char* rest = p + strcspn(p, " ");
  while (*rest == ' ') rest++;

  if      (!strcmp(word, "help"))   wgCmdHelp();
  else if (!strcmp(word, "info"))   wgCmdInfo();
  else if (!strcmp(word, "status")) wgCmdStatus();
  else if (!strcmp(word, "caps"))   wgSendCaps();
  else if (!strcmp(word, "ch"))     wgEmit("WD:PLAN chans=%s", WG_CHANS);
  else if (!strcmp(word, "reboot")) { wgEmit("rebooting"); wgRebootReq = true; }
  else if (!strcmp(word, "stop"))   wgStopStreaming();
  else if (!strcmp(word, "start"))  wgStartStreaming();
  else if (!strcmp(word, "bt"))     wgEmit("mode Wi-Fi  \xc2\xb7  bt unsupported");
  else if (!strcmp(word, "cap"))    wgEmit("mode Wi-Fi  \xc2\xb7  cap unsupported");
  else if (!strcmp(word, "wdstream")) {
    if (strstr(rest, "stop")) wgStopStreaming();
    else                      wgStartStreaming();   // -ch/-wifi flags: we scan all, always wifi
  }
  else { wgEmit("unknown command '%s'", word); wgCmdHelp(); }
}

// Unwrap a GB frame if present, else treat the whole write as raw text.
static void wgHandleCommand(const uint8_t* data, uint16_t len) {
  const uint8_t* text = data;
  uint16_t tlen = len;
  if (len >= FR_HEADER && data[0] == FR_MAGIC_0 && data[1] == FR_MAGIC_1) {
    uint16_t fplen = (uint16_t)(data[10] | (data[11] << 8));
    if ((size_t)FR_HEADER + fplen <= len) {
      wgReplyId = (uint32_t)data[6] | ((uint32_t)data[7] << 8) |
                  ((uint32_t)data[8] << 16) | ((uint32_t)data[9] << 24);
      if (data[3] == FR_CMD) {
        text = data + FR_HEADER;
        tlen = fplen;
        wgEnqueueFrame(FR_ACK, wgReplyId, nullptr, 0);   // ack the framed command
      }
    }
  }
  wgHandleText((const char*)text, tlen);
}

// ================================================================
//  GATT callbacks
// ================================================================
class WgCommandCB : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& connInfo) override {
    std::string v = c->getValue();
    if (v.empty()) return;
    uint8_t next = (wgCmdTail + 1) % WG_CMD_RING;
    if (next == wgCmdHead) return;                      // ring full -> drop
    size_t n = v.size(); if (n >= WG_CMD_SLOT_LEN) n = WG_CMD_SLOT_LEN;
    memcpy(wgCmdRing[wgCmdTail].data, v.data(), n);
    wgCmdRing[wgCmdTail].len = (uint16_t)n;
    wgCmdTail = next;
  }
};

// OTA characteristic is advertised for protocol-completeness but not wired
// to a real update path; writes are swallowed so the app never stalls on it.
class WgOtaCB : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& connInfo) override { (void)c; }
};

class WgTxCB : public NimBLECharacteristicCallbacks {
  void onSubscribe(NimBLECharacteristic* c, NimBLEConnInfo& connInfo, uint16_t subValue) override {
    wgNotifyOn = (subValue != 0);
    // The app auto-starts scanning when it subscribes to TX.
    if (wgNotifyOn) wgStartStreaming();
    else            wgStopStreaming();
  }
};

class WgServerCB : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* s, NimBLEConnInfo& i) override {
    wgConnected++;
    Serial.println("[WARDRIVEGO] central connected");
    // Do NOT re-arm advertising while a central is connected. Wardrive Go
    // reads an advertisement from a board it believes it is already linked
    // to as evidence the link is stale ("advertising again - old link is
    // gone"), so it terminates the connection and immediately reconnects --
    // a relink loop every ~15s. Advertising is re-armed in onDisconnect, so
    // the board is findable again the moment the phone really leaves.
    NimBLEDevice::stopAdvertising();
  }
  void onDisconnect(NimBLEServer* s, NimBLEConnInfo& i, int reason) override {
    if (wgConnected) wgConnected--;
    Serial.printf("[WARDRIVEGO] central disconnected (reason %d)\n", reason);
    wgNotifyOn = false; wgStreaming = false;
    NimBLEDevice::startAdvertising();
  }
};

static WgCommandCB ieCommandCb;
static WgOtaCB     wgOtaCb;
static WgTxCB      wgTxCb;
static WgServerCB  wgServerCb;

// Derive "iosESP-XXXX" from the last two octets of the BLE address.
static void wgBuildName() {
  std::string a = NimBLEDevice::getAddress().toString();   // "aa:bb:cc:dd:ee:ff"
  if (a.length() >= 17) {
    snprintf(wgName, sizeof(wgName), "iosESP-%c%c%c%c",
             toupper((unsigned char)a[12]), toupper((unsigned char)a[13]),
             toupper((unsigned char)a[15]), toupper((unsigned char)a[16]));
  } else {
    snprintf(wgName, sizeof(wgName), "iosESP");
  }
}

// ================================================================
//  Lifecycle
// ================================================================
void wardriveGoBleBegin() {
  if (wgActive) return;

  NimBLEDevice::init("iosESP");
  NimBLEDevice::setMTU(247);
  NimBLEDevice::setPower(9);   // +9 dBm

  wgBuildName();
  NimBLEDevice::setDeviceName(wgName);

  NimBLEServer* server = NimBLEDevice::createServer();
  // false => NimBLE must NOT delete this static callback object on deinit.
  server->setCallbacks(&wgServerCb, false);

  NimBLEService* svc = server->createService(WG_SERVICE_UUID);
  svc->createCharacteristic(WG_CH_RX, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR)
     ->setCallbacks(&ieCommandCb);
  chTx = svc->createCharacteristic(WG_CH_TX, NIMBLE_PROPERTY::NOTIFY);
  chTx->setCallbacks(&wgTxCb);
  svc->createCharacteristic(WG_CH_CTRL, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR)
     ->setCallbacks(&ieCommandCb);
  svc->createCharacteristic(WG_CH_OTA, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR)
     ->setCallbacks(&wgOtaCb);
  svc->start();

  // Service UUID MUST be in the primary advertisement -- it's the app's scan
  // filter. The (longer) name goes in the scan response so the 31-byte
  // advertising payload doesn't overflow.
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  NimBLEAdvertisementData advData;
  advData.setFlags(0x06);
  advData.addServiceUUID(NimBLEUUID(WG_SERVICE_UUID));
  adv->setAdvertisementData(advData);

  NimBLEAdvertisementData scanData;
  scanData.setName(wgName);
  adv->setScanResponseData(scanData);
  adv->enableScanResponse(true);

  wgNotifyOn = false;
  wgStreaming = false;
  wgGpsPresent = false;
  wgTxHead = wgTxTail = 0;
  wgCmdHead = wgCmdTail = 0;
  wgConnected = 0;
  wgRebootReq = false;
  wgActive = true;

  NimBLEDevice::startAdvertising();
  Serial.printf("[WARDRIVEGO] Up as \"%s\" (chip=%s)\n", wgName, WG_CHIP);
}

void wardriveGoBleStop() {
  if (!wgActive) return;
  wgActive = false;
  NimBLEDevice::stopAdvertising();
  // Full deinit frees the BT controller RAM for the WiFi/ESP-Now path --
  // Piglet has no other concurrent BLE consumer, so this is always safe.
  NimBLEDevice::deinit(true);
  chTx = nullptr;
  wgNotifyOn = false;
  wgStreaming = false;
  wgConnected = 0;
  wgCmdHead = wgCmdTail = 0;
  wgTxHead = wgTxTail = 0;
  Serial.println("[WARDRIVEGO] Down");
}

bool wardriveGoBridgeRunning()   { return wgActive; }
bool wardriveGoBridgeConnected() { return wgActive && wgConnected > 0; }

// ================================================================
//  Telemetry push
// ================================================================
static char wgLowerHex(uint8_t v) { return (char)(v < 10 ? '0' + v : 'a' + (v - 10)); }

void wardriveGoPushAp(const String& bssid, const String& ssid,
                      const String& auth, int channel, int rssi) {
  if (!wgActive || !wgNotifyOn || !wgStreaming) return;
  if (rssi < -128) rssi = -128;

  // SSID -> lower-case hex; empty => hidden.
  char ssidHex[2 * 33 + 1];
  size_t hi = 0;
  size_t slen = ssid.length();
  for (size_t i = 0; i < slen && hi < sizeof(ssidHex) - 2; i++) {
    uint8_t c = (uint8_t)ssid[i];
    ssidHex[hi++] = wgLowerHex(c >> 4);
    ssidHex[hi++] = wgLowerHex(c & 0x0F);
  }
  ssidHex[hi] = '\0';
  unsigned hidden = (hi == 0) ? 1u : 0u;

  wgEmit("WD:AP ts=%lu bssid=%s ssid_hex=%s rssi=%d ch=%d auth=%s hidden=%u",
         (unsigned long)millis(), bssid.length() ? bssid.c_str() : "00:00:00:00:00:00",
         ssidHex, rssi, channel, wgAuthToken(auth), hidden);
  wgApSweep++;
}

void wardriveGoPushGps(double lat, double lon, double altMeters, int sats) {
  if (!wgActive || !wgNotifyOn || !wgStreaming) return;
  if (sats < 0) sats = 0;
  wgEmit("WD:GPS ts=%lu lat=%.6f lon=%.6f alt=%.1f fix=1 sats=%d",
         (unsigned long)millis(), lat, lon, altMeters, sats);
}

// ================================================================
//  Main-loop tick
// ================================================================
void wardriveGoBleTick() {
  if (!wgActive) return;

  // 1. Handle inbound commands (replies/handshake are enqueued as frames).
  while (wgCmdHead != wgCmdTail) {
    uint8_t i = wgCmdHead;
    wgCmdHead = (wgCmdHead + 1) % WG_CMD_RING;
    wgHandleCommand(wgCmdRing[i].data, wgCmdRing[i].len);
  }

  // 2. Drain outbound frames to the phone, WITH BACKPRESSURE. NimBLE's
  // notify() returns false when the controller is out of buffers, and any
  // notifications past that point are silently dropped -- not queued. A
  // back-to-back burst (the handshake: ACK, CAPS, MODE, BEGIN, PLAN) can
  // overrun the buffers, so advance the ring only on an accepted notify and
  // leave the rest for the next tick.
  if (wgNotifyOn && chTx) {
    uint16_t sentNow = 0;
    while (wgTxHead != wgTxTail && sentNow < WG_TX_PER_TICK) {
      uint8_t i = wgTxHead;
      if (!chTx->notify(wgTxRing[i].data, wgTxRing[i].len)) break;  // buffers full: retry next tick
      wgTxHead = (wgTxHead + 1) % WG_TX_RING;
      sentNow++;
    }
    wgSent += sentNow;
  }

  // 3. Periodic WD:STATUS heartbeat + diagnostic counters (one "sweep" ~ every 5 s).
  if (wgStreaming && wgNotifyOn) {
    uint32_t now = millis();
    if (now - wgLastStat >= 5000) {
      wgLastStat = now;
      wgSweeps++;
      wgEmit("WD:STATUS aps=%lu scans=%lu", (unsigned long)wgApSweep, (unsigned long)wgSweeps);
      wgApSweep = 0;

      static uint32_t dbgMs = 0;
      if (now - dbgMs >= 3000) {
        dbgMs = now;
        uint8_t depth = (uint8_t)((wgTxTail - wgTxHead + WG_TX_RING) % WG_TX_RING);
        Serial.printf("[WARDRIVEGO] tx enq=%lu sent=%lu dropFull=%lu depth=%u\n",
                      (unsigned long)wgEnq, (unsigned long)wgSent,
                      (unsigned long)wgDropFull, depth);
      }
    }
  }

  // 4. Honor an app-requested reboot once the reply has had a chance to flush.
  if (wgRebootReq && wgTxHead == wgTxTail) {
    delay(60);
    ESP.restart();
  }
}
