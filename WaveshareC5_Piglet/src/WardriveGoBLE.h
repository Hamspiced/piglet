#pragma once
// ================================================================
//  Wardrive Go / WarBLE Go  (RocketGod "iosESP") BLE bridge
//  In its own header (not inline in the .ino) to avoid the Arduino ctags
//  auto-prototype bug -- same reason WebUI_HTML.h is split out.
// ================================================================
// Makes this T-Dongle C5 look like a RocketGod "iosESP" wardrive bridge so
// the phone Wardrive Go / WarBLE Go app pairs with it and plots the APs this
// device (and, in mesh mode, its whole node cluster) hears, live on the map.
//
// Protocol: iosESP v1.5.
//   GATT service  47686F73-7445-5350-4272-696467655376  (primary advertisement)
//     RX   0147686F-7374-4553-5042-726964675852  write / write-NR  (phone -> board cmds)
//     TX   0247686F-7374-4553-5042-726964675854  notify            (board -> phone)
//     CTRL 0347686F-7374-4553-5042-72694C525443  write / write-NR  (also accepts cmds)
//     OTA  0447686F-7374-4553-5042-72694F544144  write-NR          (firmware image; stubbed)
//   Advertised name: "iosESP-XXXX" (last two octets of the BLE address) --
//   this is a protocol identifier the app's scanner keys off of, NOT a
//   branding string. Do not rename it.
//
// Scope: only ever active on the Mesh Node/Core page, gated on
// cfg.wardriveGoBridge -- see enterNodeMode()/exitNodeMode()/enterCoreMode()/
// exitCoreMode() below in the .ino.

#include <NimBLEDevice.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

static const char* WG_SERVICE_UUID = "47686F73-7445-5350-4272-696467655376";
#define WG_CH_RX    "0147686F-7374-4553-5042-726964675852"   // write / write-NR
#define WG_CH_TX    "0247686F-7374-4553-5042-726964675854"   // notify
#define WG_CH_CTRL  "0347686F-7374-4553-5042-72694C525443"   // write / write-NR
#define WG_CH_OTA   "0447686F-7374-4553-5042-72694F544144"   // write-NR (stubbed)

// Reported in WD:CAPS/`info` as this board's firmware version -- tracks Piglet's own
// FIRMWARE_VERSION (e.g. "v2.82"), reformatted as 1<major>.<minor>.00 (e.g. "12.82.00")
// rather than a fixed/official WardriveESP release number. The leading "1" digit is
// deliberate: it keeps the reported version numerically well above the real WardriveESP
// release line (currently 1.x), so the app's "outdated firmware" nag -- which compares
// this number-by-number -- never triggers, while the rest still tracks Piglet's actual
// version for support/debugging.
static const char* wgFwVersion() {
  static char buf[16] = "";
  if (buf[0]) return buf;
  const char* v = FIRMWARE_VERSION;
  if (*v == 'v' || *v == 'V') v++;
  int major = atoi(v);
  int minor = 0;
  const char* dot = strchr(v, '.');
  if (dot) minor = atoi(dot + 1);
  snprintf(buf, sizeof(buf), "1%d.%02d.00", major, minor);
  return buf;
}
#ifdef CONFIG_IDF_TARGET
#define WG_CHIP CONFIG_IDF_TARGET      // "esp32c5", ...
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

// Channel plan: which channels we report in WD:CAPS/WD:PLAN and actually forward AP sightings for.
// This board is always a dual-band ESP32-C5, so the supported list is the full JCMK_CHANNELS table
// (already in scope -- this header is #included after it in the .ino). wgPlan/wgPlanCount is the active
// subset, which the app can narrow with `-ch` on `wdstream start` or the `ch` command, same as the real
// WardriveESP protocol.
static uint8_t  wgSupportedCount = JCMK_NUM_CHANNELS;
static uint8_t  wgPlan[48];
static uint8_t  wgPlanCount      = 0;

static int wgFormatChannelList(char* buf, size_t cap, const uint8_t* ch, uint8_t n) {
  int len = 0;
  for (uint8_t i = 0; i < n && len < (int)cap - 5; i++)
    len += snprintf(buf + len, cap - len, i ? ",%u" : "%u", (unsigned)ch[i]);
  return len;
}

static bool wgChannelSupported(uint8_t ch) {
  for (uint8_t i = 0; i < wgSupportedCount; i++) if (JCMK_CHANNELS[i] == ch) return true;
  return false;
}

static bool wgChannelInPlan(int ch) {
  for (uint8_t i = 0; i < wgPlanCount; i++) if (wgPlan[i] == (uint8_t)ch) return true;
  return false;
}

// Reset the active plan to every channel this hardware supports.
static void wgPlanResetToAll() {
  memcpy(wgPlan, JCMK_CHANNELS, wgSupportedCount);
  wgPlanCount = wgSupportedCount;
}

// `-ch 1,6,11,36` / `ch 1,6,11` / `ch all`: narrow (or reset) the channel plan to what the app asked for.
// Unsupported/invalid channels are dropped; an empty result falls back to every supported channel.
static void wgSetChannelPlan(const char* s) {
  while (*s == ' ') s++;
  if (!*s || !strcmp(s, "all")) { wgPlanResetToAll(); return; }
  uint8_t tmp[48]; uint8_t n = 0;
  while (*s && n < sizeof(tmp)) {
    while (*s == ',' || *s == ' ') s++;
    if (*s < '0' || *s > '9') break;
    char* end;
    long v = strtol(s, &end, 10);
    s = end;
    if (v > 0 && v <= 255 && wgChannelSupported((uint8_t)v)) {
      bool dup = false;
      for (uint8_t i = 0; i < n; i++) if (tmp[i] == (uint8_t)v) dup = true;
      if (!dup) tmp[n++] = (uint8_t)v;
    }
  }
  if (n == 0) { wgPlanResetToAll(); return; }
  memcpy(wgPlan, tmp, n);
  wgPlanCount = n;
}

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
  char chanBuf[200];
  wgFormatChannelList(chanBuf, sizeof(chanBuf), JCMK_CHANNELS, wgSupportedCount);
  wgEmit("WD:CAPS fw=%s chip=%s bands=2g,5g ota=0 bt=0 gps=%d cap=0 chans=%s",
         wgFwVersion(), WG_CHIP, wgGpsPresent ? 1 : 0, chanBuf);
}

static void wardriveGoSetGpsPresent(bool present) {
  if (present == wgGpsPresent) return;
  wgGpsPresent = present;
  // CAPS goes out exactly once per stream (wgSendHandshake below); if the
  // answer changes under us, re-send CAPS alone -- the app accepts it at any
  // time, unlike WD:BEGIN, which must never be replayed mid-stream.
  if (wgActive && wgNotifyOn) wgSendCaps();
}

// The full WD: scan-start handshake, sent exactly once per stream start.
static void wgSendHandshake() {
  wgSendCaps();
  wgEmit("WD:MODE mode=wifi");
  wgEmit("WD:BEGIN type=wifi interval=0 channel=auto");
  char planBuf[200];
  wgFormatChannelList(planBuf, sizeof(planBuf), wgPlan, wgPlanCount);
  wgEmit("WD:PLAN chans=%s", planBuf);
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

// Map this firmware's plain auth tokens (authModeToString()) to a WD auth
// token. Called BEFORE any Biscuit bracket-notation rewrite, so these are
// always the plain forms, never "[WPA2_PSK]" etc.
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
  wgEmit("iosESP %s chip=%s name=%s uptime=%lus", wgFwVersion(), WG_CHIP, wgName,
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
  else if (!strcmp(word, "ch")) {
    if (*rest) wgSetChannelPlan(rest);
    char planBuf[200];
    wgFormatChannelList(planBuf, sizeof(planBuf), wgPlan, wgPlanCount);
    wgEmit("WD:PLAN chans=%s", planBuf);
  }
  else if (!strcmp(word, "reboot")) { wgEmit("rebooting"); wgRebootReq = true; }
  else if (!strcmp(word, "stop"))   wgStopStreaming();
  else if (!strcmp(word, "start"))  wgStartStreaming();
  else if (!strcmp(word, "bt"))     wgEmit("mode Wi-Fi  \xc2\xb7  bt unsupported");
  else if (!strcmp(word, "cap"))    wgEmit("mode Wi-Fi  \xc2\xb7  cap unsupported");
  else if (!strcmp(word, "wdstream")) {
    if (strstr(rest, "stop")) {
      wgStopStreaming();
    } else {
      const char* chFlag = strstr(rest, "-ch");
      if (chFlag) wgSetChannelPlan(chFlag + 3);
      wgStartStreaming();   // -wifi: we only ever scan Wi-Fi, already the only mode
    }
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
    // Match the real WardriveESP firmware's connection parameters for better throughput: a 30-50 ms
    // interval (24-40 x 1.25 ms units) with a 5 s supervision timeout (500 x 10 ms units), no slave latency.
    s->updateConnParams(i.getConnHandle(), 24, 40, 0, 500);
  }
  void onDisconnect(NimBLEServer* s, NimBLEConnInfo& i, int reason) override {
    if (wgConnected) wgConnected--;
    Serial.printf("[WARDRIVEGO] central disconnected (reason %d)\n", reason);
    wgNotifyOn = false; wgStreaming = false;
    wgPlanResetToAll();   // the channel list resets to every supported channel on disconnect
    // Only re-arm if the bridge is still meant to be running -- this disconnect may be
    // the tail end of a wardriveGoBleSuspend()/wardriveGoBleStop() initiated disconnect,
    // in which case re-advertising (or touching advertising after deinit) would undo the
    // suspend or use a freed stack.
    if (wgActive) NimBLEDevice::startAdvertising();
  }
};

static WgCommandCB wgCommandCb;
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
// `isCore` is accepted for API symmetry with the main Piglet firmware, which uses it
// to avoid restricting a Core's relayed channel list to its own radio's band support
// (a Core never scans itself). This board is always a dual-band C5 regardless of role,
// so both roles already use the full channel list and the parameter is a no-op here.
// True once the NimBLE stack, GATT server/service/characteristics, and advertising
// payloads have been built for this boot. Stays true across wardriveGoBleSuspend() so a
// later wardriveGoBleBegin() (double-press re-enable, or re-entering the Mesh page) only
// has to restart advertising -- cheap and fast -- instead of re-running the full
// NimBLEDevice::init()/createServer()/createService() sequence. Only cleared by the full
// teardown in wardriveGoBleStop() (page exit).
static bool wgStackReady = false;

static void wardriveGoBleBegin(bool isCore) {
  (void)isCore;
  if (wgActive) return;

  wgPlanResetToAll();

  if (!wgStackReady) {
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
       ->setCallbacks(&wgCommandCb);
    chTx = svc->createCharacteristic(WG_CH_TX, NIMBLE_PROPERTY::NOTIFY);
    chTx->setCallbacks(&wgTxCb);
    svc->createCharacteristic(WG_CH_CTRL, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR)
       ->setCallbacks(&wgCommandCb);
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

    wgStackReady = true;
  }

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

// Lightweight disable for use while the Mesh Node/Core page stays active (e.g. the
// double-press toggle): stops advertising and drops any linked phone, but leaves the
// NimBLE stack/GATT server initialized so a later wardriveGoBleBegin() is an instant
// restart instead of a full re-init. Prefer this over wardriveGoBleStop() for anything
// that must not risk blocking -- a full NimBLEDevice::deinit() while a phone is
// connected can stall waiting on the BT controller, especially with Wi-Fi/ESP-NOW
// busy on the shared radio.
static void wardriveGoBleSuspend() {
  if (!wgActive) return;
  wgActive = false;
  if (wgStackReady) {
    NimBLEServer* server = NimBLEDevice::getServer();
    if (server) {
      for (uint16_t h : server->getPeerDevices()) server->disconnect(h);
    }
    NimBLEDevice::stopAdvertising();
  }
  wgNotifyOn = false;
  wgStreaming = false;
  wgConnected = 0;
  wgCmdHead = wgCmdTail = 0;
  wgTxHead = wgTxTail = 0;
  Serial.println("[WARDRIVEGO] Suspended");
}

// Full teardown -- only call when actually leaving the Mesh Node/Core page.
static void wardriveGoBleStop() {
  if (!wgStackReady) { wgActive = false; return; }
  wgActive = false;
  // Drop any linked phone before tearing the stack down -- NimBLEDevice::deinit() while
  // a central is still connected can otherwise block the caller waiting on the BT
  // controller, which can take a long time (or stall outright) with Wi-Fi/ESP-NOW also
  // busy on the shared radio.
  NimBLEServer* server = NimBLEDevice::getServer();
  if (server) {
    for (uint16_t h : server->getPeerDevices()) server->disconnect(h);
  }
  NimBLEDevice::stopAdvertising();
  // Full deinit frees the BT controller RAM for the WiFi/ESP-Now path --
  // this firmware has no other concurrent BLE consumer, so this is safe.
  NimBLEDevice::deinit(true);
  chTx = nullptr;
  wgNotifyOn = false;
  wgStreaming = false;
  wgConnected = 0;
  wgCmdHead = wgCmdTail = 0;
  wgTxHead = wgTxTail = 0;
  wgStackReady = false;
  Serial.println("[WARDRIVEGO] Down");
}

static bool wardriveGoBridgeRunning()   { return wgActive; }
static bool wardriveGoBridgeConnected() { return wgActive && wgConnected > 0; }

// True when the phone has explicitly narrowed the channel plan below every supported
// channel (via `-ch` on `wdstream start` or the `ch` command) -- fills `out` (capacity
// at least 48 bytes) with that channel list and `outCount` with its length. Used by
// Node-mode scanning to actually scan only those channels, not just filter which
// sightings get forwarded to the phone.
static bool wardriveGoChannelOverride(uint8_t* out, uint8_t* outCount) {
  if (!wgActive || wgPlanCount == 0 || wgPlanCount >= wgSupportedCount) return false;
  memcpy(out, wgPlan, wgPlanCount);
  *outCount = wgPlanCount;
  return true;
}

// ================================================================
//  Telemetry push
// ================================================================
static char wgLowerHex(uint8_t v) { return (char)(v < 10 ? '0' + v : 'a' + (v - 10)); }

static void wardriveGoPushAp(const String& bssid, const String& ssid,
                             const String& auth, int channel, int rssi) {
  if (!wgActive || !wgNotifyOn || !wgStreaming) return;
  if (!wgChannelInPlan(channel)) return;   // respect the app's -ch / `ch` channel selection
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

static void wardriveGoPushGps(double lat, double lon, double altMeters, int sats) {
  if (!wgActive || !wgNotifyOn || !wgStreaming) return;
  if (sats < 0) sats = 0;
  wgEmit("WD:GPS ts=%lu lat=%.6f lon=%.6f alt=%.1f fix=1 sats=%d",
         (unsigned long)millis(), lat, lon, altMeters, sats);
}

// ================================================================
//  Main-loop tick
// ================================================================
static void wardriveGoBleTick() {
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
