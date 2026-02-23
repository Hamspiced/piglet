#include "Config.h"
#include "Globals.h"
#include <SD.h>
#include <ArduinoJson.h>

#if __has_include(<esp_chip_info.h>)
  #include <esp_chip_info.h>
#else
  #include <esp_system.h>
#endif

static const char* CFG_PATH = "/wardriver.cfg";

// ---------------- Board / Pin detection ----------------

const PinMap& detectPinsByChip() {
  String m = ESP.getChipModel();  // e.g. "ESP32-C5"
  Serial.print("[CHIP] Model: ");
  Serial.println(m);

  m.toUpperCase();

  if (m.indexOf("C5") >= 0) return PINS_C5;
  if (m.indexOf("C6") >= 0) return PINS_C6;
  if (m.indexOf("S3") >= 0) return PINS_S3;

  // fallback
  return PINS_S3;
}

PinMap pickPinsFromConfig() {
  // cfg.board is expected to be: "auto" | "s3" | "c6" (lowercase)
  if (cfg.board == "s3") return PINS_S3;
  if (cfg.board == "exp") return PINS_S3_EXP_BASE;
  if (cfg.board == "c6") return PINS_C6;
  if (cfg.board == "c5") return PINS_C5;
  return detectPinsByChip(); // "auto" or anything else
}

// True when THIS wardriver hardware is configured as ESP32-C5 (5GHz capable)
bool wardriverIsC5() {
  // Explicit config override wins
  if (cfg.board == "c5") return true;
  if (cfg.board == "c6" || cfg.board == "s3" || cfg.board == "exp") return false;

  // Auto mode: infer from the active pinmap name (set during setup())
  String n = String(pins.name);
  n.toUpperCase();
  return (n.indexOf("C5") >= 0);
}

// ---------------- Parsing helpers ----------------

String trimCopy(String s) {
  s.trim();
  return s;
}

bool parseKeyValueLine(const String& lineIn, String& keyOut, String& valOut) {
  String line = lineIn;
  line.trim();
  if (line.length() == 0) return false;
  if (line[0] == '#') return false;           // comment
  if (line.startsWith("//")) return false;    // comment

  int eq = line.indexOf('=');
  if (eq < 0) return false;

  String k = line.substring(0, eq);
  String v = line.substring(eq + 1);

  k.trim();
  v.trim();

  // allow quoted values
  if (v.length() >= 2 && ((v[0] == '"' && v[v.length()-1] == '"') || (v[0] == '\'' && v[v.length()-1] == '\''))) {
    v = v.substring(1, v.length()-1);
    v.trim();
  }

  if (k.length() == 0) return false;
  keyOut = k;
  valOut = v;
  return true;
}

void cfgAssignKV(const String& k, const String& v) {
  if (k == "wigleBasicToken") cfg.wigleBasicToken = v;
  else if (k == "homeSsid")   cfg.homeSsid = v;
  else if (k == "homePsk")    cfg.homePsk = v;
  else if (k == "wardriverSsid") cfg.wardriverSsid = v;
  else if (k == "wardriverPsk")  cfg.wardriverPsk = v;
  else if (k == "gpsBaud") {
    uint32_t b = (uint32_t)v.toInt();
    if (b > 0) cfg.gpsBaud = b;
  }
  else if (k == "scanMode") {
    if (v == "aggressive" || v == "powersaving") cfg.scanMode = v;
  }
  else if (k == "board") {
    String vv = v; vv.toLowerCase();
    if (vv == "auto" || vv == "s3" || vv == "exp" || vv == "c5" || vv == "c6") cfg.board = vv;
  }
  else if (k == "speedUnits") {
    String vv = v; vv.toLowerCase();
    if (vv == "kmh" || vv == "mph") cfg.speedUnits = vv;
  }
  else if (k == "battPin") {
    int p = v.toInt();
    cfg.battPin = (v == "0") ? 0 : (p > 0 ? p : -1);  // allow GPIO 0; treat non-numeric as disabled
  }
  else if (k == "maxBootUploads") {
    int n = v.toInt();
    if (n >= 0) cfg.maxBootUploads = n;  // 0 = disable auto-upload
  }
}

// ---------------- Load / Save ----------------

bool loadConfigFromSD() {
  Serial.println("[CFG] loadConfigFromSD() start");
  if (!sdOk) {
    Serial.println("[CFG] SD not OK, skipping config load");
    return false;
  }

  // Prefer new plain-text config
  if (SD.exists(CFG_PATH)) {
    File f = SD.open(CFG_PATH, FILE_READ);
    if (!f) {
      Serial.println("[CFG] Failed to open /wardriver.cfg for read");
      return false;
    }

    while (f.available()) {
      String line = f.readStringUntil('\n');
      String k, v;
      if (parseKeyValueLine(line, k, v)) {
        cfgAssignKV(k, v);
      }
    }
    f.close();

    Serial.println("[CFG] Loaded config from /wardriver.cfg:");
    Serial.print("      wardriverSsid: "); Serial.println(cfg.wardriverSsid);
    Serial.print("      wardriverPsk:  "); Serial.println(cfg.wardriverPsk.length() ? "(set)" : "(empty)");
    Serial.print("      homeSsid:      "); Serial.println(cfg.homeSsid);
    Serial.print("      homePsk:       "); Serial.println(cfg.homePsk.length() ? "(set)" : "(empty)");
    Serial.print("      gpsBaud:       "); Serial.println(cfg.gpsBaud);
    Serial.print("      scanMode:      "); Serial.println(cfg.scanMode);
    Serial.print("      wigle token:   "); Serial.println(cfg.wigleBasicToken.length() ? "(set)" : "(empty)");
    return true;
  }

  // Backward compat: if old JSON exists, load it once, then save as CFG
  if (SD.exists("/wardriver.json")) {
    Serial.println("[CFG] Found legacy /wardriver.json, importing once -> /wardriver.cfg");

    File f = SD.open("/wardriver.json", FILE_READ);
    if (!f) {
      Serial.println("[CFG] Failed to open /wardriver.json for read");
      return false;
    }

    DynamicJsonDocument doc(2048);
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err) {
      Serial.print("[CFG] JSON parse error: ");
      Serial.println(err.c_str());
      return false;
    }

    cfg.wigleBasicToken = doc["wigleBasicToken"] | "";
    cfg.homeSsid        = doc["homeSsid"]        | "";
    cfg.homePsk         = doc["homePsk"]         | "";
    cfg.wardriverSsid   = doc["wardriverSsid"]   | cfg.wardriverSsid;
    cfg.wardriverPsk    = doc["wardriverPsk"]    | cfg.wardriverPsk;
    cfg.gpsBaud         = doc["gpsBaud"]         | cfg.gpsBaud;
    cfg.scanMode        = doc["scanMode"]        | cfg.scanMode;

    // Save as new format
    bool ok = saveConfigToSD();
    Serial.println(ok ? "[CFG] Legacy JSON imported + saved to CFG" : "[CFG] Legacy JSON imported but CFG save failed");
    return ok;
  }

  Serial.println("[CFG] No /wardriver.cfg (or legacy json). Using defaults.");
  return false;
}

bool saveConfigToSD() {
  Serial.println("[CFG] saveConfigToSD() start");
  if (!sdOk) {
    Serial.println("[CFG] SD not OK, cannot save config");
    return false;
  }

  // Remove old file if present
  if (SD.exists(CFG_PATH)) {
    if (!SD.remove(CFG_PATH)) {
      Serial.println("[CFG] WARNING: failed to remove existing /wardriver.cfg");
    }
  }

  File f = SD.open(CFG_PATH, FILE_WRITE);
  if (!f) {
    Serial.println("[CFG] Failed to open /wardriver.cfg for write");
    return false;
  }

  // Simple key=value format (human editable)
  f.println("# Piglet Wardriver config (key=value)");
  f.println("# Lines starting with # are comments");
  f.println("");

  f.print("wigleBasicToken="); f.println(cfg.wigleBasicToken);
  f.print("homeSsid=");        f.println(cfg.homeSsid);
  f.print("homePsk=");         f.println(cfg.homePsk);
  f.print("wardriverSsid=");   f.println(cfg.wardriverSsid);
  f.print("wardriverPsk=");    f.println(cfg.wardriverPsk);
  f.print("gpsBaud=");         f.println(cfg.gpsBaud);
  f.print("scanMode=");        f.println(cfg.scanMode);
  f.print("speedUnits=");     f.println(cfg.speedUnits);
  f.print("board=");          f.println(cfg.board);
  f.print("battPin=");        f.println(cfg.battPin);
  f.print("maxBootUploads="); f.println(cfg.maxBootUploads);

  f.flush();
  f.close();

  Serial.println("[CFG] Saved /wardriver.cfg OK");
  return true;
}
