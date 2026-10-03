#include "SerialSync.h"
#include <SD.h>
#include <base64.h>
#include <esp_mac.h>
#include <esp_log.h>
#include "Globals.h"
#include "SDUtils.h"

static const size_t CMD_MAX = 200;
// 144 raw bytes -> 192 base64 chars; a whole data line (~206 B) must fit the
// USB CDC TX buffer (256 B on the C3/C5/C6 HW CDC) or waitForRoom never succeeds.
static const size_t CHUNK = 144;
static const uint32_t WRITE_WAIT_MS = 10000;  // give up if the host stops reading

static String cmdBuf;

static uint32_t crc32Update(uint32_t crc, const uint8_t* data, size_t len) {
  crc = ~crc;
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
  }
  return ~crc;
}

// Blocks until the CDC TX buffer can take `n` bytes, or times out (host gone).
static bool waitForRoom(size_t n) {
  uint32_t start = millis();
  while (Serial.availableForWrite() < (int)n) {
    if (millis() - start > WRITE_WAIT_MS) return false;
    delay(1);
  }
  return true;
}

static void sendLine(const String& s) {
  waitForRoom(s.length() + 2);
  Serial.println(s);
}

// Wi-Fi station MAC: a stable per-device id for the host's import bookkeeping.
// (ESP.getEfuseMac() is a 64-bit EUI on the C5/C6, so don't unpack that.)
static String macString() {
  uint8_t m[6] = {0};
  esp_read_mac(m, ESP_MAC_WIFI_STA);
  char buf[18];
  snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
  return String(buf);
}

static void cmdHello() {
  // rst/up: why and how long ago this Piglet last booted, so a host can tell
  // a crash or watchdog reset during a transfer (4 panic, 5/6/7 watchdogs)
  // from a plain power-on (1). Kept before sd= for older hosts' parsing.
  sendLine(String("@PH ") + FIRMWARE_VERSION + " " + ESP.getChipModel() + " " +
           macString() + " rst=" + String((int)esp_reset_reason()) +
           " up=" + String(millis() / 1000) + " sd=" + (sdOk ? "1" : "0"));
}

static void cmdList() {
  sendLine("@PL BEGIN");
  int count = 0;
  if (sdOk) {
    const char* dirs[] = {"/logs", "/uploaded"};
    for (const char* dir : dirs) {
      File d = SD.open(dir);
      if (!d || !d.isDirectory()) continue;
      File f = d.openNextFile();
      while (f) {
        if (!f.isDirectory()) {
          String path = normalizeSdPath(dir, f.name());
          if (path.endsWith(".csv") || path.endsWith(".CSV")) {
            bool active = (path == currentCsvPath);
            // 4th field: last-write time (epoch, 0 if unknown). Piglet sets
            // its clock from GPS, so a finished drive carries a real time and
            // hosts can fetch the newest drives first.
            sendLine("@PL F " + path + "\t" + String((uint32_t)f.size()) + "\t" +
                     (active ? "1" : "0") + "\t" + String((uint32_t)f.getLastWrite()));
            count++;
          }
        }
        f.close();
        f = d.openNextFile();
      }
      d.close();
    }
  }
  sendLine("@PL END " + String(count));
}

static void cmdGet(const String& path, uint32_t offset) {
  if (!sdOk) { sendLine("@PG ERR sd-not-ready"); return; }
  if (!isAllowedDataPath(path) || path.indexOf("..") >= 0 ||
      !(path.endsWith(".csv") || path.endsWith(".CSV"))) {
    sendLine("@PG ERR not-allowed"); return;
  }
  if (path == currentCsvPath && logFile) logFile.flush();   // serve what's on card
  File f = SD.open(path, FILE_READ);
  if (!f) { sendLine("@PG ERR not-found"); return; }
  uint32_t size = (uint32_t)f.size();
  if (offset % CHUNK != 0 || offset > size || !f.seek(offset)) {
    f.close(); sendLine("@PG ERR bad-offset"); return;
  }

  // Other tasks (Wi-Fi driver, IDF components) print to this same USB port;
  // a log line landing inside a data line corrupts it, and contending writes
  // can make the CDC driver drop bytes. Mute IDF logging while serving.
  esp_log_level_t prevLevel = esp_log_level_get("*");
  esp_log_level_set("*", ESP_LOG_NONE);

  sendLine("@PG BEGIN " + path + " " + String(size) + " " + String(offset));
  uint8_t buf[CHUNK];
  uint32_t sent = offset, seq = offset / CHUNK;
  bool aborted = false;
  bool readError = false;
  while (true) {
    int n = f.read(buf, CHUNK);
    if (n <= 0) { readError = (sent < size); break; }
    char crc[10];
    snprintf(crc, sizeof(crc), "%08lX", (unsigned long)crc32Update(0, buf, (size_t)n));
    String line = "@PG D " + String(seq++) + " " + crc + " " + base64::encode(buf, (size_t)n);
    if (!waitForRoom(line.length() + 2)) { aborted = true; break; }   // host stopped reading
    Serial.println(line);
    sent += (uint32_t)n;
    if ((seq & 15) == 0) delay(1);    // let the idle task / WDT breathe
  }
  f.close();
  if (readError)     sendLine("@PG ERR read-error " + String(sent));   // bad SD sector: retrying won't help
  else if (!aborted) sendLine("@PG END " + path + " " + String(size) + " " + String(sent));
  esp_log_level_set("*", prevLevel);
}

static void handleCommand(String cmd) {
  cmd.trim();
  if (!cmd.startsWith("@PIGLET ")) return;      // not for us (stray input)
  String body = cmd.substring(8);
  body.trim();
  if (body == "HELLO")              cmdHello();
  else if (body == "LIST")          cmdList();
  else if (body.startsWith("GET ")) {
    // GET <path> [offset]  (offset: resume point, a multiple of the chunk size)
    String rest = body.substring(4); rest.trim();
    int sp = rest.lastIndexOf(' ');
    uint32_t off = 0;
    if (sp > 0 && rest.substring(sp + 1).length() && isDigit(rest.charAt(sp + 1))) {
      off = (uint32_t)rest.substring(sp + 1).toInt();
      rest = rest.substring(0, sp);
    }
    cmdGet(rest, off);
  }
  else                              sendLine("@PE unknown-command");
}

void serialSyncPoll() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (cmdBuf.length()) { String cmd = cmdBuf; cmdBuf = ""; handleCommand(cmd); }
    } else if (cmdBuf.length() < CMD_MAX) {
      cmdBuf += c;
    } else {
      cmdBuf = "";                       // overlong garbage: drop it
    }
  }
}
