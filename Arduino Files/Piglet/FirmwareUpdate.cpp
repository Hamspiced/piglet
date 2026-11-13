#include "FirmwareUpdate.h"
#include "Globals.h"
#include "SDUtils.h"
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Update.h>
#include <stdarg.h>
#include "esp_heap_caps.h"
#include "JsonPsramAllocator.h"

// Minimal pull-based de-chunking Stream wrapper for HTTP chunked
// transfer-encoding. GitHub's release API response (currently 34KB+ for
// this repo, and only growing as more assets/changelog accumulate) is
// served chunked (no Content-Length). http.getStream()/getStreamPtr()
// exposes the RAW connection -- chunk-size framing bytes are only
// stripped by HTTPClient's own body readers (getString()/writeToStream()),
// so parsing the raw stream directly would corrupt the JSON. Buffering the
// de-chunked body into one String first (via writeToStream()) was tried,
// but a single-digit-KB reserve() guess was hugely undersized against the
// real 34KB+ response -- String's grow-by-doubling then needs several large
// reallocations (8KB->16KB->32KB->~34KB+), each momentarily holding both
// the old and new buffer alive at once, on top of the TLS session's own
// buffers, against this device's much smaller (~80KB) free-heap budget.
// That was fragmenting/exhausting the heap badly enough to crash shortly
// afterward, regardless of how large the reserve() guess was bumped to.
// This wrapper performs the same de-chunking HTTPClient::writeToStream()
// does internally, but pull-based so ArduinoJson parses the response as it
// arrives -- peak memory stays a few hundred bytes (ArduinoJson's own
// parsing state) no matter how large the raw response is.
class ChunkedReader : public Stream {
public:
  explicit ChunkedReader(WiFiClient* client) : _client(client) {}

  int available() override {
    fillChunk();
    return (int)_remaining;
  }
  int read() override {
    fillChunk();
    if (_remaining == 0) return -1;
    // ArduinoJson's Stream reader explicitly avoids calling read() directly
    // because the bare, non-blocking _client->read() can return -1 the
    // instant a TLS record hasn't finished arriving/decrypting yet. Called
    // from Stream::timedRead()'s own retry loop (which has no yield()), a
    // bare read() here busy-spins and can starve the WiFi/lwIP task on this
    // single-core chip until the 1 s default Stream timeout gives up mid-
    // document -- producing a spurious "IncompleteInput" parse failure well
    // before the real end of a 34KB+ response. readBytes() uses the
    // underlying client's own proven blocking-with-timeout implementation
    // (the same one used successfully everywhere else in this codebase),
    // so it waits for a byte instead of giving up immediately.
    uint8_t b;
    if (_client->readBytes(&b, 1) != 1) return -1;
    _remaining--;
    _totalRead++;
    return b;
  }
  int peek() override {
    fillChunk();
    if (_remaining == 0) return -1;
    return _client->peek();
  }
  void flush() override {}
  size_t write(uint8_t) override { return 0; }  // write side unused

private:
  WiFiClient* _client;
  size_t      _remaining = 0;
  bool        _done      = false;
  bool        _first     = true;
  size_t      _totalRead = 0;  // bytes delivered via read() so far -- diagnostics only

  void fillChunk() {
    while (_remaining == 0 && !_done) {
      if (!_first) {
        // Consume the trailing CRLF after the previous chunk's data.
        char trail[2];
        int n = _client->readBytes(trail, 2);
        if (n != 2) {
          // A genuine end-of-stream is always a "0" chunk-size line (handled
          // below), never a short/aborted trailing-CRLF read. If this ever
          // fires, the underlying TLS/TCP read stalled or the connection
          // was cut mid-stream -- surfaced here (byte offset + bytes
          // actually read) since this exact spot is suspected of causing
          // the recurring "IncompleteInput" parse failures on GitHub's
          // 34KB+ chunked release-API response.
          Serial.printf("[FWUPD] ChunkedReader: short trailing-CRLF read (%d/2 bytes) after %u bytes -- ending stream early\n",
                        n, (unsigned)_totalRead);
          _done = true;
          return;
        }
      }
      _first = false;
      String line = _client->readStringUntil('\n');
      line.trim();
      long sz = strtol(line.c_str(), nullptr, 16);
      if (sz <= 0) { _done = true; return; }
    _remaining = (size_t)sz;
    }
  }
};

// Same blocking-read rationale as ChunkedReader above, for the other response
// shape this endpoint can return: a plain Content-Length body instead of
// chunked transfer-encoding (confirmed seen in practice -- getSize() > 0).
// Without this, deserializeJson(doc, *http.getStreamPtr(), ...) falls back to
// ArduinoJson's default Stream handling, which calls the bare, non-blocking
// read() directly and can hit the same spurious IncompleteInput failure
// ChunkedReader was built to avoid.
class SizedReader : public Stream {
public:
  SizedReader(WiFiClient* client, size_t totalLen) : _client(client), _remaining(totalLen) {}

  int available() override { return (int)_remaining; }
  int read() override {
    if (_remaining == 0) return -1;
    uint8_t b;
    if (_client->readBytes(&b, 1) != 1) return -1;
    _remaining--;
    return b;
  }
  int peek() override {
    if (_remaining == 0) return -1;
    return _client->peek();
  }
  void flush() override {}
  size_t write(uint8_t) override { return 0; }

private:
  WiFiClient* _client;
  size_t      _remaining;
};

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
String   fwCheckError = "";

bool     fwDownloading = false;
bool     fwDownloaded = false;
String   fwDownloadStage = "";
uint32_t fwDownloadedBytes = 0;
uint32_t fwDownloadTotalBytes = 0;
String   fwDownloadError = "";

bool     fwInstalling = false;
String   fwInstallError = "";

String   fwLog = "";
static const size_t FW_LOG_MAX = 2048;  // capped buffer -- oldest whole lines trimmed from the front

bool     fwStagingNode = false;
String   fwStageNodeError = "";

// Asset download URLs found by the last successful check.
static String fwOwnAssetUrl;
static String fwOtherAssetUrls[FW_OTHER_ASSET_COUNT];

// printf-style: both Serial.printf()s and appends an elapsed-seconds-
// prefixed line to the capped fwLog buffer the Web UI's download/install
// modal polls. Oldest whole lines are dropped once the buffer exceeds
// FW_LOG_MAX, so a long-running transfer can't grow this unbounded.
static void fwLogAppend(const char* fmt, ...) {
  char msg[160];
  va_list args;
  va_start(args, fmt);
  vsnprintf(msg, sizeof(msg), fmt, args);
  va_end(args);
  Serial.printf("[FWUPD] %s\n", msg);

  char line[180];
  snprintf(line, sizeof(line), "[%lus] %s\n", (unsigned long)(millis() / 1000), msg);
  fwLog += line;
  if (fwLog.length() > FW_LOG_MAX) {
    int cut = (int)(fwLog.length() - FW_LOG_MAX);
    int nl = fwLog.indexOf('\n', cut);
    fwLog = (nl >= 0) ? fwLog.substring(nl + 1) : "";
  }
}

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

// Logs free/largest-contiguous MALLOC_CAP_DMA heap -- the narrow pool the
// hardware AES/DMA driver allocates its (small) per-TLS-session descriptors
// from, separate from (and a subset of) general MALLOC_CAP_INTERNAL free
// heap. General free heap/largest-block can look perfectly healthy while
// this specific pool is exhausted/fragmented -- that mismatch is exactly
// the "esp-aes: Failed to allocate memory" / "SSL - Memory allocation
// failed" signature seen on Web UI-triggered Check-for-Update calls after a
// long uptime with a lot of prior TLS churn (boot upload storm, other
// button clicks). Same pattern as WigleUpload.cpp's logDmaHeap().
static void fwLogDmaHeap(const char* tag) {
  Serial.printf("[FWUPD] %s: dmaFree=%u dmaLargest=%u\n", tag,
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
}

// Note: deliberately does NOT reset fwUpdateAvailable/fwAvailableVersion/
// fwReleaseNotes/fwOwnAssetUrl/fwOtherAssetUrls up front. A Web UI-triggered
// recheck can fail (e.g. transient DMA-pool exhaustion, see fwLogDmaHeap()
// below) after a prior check -- boot-time or manual -- already found a real
// update; blanking those fields before knowing whether this attempt will
// succeed made a failed recheck erase a previously-detected update from the
// Web UI, which read as "GitHub not reporting the correct version". Those
// fields are now only overwritten once a complete, valid GitHub response is
// in hand (see the bottom of this function). Every failure path below just
// records fwCheckError and returns, leaving prior state untouched.
void fwCheckForUpdate() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[FWUPD] Skipped check (no STA WiFi)");
    fwCheckError    = "STA WiFi not connected";
    fwUpdateChecked = true;
    return;
  }

  Serial.println("[FWUPD] Checking GitHub for a newer release...");
  fwLogDmaHeap("Pre-check");
  fwCheckError = "";

  // Web UI-triggered calls can follow a long run of other TLS-heavy activity
  // (boot upload storm, other Check-for-Update/test-key clicks) that left
  // the narrow AES/DMA-descriptor pool fragmented. Closing a TLS session's
  // sockets/buffers doesn't always return them to that pool synchronously --
  // some of that cleanup is posted to and processed by the WiFi/lwIP task.
  // A short yield()+delay() here can't defragment the heap (this platform's
  // allocator has no compaction), but it does give that deferred cleanup a
  // chance to actually run and release blocks back to the pool before this
  // attempt races a measurement/allocation against it.
  yield();
  delay(100);

  // Filter to just the fields needed -- keeps the JSON document small no
  // matter how many assets a release has or how long its notes body is.
  JsonDocument filter(psramJsonAllocator());
  filter["tag_name"] = true;
  filter["body"]     = true;
  JsonObject assetFilter = filter["assets"].add<JsonObject>();
  assetFilter["name"] = true;
  assetFilter["browser_download_url"] = true;

  // Retry the WHOLE connect+GET+parse sequence, not just the connect --
  // unlike the upload paths' connect-only retries, this response is held
  // open over a multi-second TLS session (34KB+), so it's also vulnerable
  // to a transient esp-aes/DMA-pool hiccup mid-transfer rather than just at
  // connect time: a clean 200 response that then truncates partway through
  // the body (IncompleteInput), not a parsing bug. This is most likely to
  // bite via the Web UI's "Check for Update" button (as opposed to the
  // identical call at boot, the very first TLS operation since reset with a
  // fully rested DMA pool) because by then the page's own recurring status-
  // poll fetches (status.json/update/status/mesh endpoints) have been
  // competing for the same narrow pool -- confirmed by instrumented testing:
  // dmaLargest sat at an identical, depressed value across all 3 attempts of
  // a failing check, then recovered within a few seconds once loop() (and
  // therefore server.handleClient()) ran freely again. See the dmaFailure
  // fail-fast branch below for why retrying a DMA allocation failure
  // in-place doesn't help and is handled differently from other failures.
  JsonDocument doc(psramJsonAllocator());
  bool gotValidDoc = false;

  for (int attempt = 1; attempt <= 3 && !gotValidDoc; attempt++) {
    WiFiClientSecure client;
    client.setInsecure();
    // HTTPClient::setTimeout() only forwards to the underlying client if
    // already connected() -- harmless here since it's called pre-connect,
    // but means it never actually reaches `client`. Set it directly too,
    // since ChunkedReader's per-byte reads below rely on this client's own
    // configured timeout to wait properly instead of giving up too soon.
    client.setTimeout(10000);
    HTTPClient http;
    http.setTimeout(10000);
    if (!http.begin(client, String("https://") + GITHUB_API_HOST + GITHUB_API_PATH)) {
      Serial.printf("[FWUPD] HTTP begin failed (attempt %d/3)\n", attempt);
      if (attempt < 3) { delay(500); yield(); continue; }
      fwCheckError    = "Could not start HTTPS connection";
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
      bool dmaFailure = false;
      if (code < 0) {
        char errBuf[100];
        int sslErr = client.lastError(errBuf, sizeof(errBuf));
        Serial.printf("[FWUPD] GitHub API returned HTTP %d (TLS error %d: %s) -- attempt %d/3\n", code, sslErr, errBuf, attempt);
        // -32512 (MBEDTLS_ERR_SSL_ALLOC_FAILED) and esp-aes's own "Failed to
        // allocate memory" (logged separately by the driver, not visible to
        // us as a distinct code) both stem from the same DMA-pool pressure.
        // Flagged here so the fail-fast branch below can skip retrying it.
        dmaFailure = (sslErr == -32512);
        fwLogDmaHeap("Check connect fail");
      } else {
        Serial.printf("[FWUPD] GitHub API returned HTTP %d -- attempt %d/3\n", code, attempt);
      }
      http.end();
      if (dmaFailure) {
        // Retrying in place doesn't help: the narrow DMA pool is depressed
        // by OTHER connections (the Web UI's own polling loops), not a
        // momentary hiccup that clears itself within this handler's own
        // retry delay -- instrumented testing showed dmaLargest identical
        // across all 3 attempts of a failing check. Worse, every attempt
        // spent here is time server.handleClient() isn't running, letting
        // those other connections' pending data pile up instead of
        // draining. Fail fast so loop() gets control back sooner and the
        // pool can recover the way it's observed to -- the Web UI already
        // preserves the last known update status, so pressing the button
        // again a few seconds later is harmless.
        fwCheckError    = "Low memory (TLS alloc failed) -- wait a few seconds and try again";
        fwUpdateChecked = true;
        return;
      }
      if (attempt < 3) { delay(500); yield(); continue; }
      fwCheckError    = "GitHub connection failed";
      fwUpdateChecked = true;
      return;
    }

    // See ChunkedReader's comment above for why this parses the response
    // through a blocking-read wrapper instead of buffering it whole or
    // handing ArduinoJson the raw stream directly. GitHub's release API
    // response has been observed both chunked (getSize() <= 0) and with a
    // real Content-Length (getSize() > 0) -- SizedReader covers the latter
    // case with the same readBytes()-based blocking reads ChunkedReader uses.
    doc.clear();
    DeserializationError err;
    int bodySize = http.getSize();
    Serial.printf("[FWUPD] Response getSize()=%d -- parsing via %s path (attempt %d/3)\n",
                  bodySize, (bodySize > 0) ? "sized" : "chunked", attempt);
    if (bodySize > 0) {
      SizedReader reader(http.getStreamPtr(), (size_t)bodySize);
      err = deserializeJson(doc, reader, DeserializationOption::Filter(filter));
    } else {
      ChunkedReader reader(http.getStreamPtr());
      err = deserializeJson(doc, reader, DeserializationOption::Filter(filter));
    }
    http.end();

    if (err) {
      Serial.printf("[FWUPD] JSON parse failed: %s -- attempt %d/3\n", err.c_str(), attempt);
      if (attempt < 3) { delay(500); yield(); continue; }
      fwCheckError    = "Failed to parse GitHub response";
      fwUpdateChecked = true;
      return;
    }

    gotValidDoc = true;
  }

  String tag = doc["tag_name"] | "";
  if (tag.length() == 0) {
    Serial.println("[FWUPD] No tag_name in response");
    fwCheckError    = "GitHub response missing tag_name";
    fwUpdateChecked = true;
    return;
  }

  // A complete, valid response is in hand -- safe to replace the published
  // state now. Resolve every asset's download URL unconditionally -- even
  // when this board is already on the latest release (!fwIsNewer() below).
  // "Store Piglet Node Firmware" (Web UI) needs the PigletNode asset URL
  // whether or not this Core itself needs an update, so bailing out early
  // here (as this used to do) would leave fwOtherAssetUrls[] empty and
  // break that button on an up-to-date Core.
  fwOwnAssetUrl = "";
  for (int i = 0; i < FW_OTHER_ASSET_COUNT; i++) fwOtherAssetUrls[i] = "";
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
    fwUpdateAvailable  = false;
    fwUpdateChecked    = true;
    return;
  }

  if (fwOwnAssetUrl.length() == 0) {
    Serial.println("[FWUPD] Newer release found but no matching asset for this board");
    fwAvailableVersion = tag;
    fwUpdateAvailable  = false;
    fwUpdateChecked    = true;
    return;
  }

  fwAvailableVersion = tag;
  fwUpdateAvailable  = true;
  fwUpdateChecked    = true;
  Serial.printf("[FWUPD] Update available: %s -> %s\n", FIRMWARE_VERSION, fwAvailableVersion.c_str());
}

// ================================================================
//  Non-blocking download: this board's own release asset -> SD card
//  Driven one bounded step at a time by fwDownloadTick(), called every
//  loop() iteration, so server.handleClient() keeps servicing the Web UI's
//  status-poll requests throughout a multi-megabyte transfer (mirrors the
//  coreOtaTick() mesh-push state machine in MeshNode.cpp). The connect step
//  (TLS handshake + headers) still happens synchronously within a single
//  tick call -- only the body transfer is sliced across ticks.
// ================================================================
enum class FwDlPhase : uint8_t { Idle, Preparing, Connecting, Downloading, RetryWait };

static struct {
  FwDlPhase         phase          = FwDlPhase::Idle;
  WiFiClientSecure*  client        = nullptr;
  HTTPClient*        http          = nullptr;
  File               file;
  String             destPath;
  int                total         = 0;
  int                remaining     = 0;
  int                connectAttempt = 0;
  int                lastLoggedPct = 0;
  uint32_t           lastDataMs    = 0;
  uint32_t           prepStartMs   = 0;
  uint32_t           prepLastLogMs = 0;
  uint8_t            sessionAttempt = 0;  // whole-session (Preparing->...->fail) retry count
  uint32_t           retryAtMs      = 0;
} fwDl;

static const uint32_t FW_DL_STALL_MS = 20000;  // no body bytes at all for this long -> give up
static const int      FW_DL_CONNECT_MAX_ATTEMPTS = 3;

// Preparing-phase budget: loop() already stops scanning and WDGWars polling
// the instant fwDownloading goes true (see Piglet.ino), but an async scan
// already in flight, or deferred lwIP/mbedTLS cleanup from a just-finished
// Check-for-Update call, both take a moment to actually release their
// memory back to the DMA pool. Rather than race that cleanup the way a
// direct-to-Connecting start used to, wait it out first: a mandatory
// minimum settle so background work has time to actually wind down, then
// keep polling (up to a hard cap, so a boot session where the pool truly
// never recovers doesn't hang forever) before proceeding regardless.
// Field testing showed dmaLargest climbing steadily well past the old 8 s
// cap -- 25588 -> 36852 -> 38900 bytes over 8 s and still rising, just shy
// of FW_DL_DMA_HEALTHY_THRESHOLD -- so the old cap was giving up right
// before the pool would have cleared the threshold. 30 s gives that slow
// deferred cleanup much more room to finish.
static const uint32_t FW_DL_PREP_MIN_MS = 3000;
static const uint32_t FW_DL_PREP_MAX_MS = 30000;

// Below this MALLOC_CAP_DMA largest-free-block reading, a failed connect to
// this download target is treated as resource-constrained rather than a
// plain network hiccup. Chosen from instrumented testing: a pristine,
// never-used-this-boot pool starts above 60000, but the very first TLS
// connection of a boot session appears to permanently retain some
// mbedTLS/esp-aes session-level allocation afterward -- every reading taken
// for the rest of that boot session has topped out at ~34804-36852, no
// matter how long the device sits idle. This download target has never
// succeeded even starting from that ~36852 "recovered" ceiling, so anything
// below this threshold is presumed insufficient for its particular TLS
// handshake (a larger cert chain than the lightweight GitHub API check,
// via the objects.githubusercontent.com redirect).
static const uint32_t FW_DL_DMA_HEALTHY_THRESHOLD = 40000;

// Whole-session auto-retry budget: this flow is driven from the OLED/button
// (or a Web UI modal the operator may not be watching), so nobody is
// necessarily there to notice a failure and manually click/press "try
// again". Rather than strand the screen on a dead error, keep retrying the
// entire Preparing->Connecting sequence on its own -- the DMA pool has been
// observed to vary by several KB over tens of seconds (not just monotonic
// recovery), so a later attempt genuinely has a different chance than the
// one that just failed. Capped so a truly persistent problem (no WiFi, SD
// gone, GitHub down) eventually gives up instead of looping forever.
static const uint32_t FW_DL_SESSION_RETRY_DELAY_MS = 8000;
static const uint8_t  FW_DL_SESSION_RETRY_MAX      = 12;

static void fwDownloadCleanup() {
  if (fwDl.file) fwDl.file.close();
  if (fwDl.http) { fwDl.http->end(); delete fwDl.http; fwDl.http = nullptr; }
  if (fwDl.client) { delete fwDl.client; fwDl.client = nullptr; }
}

static void fwDownloadFail(const String& err) {
  fwLogAppend("ERROR: %s", err.c_str());
  fwDownloadCleanup();
  if (sdOk && fwDl.destPath.length() && SD.exists(fwDl.destPath)) SD.remove(fwDl.destPath);
  fwDownloadError = err;

  fwDl.sessionAttempt++;
  if (fwDl.sessionAttempt < FW_DL_SESSION_RETRY_MAX) {
    // fwDownloading stays true throughout the wait -- the OLED/TFT "Updating"
    // screen and any open Web UI modal keep showing live status instead of
    // reverting to "press button to download" and silently waiting for a
    // manual retry that may never come.
    fwDownloadStage = "Failed -- retrying...";
    fwDl.retryAtMs  = millis() + FW_DL_SESSION_RETRY_DELAY_MS;
    fwDl.prepLastLogMs = 0;  // force an immediate countdown update on the next tick
    fwDl.phase      = FwDlPhase::RetryWait;
    fwLogAppend("Retrying in %lus (session attempt %d/%d)",
                (unsigned long)(FW_DL_SESSION_RETRY_DELAY_MS / 1000),
                (int)fwDl.sessionAttempt + 1, FW_DL_SESSION_RETRY_MAX);
    return;
  }

  fwDownloadStage = "Failed";
  fwDownloading   = false;
  fwDownloaded    = false;
  fwDl.phase      = FwDlPhase::Idle;
  fwLogAppend("Giving up after %d attempts -- press the download button/control again to retry", FW_DL_SESSION_RETRY_MAX);
}

void fwDownloadStart() {
  if (fwDownloading) return;
  if (!fwUpdateAvailable || fwOwnAssetUrl.length() == 0) {
    fwDownloadError = "No update available";
    return;
  }
  if (WiFi.status() != WL_CONNECTED) {
    fwDownloadError = "STA WiFi not connected";
    return;
  }
  if (!sdOk) {
    fwDownloadError = "SD not available";
    return;
  }

  fwLog                = "";
  fwDownloadError      = "";
  fwDownloadedBytes    = 0;
  fwDownloadTotalBytes = 0;
  fwDownloaded         = false;
  fwDownloadStage      = "Pausing background tasks...";
  fwDl.destPath        = String("/Firmware/") + FW_OWN_ASSET;
  fwDl.connectAttempt  = 0;
  fwDl.sessionAttempt  = 0;
  fwDl.prepStartMs     = millis();
  fwDl.prepLastLogMs   = 0;
  fwDl.phase           = FwDlPhase::Preparing;
  fwDownloading        = true;
  fwLogAppend("Starting download of %s -- pausing scanning and background jobs first", FW_OWN_ASSET);
}

void fwDownloadCancel() {
  if (!fwDownloading) return;
  fwLogAppend("Download cancelled by user");
  fwDownloadCleanup();
  if (sdOk && fwDl.destPath.length() && SD.exists(fwDl.destPath)) SD.remove(fwDl.destPath);
  fwDownloadStage = "Cancelled";
  fwDownloadError = "";
  fwDownloading   = false;
  fwDownloaded    = false;
  fwDl.phase      = FwDlPhase::Idle;
}

String fwOwnAssetPath() {
  return String("/Firmware/") + FW_OWN_ASSET;
}

// Deletes every file in /Firmware/ on the SD card -- this board's own
// downloaded-but-not-yet-installed image, plus anything staged for the mesh
// Core-push engine. Lets the operator clear out a stuck/partial download (or
// just reclaim SD space) without needing to pull the card. Refuses while a
// download or install is actively using that directory so a file can't be
// yanked out from under an in-flight write/flash.
bool fwClearFirmwareFolder() {
  if (fwDownloading || fwInstalling) return false;
  if (!sdOk) return false;

  File dir = SD.open("/Firmware");
  if (!dir) return true;              // directory doesn't exist -- nothing to clear
  if (!dir.isDirectory()) { dir.close(); return true; }

  uint32_t removed = 0;
  File f = dir.openNextFile();
  while (f) {
    bool   isDir = f.isDirectory();
    String path  = normalizeSdPath("/Firmware", f.name());
    f.close();
    if (!isDir && SD.remove(path)) removed++;
    f = dir.openNextFile();
  }
  dir.close();

  fwDownloaded         = false;
  fwDownloadedBytes    = 0;
  fwDownloadTotalBytes = 0;
  fwDownloadStage      = "";
  fwLogAppend("Cleared %u staged firmware file(s) from /Firmware", (unsigned)removed);
  return true;
}

void fwDownloadTick() {
  if (!fwDownloading) return;

  switch (fwDl.phase) {
    case FwDlPhase::RetryWait: {
      // Holding pattern after a failed whole-session attempt -- see
      // fwDownloadFail() for why this auto-retries instead of stopping.
      // Updates the visible countdown at most once a second.
      uint32_t nowMs = millis();
      uint32_t remainMs = (nowMs < fwDl.retryAtMs) ? (fwDl.retryAtMs - nowMs) : 0;
      if (fwDl.prepLastLogMs == 0 || nowMs - fwDl.prepLastLogMs >= 1000) {
        fwDl.prepLastLogMs = nowMs;
        fwDownloadStage = "Failed -- retrying in " + String((remainMs + 999) / 1000) + "s...";
      }
      if (nowMs >= fwDl.retryAtMs) {
        fwLogAppend("Retrying now (session attempt %d/%d)...", (int)fwDl.sessionAttempt + 1, FW_DL_SESSION_RETRY_MAX);
        fwDl.connectAttempt = 0;
        fwDl.prepStartMs    = millis();
        fwDl.prepLastLogMs  = 0;
        fwDownloadStage     = "Pausing background tasks...";
        fwDl.phase          = FwDlPhase::Preparing;
      }
      break;
    }

    case FwDlPhase::Preparing: {
      // loop() already forces scanning/WDGWars off the instant fwDownloading
      // went true above -- this phase just waits for that to actually take
      // effect (an async scan already in flight, or deferred lwIP/mbedTLS
      // cleanup from whatever TLS session ran right before this button
      // press) before spending the DMA pool on a new TLS handshake. Polls
      // at most once a second so the modal shows real elapsed time instead
      // of sitting unchanged on "Starting...".
      uint32_t elapsed = millis() - fwDl.prepStartMs;
      if (elapsed == 0 || millis() - fwDl.prepLastLogMs >= 1000) {
        fwDl.prepLastLogMs = millis();
        uint32_t dmaLargest = heap_caps_get_largest_free_block(MALLOC_CAP_DMA);
        fwDownloadStage = "Waiting for memory to free up (" + String(elapsed / 1000) + "s)...";
        fwLogAppend("Settling -- dmaLargest=%u (%lus elapsed)", (unsigned)dmaLargest, (unsigned long)(elapsed / 1000));
      }

      bool minElapsed = elapsed >= FW_DL_PREP_MIN_MS;
      bool timedOut   = elapsed >= FW_DL_PREP_MAX_MS;
      uint32_t dmaLargestNow = heap_caps_get_largest_free_block(MALLOC_CAP_DMA);
      bool healthy = dmaLargestNow >= FW_DL_DMA_HEALTHY_THRESHOLD;

      if (minElapsed && (healthy || timedOut)) {
        if (healthy) {
          fwLogAppend("Memory looks healthy (dmaLargest=%u) -- connecting", (unsigned)dmaLargestNow);
        } else {
          fwLogAppend("Memory still low (dmaLargest=%u) after %lus -- connecting anyway",
                      (unsigned)dmaLargestNow, (unsigned long)(FW_DL_PREP_MAX_MS / 1000));
        }
        fwDl.connectAttempt = 0;
        fwDownloadStage     = "Connecting...";
        fwDl.phase          = FwDlPhase::Connecting;
      }
      break;
    }

    case FwDlPhase::Connecting: {
      fwDl.connectAttempt++;
      fwLogAppend("Connecting (attempt %d/%d)...", fwDl.connectAttempt, FW_DL_CONNECT_MAX_ATTEMPTS);

      bool   connectFailed = false;
      bool   dmaFailure    = false;
      String failReason;

      // Manually handle GitHub's one-hop redirect to
      // objects.githubusercontent.com instead of HTTPC_FORCE_FOLLOW_REDIRECTS.
      // That mode closes the first (github.com) TLS session and immediately
      // opens the second (heavier) one back-to-back with no settle time.
      // Instrumented testing showed this redirect target can fail to
      // allocate memory for RSA certificate verification (-17040: RSA
      // public key operation / BIGNUM allocation failure) even starting
      // from an otherwise healthy pool -- giving the first connection's
      // just-freed resources a brief moment to actually return to the
      // MALLOC_CAP_DMA pool before opening the second TLS session is the
      // one lever this manual approach can pull that FORCE_FOLLOW_REDIRECTS
      // cannot.
      String targetUrl = fwOwnAssetUrl;
      for (int hop = 0; hop < 2; hop++) {
        fwDl.client = new WiFiClientSecure();
        fwDl.client->setInsecure();
        fwDl.http = new HTTPClient();
        fwDl.http->setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);  // handled manually here
        // HTTPClient::GET() below is a blocking call -- loop() (and therefore
        // server.handleClient(), and the Web UI's progress poll) is frozen
        // for however long it takes to resolve. A real DMA-alloc failure
        // surfaces almost instantly, but a genuine network hang would
        // otherwise freeze the Web UI for the full timeout, twice per
        // attempt (two hops) -- kept short so a stuck connection doesn't
        // leave the modal looking dead for too long.
        fwDl.http->setTimeout(10000);

        if (!fwDl.http->begin(*fwDl.client, targetUrl)) {
          connectFailed = true;
          failReason    = "HTTP begin failed";
          break;
        }

        int code = fwDl.http->GET();
        bool isRedirect = (code == HTTP_CODE_MOVED_PERMANENTLY || code == HTTP_CODE_FOUND ||
                           code == HTTP_CODE_SEE_OTHER || code == HTTP_CODE_TEMPORARY_REDIRECT);

        if (isRedirect && hop == 0) {
          String location = fwDl.http->getLocation();
          fwDl.http->end();
          delete fwDl.http;   fwDl.http   = nullptr;
          delete fwDl.client; fwDl.client = nullptr;
          if (location.length() == 0) {
            connectFailed = true;
            failReason    = "Redirect with no Location header";
            break;
          }
          targetUrl = location;
          fwLogAppend("Redirected to CDN -- settling before second connection");
          yield();
          delay(400);
          continue;  // second hop, fresh client/http against the CDN URL
        }

        if (code != HTTP_CODE_OK) {
          connectFailed = true;
          // Same rationale as fwCheckForUpdate(): a negative code means the
          // TLS/TCP connection itself never completed, and client.lastError()
          // surfaces the actual mbedTLS/socket failure instead of just the
          // bare, uninformative HTTPClient error code.
          if (code < 0) {
            char errBuf[100];
            int sslErr = fwDl.client->lastError(errBuf, sizeof(errBuf));
            failReason = "HTTP " + String(code) + " (TLS " + String(sslErr) + ": " + errBuf + ")";
            // DMA-pool pressure against this specific target does NOT
            // reliably surface as any one consistent mbedTLS error code --
            // observed in the field as -32512 (MBEDTLS_ERR_SSL_ALLOC_FAILED),
            // -29312 ("connection indicated an EOF"), -1 ("Generic error",
            // with an explicit "esp-aes: Failed to allocate memory" driver
            // log alongside it), and -17040 (RSA public key operation /
            // BIGNUM allocation failure) across different attempts and
            // sessions. Rather than guess at which mbedTLS codes mean the
            // same thing, classify directly from the measured pool size
            // instead -- see FW_DL_DMA_HEALTHY_THRESHOLD above.
            dmaFailure = (heap_caps_get_largest_free_block(MALLOC_CAP_DMA) < FW_DL_DMA_HEALTHY_THRESHOLD);
            fwLogDmaHeap("Download connect fail");
          } else {
            failReason = "HTTP " + String(code);
          }
          break;
        }

        break;  // success -- fwDl.client/fwDl.http stay alive for the Downloading phase
      }

      if (connectFailed) {
        fwLogAppend("%s -- attempt %d/%d", failReason.c_str(), fwDl.connectAttempt, FW_DL_CONNECT_MAX_ATTEMPTS);
        fwDownloadCleanup();
        if (dmaFailure) {
          fwDownloadFail("Low memory (TLS alloc failed) -- wait a few seconds and try again");
          return;
        }
        if (fwDl.connectAttempt < FW_DL_CONNECT_MAX_ATTEMPTS) {
          delay(500);
          return;  // retry next tick, phase stays Connecting
        }
        fwDownloadFail(failReason);
        return;
      }

      int len = fwDl.http->getSize();
      if (len <= 0) { fwDownloadFail("Unknown content length"); return; }

      if (!SD.exists("/Firmware")) SD.mkdir("/Firmware");
      if (SD.exists(fwDl.destPath)) SD.remove(fwDl.destPath);  // delete before write
      fwDl.file = SD.open(fwDl.destPath, FILE_WRITE);
      if (!fwDl.file) { fwDownloadFail("Cannot open destination file"); return; }

      fwDl.total               = len;
      fwDl.remaining           = len;
      fwDl.lastLoggedPct       = 0;
      fwDl.lastDataMs          = millis();
      fwDownloadTotalBytes     = (uint32_t)len;
      fwDownloadStage          = "Downloading...";
      fwLogAppend("Connected -- downloading %d bytes", len);
      fwDl.phase               = FwDlPhase::Downloading;
      break;
    }

    case FwDlPhase::Downloading: {
      if (!fwDl.http->connected() && fwDl.remaining > 0) {
        fwDownloadFail("Connection lost");
        return;
      }
      WiFiClient* stream = fwDl.http->getStreamPtr();
      size_t avail = stream->available();
      if (avail == 0) {
        if (millis() - fwDl.lastDataMs > FW_DL_STALL_MS) fwDownloadFail("Download stalled");
        return;  // nothing ready yet -- try again next tick
      }

      uint8_t buf[2048];
      int want = (int)((avail > sizeof(buf)) ? sizeof(buf) : avail);
      int n = stream->readBytes(buf, want);
      if (n <= 0) return;
      if (fwDl.file.write(buf, n) != (size_t)n) { fwDownloadFail("SD write failed"); return; }

      fwDl.remaining    -= n;
      fwDownloadedBytes += (uint32_t)n;
      fwDl.lastDataMs    = millis();

      int pct = (fwDl.total > 0) ? (int)(((uint64_t)fwDownloadedBytes * 100) / fwDl.total) : 0;
      if (pct >= fwDl.lastLoggedPct + 10) {
        fwDl.lastLoggedPct = (pct / 10) * 10;
        fwLogAppend("Downloaded %d%% (%u / %u bytes)", pct, (unsigned)fwDownloadedBytes, (unsigned)fwDl.total);
      }

      if (fwDl.remaining <= 0) {
        fwDl.file.close();
        fwDl.http->end();
        delete fwDl.http;   fwDl.http   = nullptr;
        delete fwDl.client; fwDl.client = nullptr;
        fwDownloading   = false;
        fwDownloaded    = true;
        fwDownloadStage = "Downloaded -- ready to install";
        fwLogAppend("Download complete (%u bytes)", (unsigned)fwDownloadedBytes);
        fwDl.phase = FwDlPhase::Idle;
      }
      break;
    }

    default:
      break;
  }
}

// Reads the already-downloaded SD file back in and flashes it via Update.h.
// Purely local (SD -> flash), no network -- a couple of seconds for a
// ~1.3MB image, so unlike the download step this stays a simple blocking
// call.
bool fwInstallFromSD() {
  if (fwInstalling) return false;
  if (!fwDownloaded) {
    fwInstallError = "No firmware downloaded";
    return false;
  }
  if (!sdOk) {
    fwInstallError = "SD not available";
    return false;
  }

  fwInstalling   = true;
  fwInstallError = "";

  String path = String("/Firmware/") + FW_OWN_ASSET;
  File f = SD.open(path, FILE_READ);
  if (!f) {
    fwInstallError = "Cannot open downloaded file";
    fwLogAppend("ERROR: %s", fwInstallError.c_str());
    fwInstalling = false;
    return false;
  }

  size_t size = f.size();
  if (size == 0) {
    f.close();
    fwInstallError = "Downloaded file is empty";
    fwLogAppend("ERROR: %s", fwInstallError.c_str());
    fwInstalling = false;
    return false;
  }

  if (!Update.begin(size, U_FLASH)) {
    fwInstallError = String("Update.begin failed: ") + Update.errorString();
    fwLogAppend("ERROR: %s", fwInstallError.c_str());
    f.close();
    fwInstalling = false;
    return false;
  }

  fwLogAppend("Flashing %u bytes...", (unsigned)size);
  uint8_t buf[1024];
  size_t  written = 0;
  bool    writeFailed = false;
  while (f.available()) {
    int n = f.read(buf, sizeof(buf));
    if (n <= 0) break;
    if (Update.write(buf, n) != (size_t)n) { writeFailed = true; break; }
    written += (size_t)n;
    yield();
  }
  f.close();

  if (writeFailed || written != size) {
    fwInstallError = "Flash write failed";
    fwLogAppend("ERROR: %s", fwInstallError.c_str());
    Update.abort();
    fwInstalling = false;
    return false;
  }
  if (!Update.end(true)) {
    fwInstallError = String("Update.end failed: ") + Update.errorString();
    fwLogAppend("ERROR: %s", fwInstallError.c_str());
    fwInstalling = false;
    return false;
  }

  fwLogAppend("Flash OK -- rebooting");
  Serial.println("[FWUPD] Flash OK -- restarting");
  delay(500);
  ESP.restart();
  return true;  // unreached
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

  // Retry transient TLS connect failures -- same esp-aes/DMA-descriptor
  // allocation pressure the test-key/token functions guard against can hit
  // here too, especially since this is often one of several back-to-back
  // HTTPS connections during a single "Install Update" pass. HTTPClient::
  // connect() re-attempts _client->connect() on every call as long as the
  // previous attempt left it disconnected (returnError() stops the client
  // on failure), so simply calling GET() again is sufficient -- no need to
  // http.end()/begin() between attempts.
  int code = -1;
  for (int attempt = 1; attempt <= 3; attempt++) {
    code = http.GET();
    if (code == HTTP_CODE_OK) break;
    if (code < 0) {
      char errBuf[100];
      int sslErr = client.lastError(errBuf, sizeof(errBuf));
      Serial.printf("[FWUPD] Stage download (%s) HTTP %d (TLS %d: %s) -- attempt %d/3\n",
                    destPath.c_str(), code, sslErr, errBuf, attempt);
      if (attempt < 3) { delay(500); yield(); }
    } else {
      Serial.printf("[FWUPD] Stage download (%s) HTTP %d\n", destPath.c_str(), code);
      break;  // a real HTTP response was received -- retrying won't change it
    }
  }
  if (code != HTTP_CODE_OK) {
    http.end();
    return false;
  }

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
