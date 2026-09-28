#include "SDUtils.h"
#include "Globals.h"

// ---- Path helpers ----

String pathBasename(const String& p) {
  int slash = p.lastIndexOf('/');
  if (slash < 0) return p;
  return p.substring(slash + 1);
}

String normalizeSdPath(const char* dir, const char* nameIn) {
  if (!dir || !nameIn) return "";

  String d(dir);
  String n(nameIn);

  d.trim();
  n.trim();

  if (d.length() == 0 || n.length() == 0) return "";

  // Ensure dir starts with "/"
  if (d[0] != '/') d = "/" + d;

  // Strip trailing "/" from dir
  while (d.endsWith("/")) d.remove(d.length() - 1);

  // Case A: name is already absolute: "/logs/foo.csv" or "/uploaded/foo.csv"
  if (n[0] == '/') {
    // If SD lib already gives full path, just return it
    return n;
  }

  // Case B: name is "logs/foo.csv" (no leading slash)
  // If it starts with the same directory name, convert to absolute.
  // Example: dir="/logs", name="logs/foo.csv" => "/logs/foo.csv"
  String dNoSlash = d;
  if (dNoSlash.startsWith("/")) dNoSlash = dNoSlash.substring(1); // "logs"

  if (n.startsWith(dNoSlash + "/")) {
    return "/" + n;  // make it absolute
  }

  // Case C: name is just "foo.csv"
  // Join dir + "/" + name
  return d + "/" + n;
}

bool isAllowedDataPath(const String& p) {
  return p.startsWith("/logs/") || p.startsWith("/uploaded/");
}

// ---- SD clock negotiation ----
// Tries a descending list of SPI clock speeds (fastest first, capped by
// cfg.sdMaxSpiHz) and returns as soon as one mounts successfully, so the
// card runs as fast as the wiring/card actually supports instead of being
// pinned to a conservative fixed speed.
bool sdBeginBestClock(uint8_t csPin) {
  static const uint32_t kLadder[] = {
    20000000, 16000000, 12000000, 8000000, 4000000, 1000000
  };
  uint32_t capHz = (cfg.sdMaxSpiHz > 0) ? cfg.sdMaxSpiHz : 20000000;

  for (size_t i = 0; i < sizeof(kLadder) / sizeof(kLadder[0]); i++) {
    uint32_t hz = kLadder[i];
    if (hz > capHz) continue;  // skip entries above the configured cap

    if (SD.begin(csPin, SPI, hz)) {
      Serial.printf("[SD] Running at %lu Hz\n", (unsigned long)hz);
      return true;
    }
    SD.end();
  }

  // Last-resort attempt at the cap's own value in case it doesn't match a
  // ladder rung exactly (e.g. a user set sdMaxSpiHz=25000000).
  if (capHz > 0 && SD.begin(csPin, SPI, capHz)) {
    Serial.printf("[SD] Running at %lu Hz\n", (unsigned long)capHz);
    return true;
  }

  return false;
}

// ---- SD bus recovery ----
// After sustained heavy SD I/O (large batch uploads at high SPI clock), the
// SD/SPI bus can wedge, causing SD.exists()/SD.open() to fail even though
// the underlying file is intact on the card. A full unmount + remount at
// the negotiated best clock usually clears it. Rate-limited so a genuinely
// dead card doesn't get hammered with a remount attempt on every file.
static uint32_t sdLastRecoverMs = 0;
bool sdTryRecover() {
  uint32_t now = millis();
  if (now - sdLastRecoverMs < 4000) return false;
  sdLastRecoverMs = now;

  Serial.println("[SD] Attempting SD bus recovery (SD.end + reinit)...");
  SD.end();
  delay(50);
  bool ok = sdBeginBestClock(pins.sd_cs);
  sdOk = ok;
  Serial.printf("[SD] Recovery %s\n", ok ? "OK" : "FAILED");
  return ok;
}

// ---- Move to uploaded ----

bool moveToUploaded(const String& srcPath) {
  if (!sdOk) return false;
  if (!SD.exists(srcPath)) {
    Serial.print("[SD] moveToUploaded: source missing, attempting recovery: ");
    Serial.println(srcPath);
    if (!sdTryRecover() || !SD.exists(srcPath)) {
      Serial.print("[SD] moveToUploaded: source missing: ");
      Serial.println(srcPath);
      return false;
    }
    Serial.println("[SD] moveToUploaded: SD recovered, source found");
  }

  // Ensure folder exists
  if (!SD.exists("/uploaded")) {
    Serial.println("[SD] Creating /uploaded ...");
    if (!SD.mkdir("/uploaded")) {
      Serial.println("[SD] ERROR: SD.mkdir(/uploaded) failed");
      return false;
    }
  }

  String dstPath = String("/uploaded/") + pathBasename(srcPath);

  // If destination exists, remove it first (rename may fail otherwise)
  if (SD.exists(dstPath)) {
    Serial.print("[SD] Removing existing dst: ");
    Serial.println(dstPath);
    SD.remove(dstPath);
  }

  Serial.print("[SD] Moving ");
  Serial.print(srcPath);
  Serial.print(" -> ");
  Serial.println(dstPath);

  bool ok = SD.rename(srcPath, dstPath);
  if (!ok) {
    Serial.println("[SD] ERROR: SD.rename failed");
    // Last resort: copy + delete (some SD libs are picky)
    File in = SD.open(srcPath, FILE_READ);
    if (!in) { Serial.println("[SD] copy fallback: open src failed"); return false; }

    File out = SD.open(dstPath, FILE_WRITE);
    if (!out) { Serial.println("[SD] copy fallback: open dst failed"); in.close(); return false; }

    uint8_t buf[1024];
    while (true) {
      int n = in.read(buf, sizeof(buf));
      if (n <= 0) break;
      out.write(buf, n);
      delay(0);
    }
    out.flush();
    out.close();
    in.close();

    // Verify copy
    if (!SD.exists(dstPath)) {
      Serial.println("[SD] copy fallback: dst does not exist after write");
      return false;
    }

    if (!SD.remove(srcPath)) {
      Serial.println("[SD] copy fallback: WARNING failed to remove src after copy");
      // still consider it moved-ish, but warn
    }

    Serial.println("[SD] copy fallback: OK");
    return true;
  }

  Serial.println("[SD] Move OK");
  return true;
}

// ---- Log file ----

// Row limit per CSV file: ~120 bytes/row × 100k = ~12 MB, safely under the
// WDGoWars 15 MB upload cap. When the limit is reached, the active CSV is
// closed and a fresh one is opened with proper WiGLE headers.
static const uint32_t CSV_MAX_ROWS = 100000;
static uint32_t       csvRowCount  = 0;

// Sanitise a user-provided device name for safe use in filenames.
// Keeps alphanumerics, hyphens, underscores; replaces spaces with _;
// strips everything else; truncates to 20 chars.
static String sanitiseDeviceName(const String& raw) {
  String s = raw;
  s.replace(" ", "_");
  for (int i = (int)s.length() - 1; i >= 0; i--) {
    char c = s[i];
    if (!isAlphaNumeric(c) && c != '_' && c != '-') s.remove(i, 1);
  }
  if (s.length() > 20) s = s.substring(0, 20);
  return s;
}

static String newCsvFilename() {
  if (!SD.exists("/logs")) SD.mkdir("/logs");

  // Build optional prefix:  "name_Piglet_"  or empty
  String prefix = "";
  if (cfg.deviceName.length() > 0) {
    String safe = sanitiseDeviceName(cfg.deviceName);
    if (safe.length() > 0) prefix = safe + "_Piglet_";
  }

  // Make collisions extremely unlikely: millis + esp_random
  for (int tries = 0; tries < 25; tries++) {
    uint32_t r = (uint32_t)esp_random();
    char buf[100];
    snprintf(buf, sizeof(buf), "/logs/%sWiGLE_%lu_%08lX.csv",
             prefix.c_str(), (unsigned long)millis(), (unsigned long)r);
    String p(buf);
    if (!SD.exists(p)) return p;
  }

  // last-resort fallback
  char buf2[100];
  snprintf(buf2, sizeof(buf2), "/logs/%sWiGLE_%lu.csv",
           prefix.c_str(), (unsigned long)millis());
  return String(buf2);
}

bool openLogFile() {
  if (!sdOk) return false;

  // Close any previous handle
  closeLogFile();

  // Pick a fresh filename FIRST
  currentCsvPath = newCsvFilename();

  Serial.print("[SD] Opening log file: ");
  Serial.println(currentCsvPath);

  logFile = SD.open(currentCsvPath, FILE_WRITE);
  if (!logFile) {
    Serial.println("[SD] Failed to open log file for write");
    return false;
  }

  // Build device field: Piglet-{name} if set, otherwise Piglet-Wardriver
  String deviceField = "Piglet-Wardriver";
  if (cfg.deviceName.length() > 0) {
    String safe = sanitiseDeviceName(cfg.deviceName);
    if (safe.length() > 0) deviceField = "Piglet-" + safe;
  }

  // Derive board model from the ACTIVE pin-map name (set by detectPinsByChip /
  // pickPinsFromConfig at boot). This reflects the real chip even when
  // cfg.board="auto", avoiding the old fallback that always wrote "Xiao-ESP32S3".
  String pn = String(pins.name); pn.toUpperCase();
  String boardModel;
  if      (pn.indexOf("EXP") >= 0) boardModel = "Xiao-ESP32S3-Exp";
  else if (pn.indexOf("C5")  >= 0) boardModel = "Xiao-ESP32C5";
  else if (pn.indexOf("C6")  >= 0) boardModel = "Xiao-ESP32C6";
  else                              boardModel = "Xiao-ESP32S3";

  // WiGLE WiFi 1.6 header
  logFile.print("WigleWifi-1.6,appRelease=");
  logFile.print(FIRMWARE_VERSION);
  logFile.print(",model="); logFile.print(boardModel);
  logFile.print(",release=1,device="); logFile.print(deviceField);
  logFile.print(",display=SSD1306-128x64,board="); logFile.print(boardModel);
  logFile.println(",brand=Piglet,star=Sol,body=3,subBody=0");
  logFile.println("MAC,SSID,AuthMode,FirstSeen,Channel,Frequency,RSSI,CurrentLatitude,CurrentLongitude,AltitudeMeters,AccuracyMeters,RCOIs,MfgrId,Type");
  logFile.flush();
  csvRowCount = 0;

  Serial.println("[SD] Log file initialized with WiGLE headers");
  return true;
}

void closeLogFile() {
  if (logFile) {
    Serial.println("[SD] Closing log file");
    logFile.flush();
    logFile.close();
  }
}

// Copy `in` into fixed buffer `out` (size outSize), doubling any embedded
// double-quote characters per CSV escaping rules (RFC4180-style). Truncates
// safely rather than overflowing if the (unexpectedly long/malformed) input
// wouldn't fit.
static void csvEscapeQuotes(const String& in, char* out, size_t outSize) {
  size_t o = 0;
  size_t n = in.length();
  for (size_t i = 0; i < n; i++) {
    char c = in[i];
    if (c == '"') {
      if (o + 2 >= outSize) break;  // no room left for the doubled quote
      out[o++] = '"';
      out[o++] = '"';
    } else {
      if (o + 1 >= outSize) break;
      out[o++] = c;
    }
  }
  out[o] = '\0';
}

void appendWigleRow(const String& mac, const String& ssid, const String& auth,
                    const String& firstSeen, int channel, int rssi,
                    double lat, double lon, double altM, double accM) {
  if (!sdOk || !logFile) return;

  // Rotate CSV before it exceeds the WDGoWars 15 MB upload limit
  if (csvRowCount >= CSV_MAX_ROWS) {
    Serial.println("[SD] CSV row limit reached, rotating log file");
    closeLogFile();
    if (!openLogFile()) return;
  }

  // Frequency in MHz derived from channel number (WiGLE 1.6 requirement)
  uint32_t freq = 0;
  if      (channel >= 1  && channel <= 13) freq = 2407u + (uint32_t)channel * 5;
  else if (channel == 14)                  freq = 2484u;
  else if (channel >= 32)                  freq = 5000u + (uint32_t)channel * 5;

  // Build the row into fixed stack buffers instead of chaining Arduino
  // String concatenation (each `+=` above used to allocate/copy/free on the
  // heap). This runs once per scan result and can fire hundreds of times
  // per batch in dense areas, so per-row heap churn adds up fast.
  char safeSsid[70];  // SSID is at most 32 bytes; worst case (all quotes) doubles to 64
  csvEscapeQuotes(ssid, safeSsid, sizeof(safeSsid));

  char line[256];
  int len = snprintf(line, sizeof(line),
                      "%s,\"%s\",%s,%s,%d,%u,%d,%.6f,%.6f,%.1f,%.1f,,,WIFI",
                      mac.c_str(), safeSsid, auth.c_str(), firstSeen.c_str(),
                      channel, (unsigned)freq, rssi, lat, lon, altM, accM);
  if (len < 0) {
    Serial.println("[SD] appendWigleRow: encoding error, row skipped");
    return;
  }
  if ((size_t)len >= sizeof(line)) {
    Serial.printf("[SD] appendWigleRow: row truncated (needed %d bytes, buffer %u) — check SSID\n",
                  len, (unsigned)sizeof(line));
  }

  size_t written = logFile.println(line);
  csvRowCount++;

  // Detect silent write failure — if println() returns 0 for a non-empty line,
  // the SD card or file handle is broken. Attempt to reopen the log file once;
  // if that also fails, mark SD as unusable until next boot.
  if (written == 0 && len > 0) {
    static uint8_t consecFails = 0;
    consecFails++;
    Serial.printf("[SD] Write failed (%u consecutive)\n", consecFails);
    if (consecFails >= 3) {
      Serial.println("[SD] Attempting log reopen...");
      closeLogFile();
      if (openLogFile()) {
        Serial.println("[SD] Reopen OK — retrying write");
        logFile.println(line);  // best-effort retry
        consecFails = 0;
      } else {
        Serial.println("[SD] Reopen FAILED — SD marked unusable");
        sdOk = false;
      }
    }
    return;
  }

  // Flush less often to avoid stalls (SD writes can block hard). Threshold
  // tightened from 25 to 16 lines so large batches get broken into smaller
  // chunks; yield() after each flush gives WiFi/GPS/web-server/watchdog a
  // chance to run between chunks instead of only after the whole batch.
  static uint32_t lastFlushMs = 0;
  static uint32_t linesSinceFlush = 0;

  linesSinceFlush++;

  uint32_t nowMs = millis();
  if (linesSinceFlush >= 16 || (nowMs - lastFlushMs) >= 2000) {
    logFile.flush();
    yield();
    lastFlushMs = nowMs;
    linesSinceFlush = 0;
  }
}
