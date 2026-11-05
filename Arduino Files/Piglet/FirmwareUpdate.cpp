#include "FirmwareUpdate.h"
#include "Globals.h"
#include "SDUtils.h"
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Update.h>

// This board's own release asset, plus the other two boards' assets staged
// into /Firmware/ for the mesh Core-push engine (MeshNode.cpp's
// coreOtaPreload()/coreOtaStart()). Names must exactly match the asset
// names release-firmware.yml publishes.
static const char* FW_OWN_ASSET = "Piglet.ota.bin";
static const char* FW_OTHER_ASSETS[] = { "TDongleC5_Piglet.ota.bin", "PigletNode.ota.bin" };
static const int   FW_OTHER_ASSET_COUNT = 2;
static const int   FW_NODE_ASSET_IDX = 1;  // index of "PigletNode.ota.bin" within FW_OTHER_ASSETS[]

static const char* GITHUB_API_HOST = "api.github.com";
static const char* GITHUB_API_PATH = "/repos/Hamspiced/piglet/releases/latest";

bool     fwUpdateChecked = false;
bool     fwUpdateAvailable = false;
String   fwAvailableVersion = "";
String   fwReleaseNotes = "";
bool     fwInstalling = false;
String   fwInstallStage = "";
uint32_t fwInstallDownloadedBytes = 0;
uint32_t fwInstallTotalBytes = 0;
String   fwInstallError = "";

bool     fwStagingNode = false;
String   fwStageNodeError = "";

// Asset download URLs found by the last successful check.
static String fwOwnAssetUrl;
static String fwOtherAssetUrls[FW_OTHER_ASSET_COUNT];

// Parses "vMAJOR.MINOR" (or "MAJOR.MINOR", with/without a leading 'v') into
// a comparable (major, minor) pair -- a missing/garbled component reads as
// 0. Numeric comparison avoids lexical ordering, which would wrongly rank
// "v2.9" after "v2.10".
static void fwParseVersion(const String& verIn, int& major, int& minor) {
  String v = verIn;
  v.trim();
  if (v.length() && (v[0] == 'v' || v[0] == 'V')) v = v.substring(1);
  int dot = v.indexOf('.');
  if (dot < 0) {
    major = v.toInt();
    minor = 0;
  } else {
    major = v.substring(0, dot).toInt();
    minor = v.substring(dot + 1).toInt();
  }
}

static bool fwIsNewer(const String& latest, const String& current) {
  int lMaj, lMin, cMaj, cMin;
  fwParseVersion(latest, lMaj, lMin);
  fwParseVersion(current, cMaj, cMin);
  if (lMaj != cMaj) return lMaj > cMaj;
  return lMin > cMin;
}

void fwCheckForUpdate() {
  fwUpdateChecked     = false;
  fwUpdateAvailable   = false;
  fwAvailableVersion  = "";
  fwReleaseNotes      = "";
  fwOwnAssetUrl       = "";
  for (int i = 0; i < FW_OTHER_ASSET_COUNT; i++) fwOtherAssetUrls[i] = "";

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[FWUPD] Skipped check (no STA WiFi)");
    fwUpdateChecked = true;
    return;
  }

  Serial.println("[FWUPD] Checking GitHub for a newer release...");

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(10000);
  if (!http.begin(client, String("https://") + GITHUB_API_HOST + GITHUB_API_PATH)) {
    Serial.println("[FWUPD] HTTP begin failed");
    fwUpdateChecked = true;
    return;
  }
  http.addHeader("User-Agent", "Piglet-Firmware");
  http.addHeader("Accept", "application/vnd.github+json");

  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    // A negative code (e.g. -1 = HTTPC_ERROR_CONNECTION_REFUSED) means the
    // TLS/TCP connection itself never completed -- no HTTP response was
    // ever received, so this is NOT the firmware concluding "no update
    // available". client.lastError() surfaces the actual mbedTLS/socket
    // failure (DNS, handshake, timeout, etc.) instead of just the bare,
    // uninformative HTTPClient error code.
    if (code < 0) {
      char errBuf[100];
      int sslErr = client.lastError(errBuf, sizeof(errBuf));
      Serial.printf("[FWUPD] GitHub API returned HTTP %d (TLS error %d: %s)\n", code, sslErr, errBuf);
    } else {
      Serial.printf("[FWUPD] GitHub API returned HTTP %d\n", code);
    }
    http.end();
    fwUpdateChecked = true;
    return;
  }

  // Filter to just the fields needed -- keeps the JSON document small no
  // matter how many assets a release has or how long its notes body is.
  DynamicJsonDocument filter(256);
  filter["tag_name"] = true;
  filter["body"]     = true;
  JsonObject assetFilter = filter["assets"].add<JsonObject>();
  assetFilter["name"] = true;
  assetFilter["browser_download_url"] = true;

  // GitHub's API serves this response with chunked transfer-encoding. The
  // chunk-size framing bytes are only stripped by HTTPClient's own body
  // readers (getString()/writeToStream()) -- reading the raw connection via
  // getStream() exposes those framing bytes directly to the JSON parser,
  // corrupting the parse (and was leaving the TLS connection in a state
  // that could crash a later check). Buffering via getString() first avoids
  // this; the filtered DynamicJsonDocument below still keeps actual parsed
  // memory use small regardless of the raw body size.
  String payload = http.getString();
  http.end();

  if (payload.length() == 0) {
    Serial.println("[FWUPD] Empty response body");
    fwUpdateChecked = true;
    return;
  }

  DynamicJsonDocument doc(6144);
  DeserializationError err =
    deserializeJson(doc, payload, DeserializationOption::Filter(filter));

  if (err) {
    Serial.printf("[FWUPD] JSON parse failed: %s\n", err.c_str());
    fwUpdateChecked = true;
    return;
  }

  String tag = doc["tag_name"] | "";
  if (tag.length() == 0) {
    Serial.println("[FWUPD] No tag_name in response");
    fwUpdateChecked = true;
    return;
  }

  // Resolve every asset's download URL unconditionally -- even when this
  // board is already on the latest release (!fwIsNewer() below). "Store
  // Piglet Node Firmware" (Web UI) needs the PigletNode asset URL whether
  // or not this Core itself needs an update, so bailing out early here (as
  // this used to do) would leave fwOtherAssetUrls[] empty and break that
  // button on an up-to-date Core.
  for (JsonObject asset : doc["assets"].as<JsonArray>()) {
    String name = asset["name"] | "";
    String url  = asset["browser_download_url"] | "";
    if (name == FW_OWN_ASSET) fwOwnAssetUrl = url;
    for (int i = 0; i < FW_OTHER_ASSET_COUNT; i++) {
      if (name == FW_OTHER_ASSETS[i]) fwOtherAssetUrls[i] = url;
    }
  }
  String notes = doc["body"] | "";
  fwReleaseNotes = notes.substring(0, 300);

  if (!fwIsNewer(tag, FIRMWARE_VERSION)) {
    Serial.printf("[FWUPD] Up to date (latest=%s, current=%s)\n", tag.c_str(), FIRMWARE_VERSION);
    fwAvailableVersion = tag;  // still useful to show "Latest Available" even when not newer
    fwUpdateChecked = true;
    return;
  }

  if (fwOwnAssetUrl.length() == 0) {
    Serial.println("[FWUPD] Newer release found but no matching asset for this board");
    fwAvailableVersion = tag;
    fwUpdateChecked = true;
    return;
  }

  fwAvailableVersion = tag;
  fwUpdateAvailable  = true;
  fwUpdateChecked    = true;
  Serial.printf("[FWUPD] Update available: %s -> %s\n", FIRMWARE_VERSION, fwAvailableVersion.c_str());
}

// Streams a release asset into the active OTA flash partition via Update.h.
static bool fwDownloadAndFlashOwn() {
  fwInstallStage = String("Downloading ") + FW_OWN_ASSET + "...";

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);  // asset URLs redirect to objects.githubusercontent.com
  http.setTimeout(20000);
  if (!http.begin(client, fwOwnAssetUrl)) {
    fwInstallError = "HTTP begin failed";
    return false;
  }

  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    fwInstallError = "HTTP " + String(code);
    http.end();
    return false;
  }

  int len = http.getSize();
  if (len <= 0) {
    fwInstallError = "Unknown content length";
    http.end();
    return false;
  }
  fwInstallTotalBytes      = (uint32_t)len;
  fwInstallDownloadedBytes = 0;

  if (!Update.begin((size_t)len, U_FLASH)) {
    fwInstallError = String("Update.begin failed: ") + Update.errorString();
    http.end();
    return false;
  }

  WiFiClient* stream = http.getStreamPtr();
  uint8_t buf[1024];
  int remaining = len;
  bool writeFailed = false;
  while (http.connected() && remaining > 0) {
    size_t avail = stream->available();
    if (avail) {
      int want = (int)((avail > sizeof(buf)) ? sizeof(buf) : avail);
      int n = stream->readBytes(buf, want);
      if (n <= 0) break;
      if (Update.write(buf, n) != (size_t)n) { writeFailed = true; break; }
      remaining -= n;
      fwInstallDownloadedBytes += (uint32_t)n;
    } else {
      delay(2);
    }
    yield();
  }
  http.end();

  if (writeFailed) {
    fwInstallError = "Flash write failed";
    Update.abort();
    return false;
  }
  if (remaining > 0) {
    fwInstallError = "Download incomplete";
    Update.abort();
    return false;
  }
  if (!Update.end(true)) {
    fwInstallError = String("Update.end failed: ") + Update.errorString();
    return false;
  }
  return true;
}

// Streams a release asset straight to an SD file, deleting any existing
// file of the same name first (avoids a corrupt hybrid file if the new
// image is smaller and the write is interrupted). A missing/empty URL
// (asset not published in this release) is treated as "nothing to do",
// not a failure.
static bool fwDownloadToSD(const String& url, const String& destPath) {
  if (url.length() == 0) return false;
  if (!sdOk) return false;

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  http.setTimeout(20000);
  if (!http.begin(client, url)) return false;

  int code = http.GET();
  if (code != HTTP_CODE_OK) { http.end(); return false; }

  int len = http.getSize();
  if (len <= 0) { http.end(); return false; }

  if (!SD.exists("/Firmware")) SD.mkdir("/Firmware");
  if (SD.exists(destPath)) SD.remove(destPath);  // delete before write

  File f = SD.open(destPath, FILE_WRITE);
  if (!f) { http.end(); return false; }

  WiFiClient* stream = http.getStreamPtr();
  uint8_t buf[1024];
  int remaining = len;
  bool ok = true;
  while (http.connected() && remaining > 0) {
    size_t avail = stream->available();
    if (avail) {
      int want = (int)((avail > sizeof(buf)) ? sizeof(buf) : avail);
      int n = stream->readBytes(buf, want);
      if (n <= 0) break;
      if (f.write(buf, n) != (size_t)n) { ok = false; break; }
      remaining -= n;
    } else {
      delay(2);
    }
    yield();
  }
  f.close();
  http.end();

  if (!ok || remaining > 0) { SD.remove(destPath); return false; }
  return true;
}

bool fwInstallUpdate() {
  if (fwInstalling) return false;
  if (!fwUpdateAvailable || fwOwnAssetUrl.length() == 0) {
    fwInstallError = "No update available";
    return false;
  }
  if (WiFi.status() != WL_CONNECTED) {
    fwInstallError = "STA WiFi not connected";
    return false;
  }

  fwInstalling             = true;
  fwInstallError           = "";
  fwInstallDownloadedBytes = 0;
  fwInstallTotalBytes      = 0;

  // Stage the other two boards' images on SD first -- cheap, doesn't touch
  // this board's running firmware, and lets the mesh Core-push engine use
  // them immediately even if the own-firmware flash step below fails.
  for (int i = 0; i < FW_OTHER_ASSET_COUNT; i++) {
    fwInstallStage = String("Staging ") + FW_OTHER_ASSETS[i] + "...";
    String dest = String("/Firmware/") + FW_OTHER_ASSETS[i];
    bool ok = fwDownloadToSD(fwOtherAssetUrls[i], dest);
    Serial.printf("[FWUPD] Stage %s: %s\n", FW_OTHER_ASSETS[i], ok ? "OK" : "skipped/failed");
  }

  bool ok = fwDownloadAndFlashOwn();
  fwInstalling = false;
  if (!ok) {
    fwInstallStage = "Failed";
    Serial.printf("[FWUPD] Install failed: %s\n", fwInstallError.c_str());
    return false;
  }

  fwInstallStage = "Rebooting...";
  Serial.println("[FWUPD] Flash OK -- restarting");
  delay(500);
  ESP.restart();
  return true;  // unreached
}

bool fwHasNodeAsset() {
  return fwOtherAssetUrls[FW_NODE_ASSET_IDX].length() > 0;
}

bool fwStageNodeFirmware() {
  if (fwStagingNode) return false;
  if (WiFi.status() != WL_CONNECTED) {
    fwStageNodeError = "STA WiFi not connected";
    return false;
  }

  fwStagingNode    = true;
  fwStageNodeError = "";

  if (!fwHasNodeAsset()) {
    // Not resolved yet -- e.g. this Core is already on the latest release,
    // so no Web UI "Check for Update" click has happened this session.
    // Refresh from GitHub now rather than making the operator do that first.
    fwCheckForUpdate();
  }

  bool ok = false;
  if (!fwHasNodeAsset()) {
    fwStageNodeError = "PigletNode asset not found in latest release";
  } else {
    String dest = String("/Firmware/") + FW_OTHER_ASSETS[FW_NODE_ASSET_IDX];
    ok = fwDownloadToSD(fwOtherAssetUrls[FW_NODE_ASSET_IDX], dest);
    if (!ok) fwStageNodeError = "Download failed";
  }

  fwStagingNode = false;
  Serial.printf("[FWUPD] Store Piglet Node Firmware: %s\n", ok ? "OK" : fwStageNodeError.c_str());
  return ok;
}
