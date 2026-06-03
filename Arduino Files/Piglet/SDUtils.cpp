#include "SDUtils.h"
#include "Globals.h"
#include <unordered_set>
#include <deque>

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

// ---- Move to uploaded ----

bool moveToUploaded(const String& srcPath) {
  if (!sdOk) return false;
  if (!SD.exists(srcPath)) {
    Serial.print("[SD] moveToUploaded: source missing: ");
    Serial.println(srcPath);
    return false;
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

  // Derive a human-readable board model from the config key
  String boardModel = "Xiao-ESP32S3"; // default / "auto"
  if (cfg.board == "c5")  boardModel = "Xiao-ESP32C5";
  else if (cfg.board == "c6")  boardModel = "Xiao-ESP32C6";
  else if (cfg.board == "exp") boardModel = "Xiao-ESP32S3-Exp";

  // WiGLE WiFi 1.6 header
  logFile.print("WigleWifi-1.6,appRelease=");
  logFile.print(FIRMWARE_VERSION);
  logFile.print(",model="); logFile.print(boardModel);
  logFile.print(",release=1,device="); logFile.print(deviceField);
  logFile.print(",board="); logFile.print(boardModel);
  logFile.println(",brand=Piglet");
  logFile.println("MAC,SSID,AuthMode,FirstSeen,Channel,Frequency,RSSI,CurrentLatitude,CurrentLongitude,AltitudeMeters,AccuracyMeters,RCOIs,MfgrId,Type");
  logFile.flush();

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

// ---- Log-once dedupe (matches the upstream JCMK/Biscuit mac_history[200]) ----
// Each BSSID is written once until evicted past the cap (FIFO ring), so a
// stationary AP isn't re-logged every scan. appendWigleRow is the single WiGLE
// write path — called both by the solo scanner and by the mesh Core for records
// forwarded by nodes — so one shared ring keeps the CSV (and WiGLE merge rate)
// from ballooning whether wardriving solo or as a cluster.
struct WifiSeenRing {
  explicit WifiSeenRing(size_t cap = 200) : cap_(cap ? cap : 1) {}
  bool firstTime(const String& mac) {
    if (mac.length() < 17) return true;          // unparseable -> don't suppress
    uint64_t key = 0;
    for (int i = 0; i < 6; i++)
      key |= (uint64_t)strtoul(mac.c_str() + i * 3, nullptr, 16) << (i * 8);
    if (seen_.count(key)) return false;
    seen_.insert(key);
    order_.push_back(key);
    while (seen_.size() > cap_ && !order_.empty()) {
      seen_.erase(order_.front());
      order_.pop_front();
    }
    return true;
  }
 private:
  std::unordered_set<uint64_t> seen_;
  std::deque<uint64_t> order_;
  size_t cap_;
};

void appendWigleRow(const String& mac, const String& ssid, const String& auth,
                    const String& firstSeen, int channel, int rssi,
                    double lat, double lon, double altM, double accM) {
  if (!sdOk || !logFile) return;

  // Log each BSSID once (one ring covers solo + Core node-forwarded writes).
  static WifiSeenRing gWigleSeen;
  if (!gWigleSeen.firstTime(mac)) return;

  String safeSsid = ssid;
  safeSsid.replace("\"", "\"\"");

  String line;
  line.reserve(256);
  line += mac; line += ",";
  line += "\""; line += safeSsid; line += "\",";
  line += auth; line += ",";
  line += firstSeen; line += ",";
  line += String(channel); line += ",";
  // Frequency in MHz derived from channel number (WiGLE 1.6 requirement)
  uint32_t freq = 0;
  if      (channel >= 1  && channel <= 13) freq = 2407u + (uint32_t)channel * 5;
  else if (channel == 14)                  freq = 2484u;
  else if (channel >= 32)                  freq = 5000u + (uint32_t)channel * 5;
  line += String(freq); line += ",";
  line += String(rssi); line += ",";
  line += String(lat, 6); line += ",";
  line += String(lon, 6); line += ",";
  line += String(altM, 1); line += ",";
  line += String(accM, 1); line += ",";
  line += ",0,WIFI"; // RCOIs (empty), MfgrId (0), Type

  size_t written = logFile.println(line);

  // Detect silent write failure — if println() returns 0 for a non-empty line,
  // the SD card or file handle is broken. Attempt to reopen the log file once;
  // if that also fails, mark SD as unusable until next boot.
  if (written == 0 && line.length() > 0) {
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

  // Flush less often to avoid stalls (SD writes can block hard)
  static uint32_t lastFlushMs = 0;
  static uint32_t linesSinceFlush = 0;

  linesSinceFlush++;

  uint32_t nowMs = millis();
  if (linesSinceFlush >= 25 || (nowMs - lastFlushMs) >= 2000) {
    logFile.flush();
    lastFlushMs = nowMs;
    linesSinceFlush = 0;
  }
}
