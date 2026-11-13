#include "WigleUpload.h"
#include "Globals.h"
#include "SDUtils.h"
#include "Display.h"
#include "GzipDeflate.h"
#include "UploadState.h"
#include <WiFiClientSecure.h>
#include <vector>
#include "esp_heap_caps.h"

// Scratch directory for gzip-compressed uploads -- cleared opportunistically
// (each file removed right after its upload attempt) rather than at a fixed
// point, so a mid-batch crash/reset never leaves more than one stale file.
static const char* GZ_SCRATCH_DIR = "/logs/.gzscratch";

static void ensureGzScratchDir() {
  if (!SD.exists(GZ_SCRATCH_DIR)) SD.mkdir(GZ_SCRATCH_DIR);
}

static String gzScratchPathFor(const String& basename) {
  return String(GZ_SCRATCH_DIR) + "/" + basename + ".gz";
}

static const char*     WDGWARS_HOST = "wdgwars.pl";
static const uint16_t  WDGWARS_PORT = 443;

// Logs free/largest-contiguous MALLOC_CAP_DMA heap -- the narrower pool the
// hardware AES/DMA driver allocates its (small) per-operation descriptors
// from, separate from general MALLOC_CAP_INTERNAL free heap. A failed
// client.connect() with plenty of MALLOC_CAP_INTERNAL free but a tight or
// zero MALLOC_CAP_DMA pool is the "esp-aes: Failed to allocate memory"
// signature; called on every failed TLS connect attempt below so the next
// occurrence has concrete numbers instead of a guess.
static void logDmaHeap(const char* tag) {
  Serial.printf("[HEAP] %s: dmaFree=%u dmaLargest=%u\n", tag,
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
}

// ---- WDGoWars async job tracking ----
// A 202 response only means the CSV was received and queued for import —
// it does NOT mean the server has finished processing it. Blocking the
// upload loop for up to 45 s per file waiting on that made both WDGoWars
// and WiGLE uploads (which run right after it, for the same file) look
// hung even though the file had already reached the server. Instead, the
// job is queued here and checked opportunistically from loop() via
// wdgwarsServicePendingJobs(), without blocking anything.
struct WdgwarsPendingJob {
  int      jobId;
  String   filename;
  uint32_t queuedAtMs;
  uint32_t lastCheckMs;
  // Exponential backoff per job (see WDGWARS_JOB_CHECK_INTERVAL_MS below) --
  // starts at the base interval and doubles (capped) each inconclusive
  // check, instead of hammering a fresh TLS connection every 4s for up to
  // 5 minutes straight. That steady background TLS churn was a prime
  // suspect for the AES/DMA-pool exhaustion behind repeated, sustained
  // (not just momentary) "esp-aes: Failed to allocate memory" failures
  // seen on foreground Test Token/Key TLS attempts during the same window.
  uint32_t nextDelayMs;
};
static std::vector<WdgwarsPendingJob> wdgwarsPendingJobs;
static const uint32_t WDGWARS_JOB_CHECK_INTERVAL_MS = 4000;            // base/min gap between checks
static const uint32_t WDGWARS_JOB_CHECK_MAX_MS      = 60000;           // backoff ceiling per job
static const uint32_t WDGWARS_JOB_MAX_AGE_MS        = 5UL * 60UL * 1000UL; // give up after 5 min

// ---- Token test ----

bool wigleTestToken() {
  wigleLastHttpCode = 0;

  if (WiFi.status() != WL_CONNECTED) {
    uploadLastResult = "No STA WiFi";
    wigleTokenStatus = -1;
    return false;
  }
  if (cfg.wigleBasicToken.length() < 8) {
    uploadLastResult = "No token set";
    wigleTokenStatus = -1;
    return false;
  }

  const char* pathsToTry[] = {
    "/api/v2/profile",
    "/api/v2/stats/user",
    "/api/v2/user/profile"
  };

  for (size_t i = 0; i < sizeof(pathsToTry)/sizeof(pathsToTry[0]); i++) {

    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(15000);

    // Optional debug
    Serial.printf("[NET] status=%d ssid=%s rssi=%d\n",
                  (int)WiFi.status(),
                  WiFi.SSID().c_str(),
                  WiFi.RSSI());
    Serial.printf("[NET] ip=%s gw=%s dns=%s\n",
                  WiFi.localIP().toString().c_str(),
                  WiFi.gatewayIP().toString().c_str(),
                  WiFi.dnsIP().toString().c_str());

    IPAddress ip;
    if (!WiFi.hostByName(WIGLE_HOST, ip)) {
      Serial.println("[NET] DNS FAIL for api.wigle.net");
    } else {
      Serial.print("[NET] api.wigle.net -> ");
      Serial.println(ip);
    }

    // Retry TLS connect -- transient esp-aes/DMA-descriptor allocation
    // failures (logged as "esp-aes: Failed to allocate memory") can make a
    // single connect() attempt fail even though overall free heap looks
    // fine; the AES driver needs a small allocation from the separate
    // MALLOC_CAP_DMA pool, which a short retry gives a chance to free up.
    // Same pattern already used by the upload functions below.
    bool connected = false;
    for (int attempt = 1; attempt <= 3; attempt++) {
      if (client.connect(WIGLE_HOST, WIGLE_PORT)) { connected = true; break; }
      char errBuf[100];
      int sslErr = client.lastError(errBuf, sizeof(errBuf));
      Serial.printf("[WiGLE] Test token TLS connect failed (attempt %d/3): error %d: %s\n", attempt, sslErr, errBuf);
      logDmaHeap("WiGLE test token connect fail");
      client.stop();
      delay(500);
      yield();
    }
    if (!connected) {
      uploadLastResult = "TLS connect fail";
      wigleTokenStatus = -1;
      return false;
    }

    String req =
      String("GET ") + pathsToTry[i] + " HTTP/1.1\r\n" +
      "Host: " + WIGLE_HOST + "\r\n" +
      "Authorization: Basic " + cfg.wigleBasicToken + "\r\n" +
      "Connection: close\r\n\r\n";

    client.print(req);

    // Track AP clients (unchanged behavior)
    if (apWindowActive) {
      wifi_mode_t m = WiFi.getMode();
      if (m == WIFI_AP || m == WIFI_AP_STA) {
        if (WiFi.softAPgetStationNum() > 0) {
          if (!apClientSeen) Serial.println("[WIFI] AP client connected");
          apClientSeen = true;
        }
      }
    }

    // Wait for response
    uint32_t waitStart = millis();
    while (!client.available() && client.connected() && (millis() - waitStart) < 15000) {
      delay(10);
      yield();
    }

    // Read status line FIRST
    String status = client.readStringUntil('\n');
    status.trim();

    // Drain headers quickly
    while (client.connected()) {
      String line = client.readStringUntil('\n');
      if (line == "\r" || line.length() == 0) break;
    }

    client.stop();

    int code = 0;
    if (status.startsWith("HTTP/")) {
      int sp1 = status.indexOf(' ');
      if (sp1 > 0) {
        int sp2 = status.indexOf(' ', sp1 + 1);
        if (sp2 > sp1) code = status.substring(sp1 + 1, sp2).toInt();
      }
    }

    wigleLastHttpCode = code;

    // 200 means token valid
    if (code == 200) {
      wigleTokenStatus = 1;
      uploadLastResult = "Token valid (200)";
      return true;
    }

    // 401/403 means definitely invalid
    if (code == 401 || code == 403) {
      wigleTokenStatus = -1;
      uploadLastResult = "Token invalid (" + String(code) + ")";
      return false;
    }

    // 404 / other codes: try next endpoint
  }

  // If none of the endpoints worked, we can't be sure.
  wigleTokenStatus = 0;
  uploadLastResult = "Token test inconclusive (" + String(wigleLastHttpCode) + ")";
  return false;
}

// ---- Single file upload ----

bool uploadFileToWigle(const String& path) {
  Serial.print("[WiGLE] uploadFileToWigle: ");
  Serial.println(path);

  wigleLastHttpCode = 0;

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WiGLE] Not connected to STA WiFi");
    uploadLastResult = "No STA WiFi";
    return false;
  }
  if (cfg.wigleBasicToken.length() < 8) {
    Serial.println("[WiGLE] No Basic token set");
    uploadLastResult = "No token set";
    return false;
  }
  if (!SD.exists(path)) {
    Serial.println("[WiGLE] File missing -- attempting SD recovery");
    if (!sdTryRecover() || !SD.exists(path)) {
      Serial.println("[WiGLE] File does not exist on SD");
      uploadLastResult = "File missing";
      return false;
    }
    Serial.println("[WiGLE] SD recovered -- file found");
  }

  File f = SD.open(path, FILE_READ);
  if (!f) {
    Serial.println("[WiGLE] Failed to open file -- attempting SD recovery");
    if (sdTryRecover()) f = SD.open(path, FILE_READ);
  }
  if (!f) {
    Serial.println("[WiGLE] Failed to open file");
    uploadLastResult = "Open fail";
    return false;
  }

  String boundary = "----Piglet-WARDRIVE-BOUNDARY";
  String filename = pathBasename(path);

  String pre =
    "--" + boundary + "\r\n"
    "Content-Disposition: form-data; name=\"file\"; filename=\"" + filename + "\"\r\n"
    "Content-Type: text/csv\r\n\r\n";

  String post =
    "\r\n--" + boundary + "--\r\n";

  uint32_t fileSize = f.size();
  uint32_t contentLen = (uint32_t)pre.length() + fileSize + (uint32_t)post.length();
  Serial.printf("[WiGLE] File size: %.2f MB (%u bytes)\n", fileSize / (1024.0 * 1024.0), (unsigned)fileSize);

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(25000);
  tlsMaybeSetBufferSizes(client, 512, 512);

  IPAddress ip;
  if (!WiFi.hostByName(WIGLE_HOST, ip)) {
    Serial.println("[WiGLE] DNS lookup failed");
    uploadLastResult = "DNS fail";
    f.close();
    return false;
  }
  
  // Retry TLS connect
  bool connected = false;
  for (int attempt = 1; attempt <= 3; attempt++) {
    Serial.printf("[WiGLE] Free heap before connect (attempt %d/3): %u bytes\n",
                  attempt, (unsigned)ESP.getFreeHeap());
    if (client.connect(WIGLE_HOST, WIGLE_PORT)) { 
      connected = true;
      break;
    }
    Serial.printf("[WiGLE] TLS connect failed (attempt %d/3)\n", attempt);
    client.stop();
    delay(500);
    yield();
  }

  if (!connected) {
    Serial.println("[WiGLE] TLS connect failed after 3 attempts");
    uploadLastResult = "TLS connect fail";
    f.close();
    return false;
  }

  // HTTP headers
  client.print("POST /api/v2/file/upload HTTP/1.0\r\n");
  client.print(String("Host: ") + WIGLE_HOST + "\r\n");
  client.print(String("Authorization: Basic ") + cfg.wigleBasicToken + "\r\n");
  client.print(String("Content-Type: multipart/form-data; boundary=") + boundary + "\r\n");
  client.print(String("Content-Length: ") + String(contentLen) + "\r\n");
  client.print("Connection: close\r\n\r\n");

  // Body
  client.print(pre);

  // Stream the file, tracking actual bytes sent so a short/failed SD read
  // (e.g. SD card under stress) is detected and aborted cleanly instead of
  // silently sending a body shorter than the Content-Length already sent.
  uint32_t fileSent = 0;
  uint8_t buf[1024];
  while (true) {
    int n = f.read(buf, sizeof(buf));
    if (n <= 0) break;
    size_t w = client.write(buf, n);
    fileSent += (uint32_t)w;
    if (w != (size_t)n) break;  // short write -- socket/SD trouble, stop here
    yield();
  }
  f.close();

  if (fileSent != fileSize) {
    client.stop();
    uploadLastResult = "Read/write error (body short)";
    Serial.printf("[WiGLE] Body incomplete: sent %u of %u bytes -- aborting\n",
                  (unsigned)fileSent, (unsigned)fileSize);
    return false;
  }

  client.print(post);
  client.flush();

  // Wait for response
  uint32_t waitStart = millis();
  while (!client.available() && client.connected() && (millis() - waitStart) < 30000) {
    delay(100);
    yield();
  }
  
  if (!client.available()) {
    client.stop();
    uploadLastResult = "No response";
    Serial.println("[WiGLE] No response from server (connection closed or timed out)");
    return false;
  }

  // Read status line
  String status = client.readStringUntil('\n');
  status.trim();

  int code = 0;
  if (status.startsWith("HTTP/")) {
    int sp1 = status.indexOf(' ');
    if (sp1 > 0) {
      int sp2 = status.indexOf(' ', sp1 + 1);
      if (sp2 > sp1) code = status.substring(sp1 + 1, sp2).toInt();
    }
  }

  wigleLastHttpCode = code;

  // Drain headers
  while (client.connected() || client.available()) {
    String line = client.readStringUntil('\n');
    if (line.length() < 3) break;
  }
  client.stop();

  if (code == 200) {
    uploadLastResult = "Uploaded OK (200)";
    return true;
  }

  uploadLastResult = "Upload failed (" + String(code) + ")";
  Serial.printf("[WiGLE] Upload FAILED HTTP %d\n", code);
  return false;
}

// ---- WDGoWars API key test ----
// Calls GET /api/me on wdgwars.pl with X-API-Key header.
// Sets uploadLastResult with a human-readable message.
bool wdgwarsTestKey() {
  uploadLastResult = "";

  if (WiFi.status() != WL_CONNECTED) {
    uploadLastResult = "No STA WiFi";
    return false;
  }
  if (cfg.wdgwarsApiKey.length() < 8) {
    uploadLastResult = "No API key set";
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(15000);

  // Retry TLS connect -- see the matching comment in wigleTestToken() for
  // why a single connect() attempt here can fail transiently.
  bool connected = false;
  for (int attempt = 1; attempt <= 3; attempt++) {
    if (client.connect(WDGWARS_HOST, WDGWARS_PORT)) { connected = true; break; }
    char errBuf[100];
    int sslErr = client.lastError(errBuf, sizeof(errBuf));
    Serial.printf("[WDGWars] Test key TLS connect failed (attempt %d/3): error %d: %s\n", attempt, sslErr, errBuf);
    logDmaHeap("WDGWars test key connect fail");
    client.stop();
    delay(500);
    yield();
  }
  if (!connected) {
    uploadLastResult = "TLS connect fail";
    return false;
  }

  client.print("GET /api/me HTTP/1.0\r\n");
  client.print(String("Host: ") + WDGWARS_HOST + "\r\n");
  client.print(String("X-API-Key: ") + cfg.wdgwarsApiKey + "\r\n");
  client.print("Connection: close\r\n\r\n");

  uint32_t waitStart = millis();
  while (!client.available() && client.connected() && (millis() - waitStart) < 10000) {
    delay(10);
    yield();
  }

  // Read HTTP status line
  String status = client.readStringUntil('\n');
  status.trim();

  int code = 0;
  if (status.startsWith("HTTP/")) {
    int sp1 = status.indexOf(' ');
    if (sp1 > 0) {
      int sp2 = status.indexOf(' ', sp1 + 1);
      if (sp2 > sp1) code = status.substring(sp1 + 1, sp2).toInt();
    }
  }

  // Drain headers, collect body
  String body = "";
  bool inBody = false;
  while (client.connected() || client.available()) {
    String line = client.readStringUntil('\n');
    line.trim();
    if (!inBody) {
      if (line.length() == 0) { inBody = true; }  // blank line = end of headers
    } else {
      body += line;
      if (body.length() > 512) break;  // don't read indefinitely
    }
  }
  client.stop();

  Serial.printf("[WDGWars] /api/me HTTP %d  body: %s\n", code,
                body.substring(0, 120).c_str());

  if (code == 200) {
    // Try to extract username from JSON body for a friendlier message
    String user = "";
    int uIdx = body.indexOf("\"username\":");
    if (uIdx >= 0) {
      int q1 = body.indexOf('"', uIdx + 11);
      int q2 = (q1 >= 0) ? body.indexOf('"', q1 + 1) : -1;
      if (q1 >= 0 && q2 > q1) user = body.substring(q1 + 1, q2);
    }
    uploadLastResult = user.length() ? "Key valid — " + user : "Key valid (200)";
    return true;
  }

  if (code == 401 || code == 403) {
    uploadLastResult = "Key invalid (" + String(code) + ")";
  } else if (code == 0) {
    uploadLastResult = "No response from server";
  } else {
    uploadLastResult = "Unexpected response (" + String(code) + ")";
  }
  return false;
}

// ---- WDGoWars single-file upload ----
// Auth: X-API-Key header  |  Endpoint: POST /api/upload-csv
// Wire format identical to WiGLE (multipart/form-data, field "file").

bool uploadFileToWdgwars(const String& path) {
  Serial.print("[WDGWars] uploadFileToWdgwars: ");
  Serial.println(path);

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WDGWars] Not connected to STA WiFi");
    return false;
  }
  if (cfg.wdgwarsApiKey.length() < 8) {
    Serial.println("[WDGWars] API key too short / not set");
    return false;
  }
  if (!SD.exists(path)) {
    Serial.println("[WDGWars] File does not exist -- attempting SD recovery");
    if (!sdTryRecover() || !SD.exists(path)) {
      Serial.println("[WDGWars] File does not exist");
      return false;
    }
    Serial.println("[WDGWars] SD recovered -- file found");
  }

  File f = SD.open(path, FILE_READ);
  if (!f) {
    Serial.println("[WDGWars] Failed to open file -- attempting SD recovery");
    if (sdTryRecover()) f = SD.open(path, FILE_READ);
  }
  if (!f) {
    Serial.println("[WDGWars] Failed to open file");
    return false;
  }

  String boundary = "----Piglet-WDGWARS-BOUNDARY";
  String filename = pathBasename(path);

  String pre =
    "--" + boundary + "\r\n"
    "Content-Disposition: form-data; name=\"file\"; filename=\"" + filename + "\"\r\n"
    "Content-Type: text/csv\r\n\r\n";

  String post = "\r\n--" + boundary + "--\r\n";

  uint32_t fileSize   = f.size();
  uint32_t contentLen = (uint32_t)pre.length() + fileSize + (uint32_t)post.length();
  Serial.printf("[WDGWars] File size: %.2f MB (%u bytes)\n", fileSize / (1024.0 * 1024.0), (unsigned)fileSize);

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(25000);
  tlsMaybeSetBufferSizes(client, 512, 512);

  bool connected = false;
  for (int attempt = 1; attempt <= 3; attempt++) {
    Serial.printf("[WDGWars] Free heap before connect (attempt %d/3): %u bytes\n",
                  attempt, (unsigned)ESP.getFreeHeap());
    if (client.connect(WDGWARS_HOST, WDGWARS_PORT)) {
      connected = true;
      break;
    }
    Serial.printf("[WDGWars] TLS connect failed (attempt %d/3)\n", attempt);
    client.stop();
    delay(500);
    yield();
  }

  if (!connected) {
    uploadLastResult = "WDGW: TLS connect fail";
    Serial.println("[WDGWars] TLS connect failed after 3 attempts");
    f.close();
    return false;
  }

  // HTTP request — WDGoWars V2 async endpoint. A `.gz`-suffixed path means
  // this is a scratch file already gzip-compressed by GzipDeflate.cpp --
  // signal that with the standard Content-Encoding header (the multipart
  // part's own Content-Type stays text/csv; it's the transport encoding of
  // the body that's gzip, same as any other gzip-compressed POST).
  bool isGzip = path.endsWith(".gz");
  client.print("POST /api/v2/upload-csv HTTP/1.0\r\n");
  client.print(String("Host: ") + WDGWARS_HOST + "\r\n");
  client.print(String("X-API-Key: ") + cfg.wdgwarsApiKey + "\r\n");
  client.print(String("Content-Type: multipart/form-data; boundary=") + boundary + "\r\n");
  if (isGzip) client.print("Content-Encoding: gzip\r\n");
  client.print(String("Content-Length: ") + String(contentLen) + "\r\n");
  client.print("Connection: close\r\n\r\n");

  client.print(pre);

  // Stream file with partial-write retry; track actual bytes sent.
  uint32_t fileSent = 0;
  uint8_t buf[512];  // modest chunk size helps with TLS record framing
  bool writeError = false;
  while (!writeError) {
    int n = f.read(buf, sizeof(buf));
    if (n <= 0) break;
    int offset = 0;
    while (offset < n) {
      int w = client.write(buf + offset, n - offset);
      if (w <= 0) { writeError = true; break; }
      offset += w;
      fileSent += (uint32_t)w;
    }
    yield();
  }
  f.close();
  client.print(post);
  client.flush();

  uint32_t totalSent = (uint32_t)pre.length() + fileSent + (uint32_t)post.length();
  Serial.printf("[WDGWars] ContentLen=%u  sent=%u  (%s)\n",
                contentLen, totalSent,
                (totalSent == contentLen) ? "MATCH" : "*** MISMATCH ***");
  if (writeError || totalSent != contentLen) {
    client.stop();
    uploadLastResult = "WDGW: write error (body short)";
    Serial.println("[WDGWars] Body write incomplete — aborting");
    return false;
  }

  // Wait for response — distinguish real timeout from server closing connection
  uint32_t waitStart = millis();
  while (!client.available() && client.connected() && (millis() - waitStart) < 60000) {
    delay(100);
    yield();
  }
  uint32_t elapsed = millis() - waitStart;

  if (!client.available()) {
    client.stop();
    bool connClosed = !client.connected();
    if (connClosed) {
      uploadLastResult = "WDGW: conn closed (" + String(elapsed) + "ms)";
      Serial.printf("[WDGWars] Server closed connection after %lums (no data)\n", (unsigned long)elapsed);
    } else {
      uploadLastResult = "WDGW: timeout (60s)";
      Serial.printf("[WDGWars] No response after %lums — server may be waiting for more body\n", (unsigned long)elapsed);
    }
    return false;
  }

  String status = client.readStringUntil('\n');
  status.trim();

  int code = 0;
  if (status.startsWith("HTTP/")) {
    int sp1 = status.indexOf(' ');
    if (sp1 > 0) {
      int sp2 = status.indexOf(' ', sp1 + 1);
      if (sp2 > sp1) code = status.substring(sp1 + 1, sp2).toInt();
    }
  }

  // Drain HTTP headers until blank line, then read response body
  bool inBody = false;
  String body = "";
  while (client.connected() || client.available()) {
    String line = client.readStringUntil('\n');
    line.trim();
    if (!inBody) {
      if (line.length() == 0) inBody = true;  // blank line = end of headers
    } else {
      body += line;
      if (body.length() > 512) break;         // guard against runaway body
    }
  }
  client.stop();

  Serial.printf("[WDGWars] HTTP %d  body: %s\n", code, body.substring(0, 100).c_str());

  // V2 API: HTTP 202 Accepted — file received and queued for processing.
  // Report success immediately instead of blocking here for up to 45 s;
  // the job result is checked later in the background (see
  // wdgwarsServicePendingJobs() / wdgwarsDrainPendingJobs() below).
  if (code == 202) {
    // Extract job_id from {"ok":true,"job_id":42,...}
    int jobId = 0;
    int ji = body.indexOf("\"job_id\":");
    if (ji >= 0) jobId = body.substring(ji + 9).toInt();
    if (jobId <= 0) {
      uploadLastResult = "WDGW: no job_id in 202";
      Serial.println("[WDGWars] 202 but no job_id in response");
      return false;
    }

    uint32_t nowMs = millis();
    wdgwarsPendingJobs.push_back({ jobId, filename, nowMs, nowMs, WDGWARS_JOB_CHECK_INTERVAL_MS });
    uploadLastResult = "WDGW: queued (job " + String(jobId) + ")";
    Serial.printf("[WDGWars] Job %d submitted for %s — result will be checked in background\n",
                  jobId, filename.c_str());
    return true;
  }

  uploadLastResult = "WDGW fail (" + String(code) + ")";
  Serial.printf("[WDGWars] Upload FAILED HTTP %d: %s\n", code, body.c_str());
  return false;
}

// ---- Wardrift API key test ----
// POSTs a minimal, 0-data-row WiGLE CSV (just the 2 header lines) to
// POST /v1/ingest/signals with X-API-Key -- the same endpoint/auth used for
// real uploads. There's no dedicated key-check endpoint, so this doubles as
// a safe live test. Per the Wardrift API docs, "a CSV file with no supported
// observations fails" -- so a valid key with this empty payload is expected
// to come back as either 200 (accepted, 0 rows) or 400 (rejected for having
// no observations, which only happens *after* auth succeeds). An invalid key
// 401s/403s before the body is ever parsed.
bool wardriftTestKey() {
  uploadLastResult = "";

  if (WiFi.status() != WL_CONNECTED) {
    uploadLastResult = "No STA WiFi";
    return false;
  }
  if (cfg.wardriftApiKey.length() < 8) {
    uploadLastResult = "No API key set";
    return false;
  }

  String body =
    String("WigleWifi-1.6,appRelease=") + FIRMWARE_VERSION +
    ",model=Piglet-Test,release=1,device=Piglet-Test,display=SSD1306-128x64,"
    "board=Piglet-Test,brand=Piglet,star=Sol,body=3,subBody=0\r\n"
    "MAC,SSID,AuthMode,FirstSeen,Channel,Frequency,RSSI,CurrentLatitude,"
    "CurrentLongitude,AltitudeMeters,AccuracyMeters,RCOIs,MfgrId,Type\r\n";

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(15000);

  // Retry TLS connect -- see the matching comment in wigleTestToken() for
  // why a single connect() attempt here can fail transiently.
  bool connected = false;
  for (int attempt = 1; attempt <= 3; attempt++) {
    if (client.connect(WARDRIFT_HOST, WARDRIFT_PORT)) { connected = true; break; }
    char errBuf[100];
    int sslErr = client.lastError(errBuf, sizeof(errBuf));
    Serial.printf("[Wardrift] Test key TLS connect failed (attempt %d/3): error %d: %s\n", attempt, sslErr, errBuf);
    logDmaHeap("Wardrift test key connect fail");
    client.stop();
    delay(500);
    yield();
  }
  if (!connected) {
    uploadLastResult = "TLS connect fail";
    return false;
  }

  client.print("POST /v1/ingest/signals HTTP/1.0\r\n");
  client.print(String("Host: ") + WARDRIFT_HOST + "\r\n");
  client.print(String("X-API-Key: ") + cfg.wardriftApiKey + "\r\n");
  client.print("Content-Type: text/csv\r\n");
  client.print("X-File-Name: test.wiglecsv\r\n");
  client.print(String("Content-Length: ") + String(body.length()) + "\r\n");
  client.print("Connection: close\r\n\r\n");
  client.print(body);

  uint32_t waitStart = millis();
  while (!client.available() && client.connected() && (millis() - waitStart) < 15000) {
    delay(10);
    yield();
  }

  String status = client.readStringUntil('\n');
  status.trim();

  int code = 0;
  if (status.startsWith("HTTP/")) {
    int sp1 = status.indexOf(' ');
    if (sp1 > 0) {
      int sp2 = status.indexOf(' ', sp1 + 1);
      if (sp2 > sp1) code = status.substring(sp1 + 1, sp2).toInt();
    }
  }

  String respBody = "";
  bool inBody = false;
  while (client.connected() || client.available()) {
    String line = client.readStringUntil('\n');
    line.trim();
    if (!inBody) {
      if (line.length() == 0) inBody = true;
    } else {
      respBody += line;
      if (respBody.length() > 512) break;
    }
  }
  client.stop();

  Serial.printf("[Wardrift] /v1/ingest/signals (test) HTTP %d  body: %s\n", code,
                respBody.substring(0, 160).c_str());

  if (code == 200) {
    uploadLastResult = "API key valid (200)";
    return true;
  }

  // A 400 here means the server parsed far enough to reject the payload for
  // having no supported observations -- which only happens once auth passes.
  if (code == 400) {
    uploadLastResult = "API key valid (400 -- empty test payload rejected, as expected)";
    return true;
  }

  if (code == 401 || code == 403) {
    uploadLastResult = "API key invalid (" + String(code) + ")";
  } else if (code == 0) {
    uploadLastResult = "No response from server";
  } else {
    uploadLastResult = "Test inconclusive (" + String(code) + ")";
  }
  return false;
}

// ---- Wardrift single-file upload ----
// Auth: X-API-Key header  |  Endpoint: POST /v1/ingest/signals
// The CSV file is uploaded as-is -- raw body, Content-Type: text/csv, plus
// an X-File-Name header -- no multipart wrapper and no per-row parsing
// needed. This endpoint caps uploaded/decompressed files at 10 MiB
// (10,485,760 bytes); a duplicate file comes back as HTTP 200 with
// "duplicate":true in the body, not a 409 -- a 409 here means a real
// idempotency/sequence conflict and must NOT be treated as success.

bool uploadFileToWardrift(const String& path) {
  Serial.print("[Wardrift] uploadFileToWardrift: ");
  Serial.println(path);

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[Wardrift] Not connected to STA WiFi");
    uploadLastResult = "No STA WiFi";
    return false;
  }
  if (cfg.wardriftApiKey.length() < 8) {
    Serial.println("[Wardrift] No API key set");
    uploadLastResult = "No API key set";
    return false;
  }
  if (!SD.exists(path)) {
    Serial.println("[Wardrift] File does not exist -- attempting SD recovery");
    if (!sdTryRecover() || !SD.exists(path)) {
      Serial.println("[Wardrift] File does not exist");
      uploadLastResult = "File missing";
      return false;
    }
    Serial.println("[Wardrift] SD recovered -- file found");
  }

  File f = SD.open(path, FILE_READ);
  if (!f) {
    Serial.println("[Wardrift] Failed to open file -- attempting SD recovery");
    if (sdTryRecover()) f = SD.open(path, FILE_READ);
  }
  if (!f) {
    Serial.println("[Wardrift] Failed to open file");
    uploadLastResult = "Open fail";
    return false;
  }

  String filename = pathBasename(path);
  uint32_t fileSize = f.size();
  Serial.printf("[Wardrift] File size: %.2f MB (%u bytes)\n", fileSize / (1024.0 * 1024.0), (unsigned)fileSize);

  // Wardrift caps this endpoint's uploaded/decompressed file size at 10 MiB
  // (10,485,760 bytes). Fail fast with a clear reason instead of wasting a
  // TLS round-trip on a guaranteed 413 -- other services (WiGLE/WDGoWars)
  // have higher limits and can still take this file.
  static const uint32_t WARDRIFT_MAX_FILE_BYTES = 10485760UL;
  if (fileSize > WARDRIFT_MAX_FILE_BYTES) {
    f.close();
    uploadLastResult = "File too large for Wardrift (>10 MiB)";
    Serial.printf("[Wardrift] File exceeds 10 MiB limit (%u bytes) -- skipping\n", (unsigned)fileSize);
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(25000);
  tlsMaybeSetBufferSizes(client, 512, 512);

  bool connected = false;
  for (int attempt = 1; attempt <= 3; attempt++) {
    Serial.printf("[Wardrift] Free heap before connect (attempt %d/3): %u bytes\n",
                  attempt, (unsigned)ESP.getFreeHeap());
    if (client.connect(WARDRIFT_HOST, WARDRIFT_PORT)) {
      connected = true;
      break;
    }
    Serial.printf("[Wardrift] TLS connect failed (attempt %d/3)\n", attempt);
    client.stop();
    delay(500);
    yield();
  }

  if (!connected) {
    uploadLastResult = "TLS connect fail";
    Serial.println("[Wardrift] TLS connect failed after 3 attempts");
    f.close();
    return false;
  }

  // A `.gz`-suffixed path means this is a scratch file already
  // gzip-compressed by GzipDeflate.cpp -- signal that with the standard
  // Content-Encoding header, same rationale as uploadFileToWdgwars() above.
  bool isGzip = path.endsWith(".gz");
  client.print("POST /v1/ingest/signals HTTP/1.0\r\n");
  client.print(String("Host: ") + WARDRIFT_HOST + "\r\n");
  client.print(String("X-API-Key: ") + cfg.wardriftApiKey + "\r\n");
  client.print("Content-Type: text/csv\r\n");
  if (isGzip) client.print("Content-Encoding: gzip\r\n");
  client.print(String("X-File-Name: ") + filename + "\r\n");
  client.print(String("Content-Length: ") + String(fileSize) + "\r\n");
  client.print("Connection: close\r\n\r\n");

  // Stream the raw file bytes directly as the body -- no multipart wrapper.
  uint32_t fileSent = 0;
  uint8_t buf[512];
  bool writeError = false;
  while (!writeError) {
    int n = f.read(buf, sizeof(buf));
    if (n <= 0) break;
    int offset = 0;
    while (offset < n) {
      int w = client.write(buf + offset, n - offset);
      if (w <= 0) { writeError = true; break; }
      offset += w;
      fileSent += (uint32_t)w;
    }
    yield();
  }
  f.close();
  client.flush();

  Serial.printf("[Wardrift] ContentLen=%u  sent=%u  (%s)\n",
                fileSize, fileSent,
                (fileSent == fileSize) ? "MATCH" : "*** MISMATCH ***");
  if (writeError || fileSent != fileSize) {
    client.stop();
    uploadLastResult = "Write error (body short)";
    Serial.println("[Wardrift] Body write incomplete — aborting");
    return false;
  }

  uint32_t waitStart = millis();
  while (!client.available() && client.connected() && (millis() - waitStart) < 60000) {
    delay(100);
    yield();
  }
  uint32_t elapsed = millis() - waitStart;

  if (!client.available()) {
    client.stop();
    bool connClosed = !client.connected();
    uploadLastResult = connClosed
      ? "Conn closed (" + String(elapsed) + "ms)"
      : "Timeout (60s)";
    Serial.printf("[Wardrift] No response after %lums%s\n", (unsigned long)elapsed,
                  connClosed ? " (server closed connection)" : "");
    return false;
  }

  String status = client.readStringUntil('\n');
  status.trim();

  int code = 0;
  if (status.startsWith("HTTP/")) {
    int sp1 = status.indexOf(' ');
    if (sp1 > 0) {
      int sp2 = status.indexOf(' ', sp1 + 1);
      if (sp2 > sp1) code = status.substring(sp1 + 1, sp2).toInt();
    }
  }

  bool inBody = false;
  String body = "";
  while (client.connected() || client.available()) {
    String line = client.readStringUntil('\n');
    line.trim();
    if (!inBody) {
      if (line.length() == 0) inBody = true;
    } else {
      body += line;
      if (body.length() > 512) break;
    }
  }
  client.stop();

  Serial.printf("[Wardrift] HTTP %d  body: %s\n", code, body.substring(0, 160).c_str());

  if (code == 200) {
    // Synchronous responses use "item_count" (not "accepted_signals", which
    // only appears in the async queued-upload /v1/wardrive/logs response).
    bool isDuplicate = body.indexOf("\"duplicate\":true") >= 0;
    int icIdx = body.indexOf("\"item_count\":");
    String countStr = "";
    if (icIdx >= 0) {
      int start = icIdx + 13;
      int end = start;
      while (end < (int)body.length() && isDigit(body[end])) end++;
      countStr = body.substring(start, end);
    }
    if (isDuplicate) {
      uploadLastResult = "Already uploaded (duplicate)";
    } else {
      uploadLastResult = countStr.length() ? "Uploaded OK (" + countStr + " signals)" : "Uploaded OK (200)";
    }
    return true;
  }

  // Pull a "message" field out of the error body if present, for a clearer reason
  String msg = "";
  int mIdx = body.indexOf("\"message\":");
  if (mIdx >= 0) {
    int q1 = body.indexOf('"', mIdx + 11);
    int q2 = (q1 >= 0) ? body.indexOf('"', q1 + 1) : -1;
    if (q1 >= 0 && q2 > q1) msg = body.substring(q1 + 1, q2);
  }
  uploadLastResult = msg.length() ? ("Upload failed (" + String(code) + "): " + msg) : ("Upload failed (" + String(code) + ")");
  Serial.printf("[Wardrift] Upload FAILED HTTP %d: %s\n", code, body.c_str());
  return false;
}

// ---- WDGoWars background job status check ----
// Does the actual work of checking a single pending job: one short-lived
// HTTPS GET, no retries. Removes the job from the queue once it resolves
// (done/failed) or once it has been pending longer than
// WDGWARS_JOB_MAX_AGE_MS. Returns true if it made a network attempt (or
// gave up on an aged-out job), so callers can pace successive checks.
static bool wdgwarsCheckOnePendingJob() {
  if (wdgwarsPendingJobs.empty()) return false;
  if (WiFi.status() != WL_CONNECTED) return false;

  uint32_t nowMs = millis();

  // Find the least-recently-checked job that's due for a check, using each
  // job's own (growing) backoff interval rather than a fixed one.
  int idx = -1;
  for (size_t i = 0; i < wdgwarsPendingJobs.size(); i++) {
    if (nowMs - wdgwarsPendingJobs[i].lastCheckMs >= wdgwarsPendingJobs[i].nextDelayMs) {
      idx = (int)i;
      break;
    }
  }
  if (idx < 0) return false;

  WdgwarsPendingJob job = wdgwarsPendingJobs[idx];

  // Give up on jobs that have been pending too long
  if (nowMs - job.queuedAtMs > WDGWARS_JOB_MAX_AGE_MS) {
    Serial.printf("[WDGWars] Job %d (%s): giving up after %lus with no result\n",
                  job.jobId, job.filename.c_str(),
                  (unsigned long)(WDGWARS_JOB_MAX_AGE_MS / 1000));
    wdgwarsPendingJobs.erase(wdgwarsPendingJobs.begin() + idx);
    return true;
  }

  wdgwarsPendingJobs[idx].lastCheckMs = nowMs;
  // Double the wait before the next check on this job (capped), whether
  // this attempt connects or not -- a resolving check erases the entry
  // anyway, so only the "still processing"/connect-fail paths ever see the
  // larger gap take effect.
  wdgwarsPendingJobs[idx].nextDelayMs =
    (job.nextDelayMs > WDGWARS_JOB_CHECK_MAX_MS / 2) ? WDGWARS_JOB_CHECK_MAX_MS : job.nextDelayMs * 2;

  WiFiClientSecure pc;
  pc.setInsecure();
  pc.setTimeout(8000);
  if (!pc.connect(WDGWARS_HOST, WDGWARS_PORT)) {
    logDmaHeap("WDGWars job-poll connect fail");
    return true;
  }

  String jobPath = String("/api/v2/upload-job/") + String(job.jobId);
  pc.print("GET " + jobPath + " HTTP/1.0\r\n");
  pc.print(String("Host: ") + WDGWARS_HOST + "\r\n");
  pc.print(String("X-API-Key: ") + cfg.wdgwarsApiKey + "\r\n");
  pc.print("Connection: close\r\n\r\n");

  uint32_t pw = millis();
  while (!pc.available() && pc.connected() && (millis() - pw) < 6000) { delay(50); yield(); }
  if (!pc.available()) { pc.stop(); return true; }

  pc.readStringUntil('\n');  // skip HTTP status line
  String pb = ""; bool pInBody = false;
  while (pc.connected() || pc.available()) {
    String ln = pc.readStringUntil('\n'); ln.trim();
    if (!pInBody) { if (ln.length() == 0) pInBody = true; }
    else { pb += ln; if (pb.length() > 512) break; }
  }
  pc.stop();

  if (pb.indexOf("\"done\"") >= 0) {
    int imp = 0;
    int ii = pb.indexOf("\"imported\":");
    if (ii >= 0) imp = pb.substring(ii + 11).toInt();
    Serial.printf("[WDGWars] Job %d (%s) done — imported: %d\n",
                  job.jobId, job.filename.c_str(), imp);
    wdgwarsPendingJobs.erase(wdgwarsPendingJobs.begin() + idx);
  } else if (pb.indexOf("\"failed\"") >= 0) {
    Serial.printf("[WDGWars] Job %d (%s) FAILED server-side\n",
                  job.jobId, job.filename.c_str());
    wdgwarsPendingJobs.erase(wdgwarsPendingJobs.begin() + idx);
  }
  // else still "processing" — leave queued, checked again later

  return true;
}

// Call periodically from loop(). Internally rate-limited so it never does
// more than one short network round-trip per call.
void wdgwarsServicePendingJobs() {
  static uint32_t lastRunMs = 0;
  uint32_t nowMs = millis();
  if (nowMs - lastRunMs < WDGWARS_JOB_CHECK_INTERVAL_MS) return;
  lastRunMs = nowMs;
  wdgwarsCheckOnePendingJob();
}

// Best-effort: keep checking pending jobs back-to-back for up to budgetMs.
// Intended to be called right before intentionally dropping the STA
// connection (e.g. autoStartAfterUpload or entering mesh Core mode) so
// quick jobs still get confirmed instead of being silently abandoned.
void wdgwarsDrainPendingJobs(uint32_t budgetMs) {
  if (wdgwarsPendingJobs.empty()) return;

  // Force every queued job to be immediately eligible for a check.
  for (auto& j : wdgwarsPendingJobs) j.lastCheckMs = 0;

  uint32_t start = millis();
  while (!wdgwarsPendingJobs.empty() && (millis() - start) < budgetMs) {
    if (!wdgwarsCheckOnePendingJob()) break;  // WiFi down or nothing due yet
    delay(200);
    yield();
  }

  if (!wdgwarsPendingJobs.empty()) {
    Serial.printf("[WDGWars] %u job(s) still pending — will not be confirmed (STA disconnecting)\n",
                  (unsigned)wdgwarsPendingJobs.size());
  }
}

// ---- Empty-file guard ----
// Returns true if the CSV contains at least one data row beyond the two
// mandatory WiGLE header lines. Opens, reads two lines, checks for more.
// `outSize` (optional) receives the file's byte size for diagnostic logging.
//
// IMPORTANT: if the file can't even be opened, this returns true ("assume it
// has data") rather than false. Callers only delete a file when this returns
// false, so a transient SD/SPI error must never look identical to "verified
// empty" -- that conflation previously caused real, non-empty files to be
// deleted en masse whenever the SD card hit a rough patch mid-session.
static bool csvHasDataRows(const String& path, uint32_t* outSize = nullptr) {
  if (outSize) *outSize = 0;

  File f = SD.open(path, FILE_READ);
  if (!f) {
    Serial.printf("[SD] csvHasDataRows: could not open %s -- attempting SD recovery\n",
                  pathBasename(path).c_str());
    if (sdTryRecover()) f = SD.open(path, FILE_READ);
  }
  if (!f) {
    Serial.printf("[SD] csvHasDataRows: could not open %s -- assuming it HAS data (not deleting)\n",
                  pathBasename(path).c_str());
    return true;
  }

  if (outSize) *outSize = f.size();

  // Skip the two header lines
  for (int i = 0; i < 2; i++) {
    if (!f.available()) { f.close(); return false; }
    f.readStringUntil('\n');
  }

  // If anything remains after the headers, there is at least one data row
  bool hasData = f.available() > 0;
  f.close();
  return hasData;
}

// ---- No-GPS-fix guard ----
// Returns true only if the file has at least one data row AND every one of
// those rows has lat=lon=0.000000 -- "null island", the exact placeholder
// appendWigleRow() writes when GPS has never had a fix this session (see
// Scanner.cpp's processScanResults()). A file like this is either a bench/
// indoor test capture or a session where GPS never locked, and is exactly
// the shape Wardrift's "no supported signal rows" validator rejects (it
// treats 0,0 as no-fix, not a real location) -- confirmed against real
// uploads where every row sent was rejected this way. Scans only as far as
// the first non-zero coordinate before stopping, so a large real-data file
// is cheap to clear.
//
// Relies on the row format appendWigleRow() writes: field 0 is the
// unquoted MAC, field 1 is the double-quoted SSID (the only quoted field in
// a WiGLE data row, per RFC4180), and everything after it --
// AuthMode,FirstSeen,Channel,Frequency,RSSI,Lat,Lon,... -- is plain,
// unquoted CSV. Lat/Lon are always formatted with exactly 6 decimal places
// ("%.6f"), so a genuine no-fix row is always the literal string
// "0.000000", not just numerically close to zero.
static bool csvAllRowsZeroGps(const String& path) {
  File f = SD.open(path, FILE_READ);
  if (!f) return false;  // can't verify -- never delete on an unknown

  // Skip the two WiGLE header lines.
  for (int i = 0; i < 2; i++) {
    if (!f.available()) { f.close(); return false; }  // no data rows -- csvHasDataRows() handles this
    f.readStringUntil('\n');
  }

  bool sawAnyRow = false;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;

    int c1 = line.indexOf(',');            // end of the unquoted MAC field
    if (c1 < 0) continue;                  // malformed row -- ignore, don't let it block deletion
    int i = c1 + 1;
    int n = line.length();
    if (i >= n || line[i] != '"') continue;  // expected the quoted SSID here -- malformed, skip

    // Walk to the SSID's closing quote, honoring doubled """ "" escaping.
    i++;
    while (i < n) {
      if (line[i] == '"') {
        if (i + 1 < n && line[i + 1] == '"') { i += 2; continue; }
        i++;
        break;
      }
      i++;
    }
    if (i >= n || line[i] != ',') continue;  // malformed -- skip

    // Remainder is plain CSV: AuthMode,FirstSeen,Channel,Frequency,RSSI,Lat,Lon,...
    String rest = line.substring(i + 1);
    int pos = 0, field = 0;
    String lat, lon;
    while (field <= 6) {
      int comma = rest.indexOf(',', pos);
      String val = (comma < 0) ? rest.substring(pos) : rest.substring(pos, comma);
      if (field == 5) lat = val;
      if (field == 6) lon = val;
      if (comma < 0) break;
      pos = comma + 1;
      field++;
    }

    sawAnyRow = true;
    if (lat != "0.000000" || lon != "0.000000") {
      f.close();
      return false;  // found a real fix -- not a no-GPS file
    }
  }
  f.close();

  return sawAnyRow;
}

// ---- Standalone empty-CSV cleanup ----
// Scans /logs and deletes any CSV that has no data rows (header-only).
// Called at boot and available via /cleanup endpoint.
void deleteEmptyCsvs() {
  if (!sdOk) return;
  File root = SD.open("/logs");
  if (!root) return;

  std::vector<String> toDelete;
  File f = root.openNextFile();
  while (f) {
    f.setBufferSize(64);  // avoid the 4KB default stdio buffer -- see WebUI.cpp's addDirFiles()
    String path = normalizeSdPath("/logs", f.name());
    bool isCsv    = path.endsWith(".csv");
    bool isCurrent = (currentCsvPath.length() && path == currentCsvPath);
    f.close();
    if (isCsv && !isCurrent) toDelete.push_back(path);
    f = root.openNextFile();
  }
  root.close();

  for (const String& path : toDelete) {
    uint32_t sz = 0;
    if (!csvHasDataRows(path, &sz)) {
      Serial.printf("[CLEANUP] Empty CSV (%u bytes), deleting: %s\n", (unsigned)sz, pathBasename(path).c_str());
      if (!SD.remove(path)) {
        Serial.printf("[CLEANUP] WARNING: SD.remove failed for %s -- file left in place\n", pathBasename(path).c_str());
      }
    }
  }
}

// ---- Shared file collection for batch uploads ----
// Enumerates /logs/*.csv (excluding the active log file and the hidden
// gzip-scratch/ledger housekeeping files, which never match ".csv"
// anyway), deleting header-only files along the way. Returns the remaining
// eligible paths, capped to maxFiles if positive.
static std::vector<String> collectEligibleCsvs(int maxFiles) {
  std::vector<String> all;
  File root = SD.open("/logs");
  if (root) {
    File f = root.openNextFile();
    while (f) {
      f.setBufferSize(64);  // avoid the 4KB default stdio buffer -- see WebUI.cpp's addDirFiles()
      String name = normalizeSdPath("/logs", f.name());
      // Explicitly skip dot-prefixed housekeeping entries (the .gzscratch
      // scratch directory, the .upload_state.cfg ledger) -- redundant with
      // the .csv suffix check below today, but makes the exclusion explicit
      // and keeps it safe if either ever changes shape.
      bool isHidden = isHiddenSdEntry(pathBasename(name));
      bool isCsv = name.endsWith(".csv");
      bool isCurrent = (currentCsvPath.length() && name == currentCsvPath);
      f.close();
      if (isCsv && !isHidden && !isCurrent) all.push_back(name);
      f = root.openNextFile();
    }
    root.close();
  }

  std::vector<String> eligible;
  eligible.reserve(all.size());
  for (const String& p : all) {
    uint32_t emptySz = 0;
    if (!csvHasDataRows(p, &emptySz)) {
      Serial.printf("[UPLOAD] Empty CSV (%u bytes, no data rows), deleting: %s\n",
                    (unsigned)emptySz, pathBasename(p).c_str());
      if (!SD.remove(p)) {
        Serial.printf("[UPLOAD] WARNING: SD.remove failed for %s -- file left in place\n", pathBasename(p).c_str());
      }
      clearUploadState(pathBasename(p));
      continue;
    }
    if (csvAllRowsZeroGps(p)) {
      Serial.printf("[UPLOAD] No GPS fix in any row -- deleting bench-test/no-lock capture: %s\n",
                    pathBasename(p).c_str());
      if (!SD.remove(p)) {
        Serial.printf("[UPLOAD] WARNING: SD.remove failed for %s -- file left in place\n", pathBasename(p).c_str());
      }
      clearUploadState(pathBasename(p));
      continue;
    }
    eligible.push_back(p);
  }

  if (maxFiles > 0 && (uint32_t)maxFiles < eligible.size()) {
    Serial.printf("[UPLOAD] Limiting this round to %d of %u eligible files (maxBootUploads)\n",
                  maxFiles, (unsigned)eligible.size());
    eligible.resize((size_t)maxFiles);
  }
  return eligible;
}

// ---- Combined batch upload (service-major, with per-file/per-service retry tracking) ----
// Uploads every pending file to WiGLE first (as one merged, gzip-compressed
// archive when more than one file is pending -- WiGLE's upload API can take
// many files' worth of rows in a single call), then every file still
// pending to WDGoWars (gzip per file -- it cannot accept an archive), then
// every file still pending to Wardrift (same, per file). A file only moves
// to /uploaded/ once every *configured* service has succeeded for it (see
// UploadState.h); a failure against one service never blocks or re-triggers
// the others for that file -- it just stays in /logs, retried next time
// only against whichever service(s) are still pending.
uint32_t uploadAllPending(int maxFiles) {
  if (!sdOk) { uploadLastResult = "SD not OK"; return 0; }

  uploadPausedScanWasEnabled = scanningEnabled;
  scanningEnabled = false;

  uploading = true;
  uploadDoneFiles = 0;
  uploadFailedFiles = 0;
  uploadTotalFiles = 0;
  uploadCurrentFile = "";
  updateOLED(0);

  std::vector<String> paths = collectEligibleCsvs(maxFiles);

  if (paths.empty()) {
    uploading = false;
    scanningEnabled = uploadPausedScanWasEnabled;
    uploadLastResult = "No CSVs to upload";
    return 0;
  }

  ensureGzScratchDir();

  std::vector<FileUploadState> states(paths.size());
  for (size_t i = 0; i < paths.size(); i++) {
    states[i] = loadUploadState(pathBasename(paths[i]));
  }

  // Progress (OLED/web UI) tracks individual upload ATTEMPTS across all three
  // service phases, not "every configured service done for this file" -- a
  // file can succeed WiGLE but still be mid-flight on WDGoWars/Wardrift, and
  // the operator should see the bar/counter visibly tick forward on every
  // attempt (success or fail), matching what the serial log shows, instead
  // of sitting still until a file clears every service.
  for (size_t i = 0; i < paths.size(); i++) {
    if (!states[i].wigleDone)    uploadTotalFiles++;
    if (!states[i].wdgwDone)     uploadTotalFiles++;
    if (!states[i].wardriftDone) uploadTotalFiles++;
  }
  auto touchFile = [&](size_t /*idx*/) { uploadDoneFiles++; };

  // ---- Phase 1: WiGLE ----
  if (cfg.wigleBasicToken.length() >= 8) {
    std::vector<size_t> pendingIdx;
    for (size_t i = 0; i < paths.size(); i++) if (!states[i].wigleDone) pendingIdx.push_back(i);

    if (!pendingIdx.empty()) {
      uploadTargetName = "WiGLE UL";

      if (cfg.gzipUploads && pendingIdx.size() > 1) {
        // One merged + gzip-compressed archive covering every pending file.
        uploadCurrentFile = "(" + String(pendingIdx.size()) + " files)";
        updateOLED(0);
        std::vector<String> srcPaths;
        for (size_t idx : pendingIdx) srcPaths.push_back(paths[idx]);
        String scratch = gzScratchPathFor("wigle_batch");
        bool ok = gzipCompressMergedFilesToFile(srcPaths, scratch) && uploadFileToWigle(scratch);
        SD.remove(scratch);
        if (ok) {
          Serial.printf("[WiGLE] Merged archive OK -- %u file(s) marked done\n", (unsigned)pendingIdx.size());
        } else {
          Serial.printf("[WiGLE] Merged archive FAILED -- %u file(s) remain pending\n", (unsigned)pendingIdx.size());
          uploadFailedFiles++;
        }
        for (size_t idx : pendingIdx) {
          if (ok) { markServiceDone(pathBasename(paths[idx]), UploadService::Wigle); states[idx].wigleDone = true; }
          touchFile(idx);
        }
      } else {
        // Single pending file, or gzip disabled (can't merge multiple files
        // into one request without compression) -- send one at a time.
        for (size_t n = 0; n < pendingIdx.size(); n++) {
          size_t idx = pendingIdx[n];
          uploadCurrentFile = paths[idx];
          updateOLED(0);
          bool ok;
          if (cfg.gzipUploads) {
            String scratch = gzScratchPathFor(pathBasename(paths[idx]));
            ok = gzipCompressFileToFile(paths[idx], scratch) && uploadFileToWigle(scratch);
            SD.remove(scratch);
          } else {
            ok = uploadFileToWigle(paths[idx]);
          }
          if (ok) { markServiceDone(pathBasename(paths[idx]), UploadService::Wigle); states[idx].wigleDone = true; }
          else uploadFailedFiles++;
          touchFile(idx);
          if (n + 1 < pendingIdx.size()) delay(1500);
        }
      }
    }
  }

  // ---- Phase 2: WDGoWars (per file -- cannot accept an archive) ----
  if (cfg.wdgwarsApiKey.length() >= 8) {
    for (size_t i = 0; i < paths.size(); i++) {
      if (states[i].wdgwDone) continue;
      uploadTargetName = "WDGW UL";
      uploadCurrentFile = paths[i];
      updateOLED(0);
      Serial.printf("[WDGWars] %s (heap free: %u bytes)\n", pathBasename(paths[i]).c_str(), (unsigned)ESP.getFreeHeap());

      bool ok;
      if (cfg.gzipUploads) {
        String scratch = gzScratchPathFor(pathBasename(paths[i]));
        ok = gzipCompressFileToFile(paths[i], scratch) && uploadFileToWdgwars(scratch);
        SD.remove(scratch);
      } else {
        ok = uploadFileToWdgwars(paths[i]);
      }
      if (ok) { markServiceDone(pathBasename(paths[i]), UploadService::Wdgw); states[i].wdgwDone = true; }
      else uploadFailedFiles++;
      touchFile(i);
      updateOLED(0);

      bool morePending = (i + 1 < paths.size());
      if (morePending) delay(1500);
    }
  }

  // ---- Phase 3: Wardrift (per file -- cannot accept an archive) ----
  if (cfg.wardriftApiKey.length() >= 8) {
    for (size_t i = 0; i < paths.size(); i++) {
      if (states[i].wardriftDone) continue;
      uploadTargetName = "Wardrift UL";
      uploadCurrentFile = paths[i];
      updateOLED(0);
      Serial.printf("[Wardrift] %s (heap free: %u bytes)\n", pathBasename(paths[i]).c_str(), (unsigned)ESP.getFreeHeap());

      // Always sent uncompressed -- Wardrift's /v1/ingest/signals endpoint
      // does not honor Content-Encoding: gzip (confirmed in the field: every
      // gzip-compressed upload came back HTTP 400 "file did not contain
      // supported signal rows" -- it was trying to parse the raw gzip bytes
      // as CSV text -- even though the identical file succeeded against
      // WDGoWars, which does support gzip). Ignore cfg.gzipUploads here.
      bool ok = uploadFileToWardrift(paths[i]);
      if (ok) { markServiceDone(pathBasename(paths[i]), UploadService::Wardrift); states[i].wardriftDone = true; }
      else uploadFailedFiles++;
      touchFile(i);
      updateOLED(0);

      bool morePending = (i + 1 < paths.size());
      if (morePending) delay(1500);
    }
  }

  // ---- Finalize: move fully-done files to /uploaded/ ----
  uint32_t okCount = 0;
  for (size_t i = 0; i < paths.size(); i++) {
    if (allConfiguredServicesDone(states[i])) {
      okCount++;
      // Only clear the ledger entry once the move actually succeeds. Every
      // configured service already succeeded for this file at this point --
      // clearing the ledger unconditionally here (even when moveToUploaded()
      // itself failed, e.g. a transient SD hiccup) made that look identical
      // to "never uploaded" on the next run, which re-sent it to every
      // service all over again. Leaving the entry intact just retries the
      // move next time, with no re-upload.
      if (moveToUploaded(paths[i])) {
        clearUploadState(pathBasename(paths[i]));
      } else {
        Serial.printf("[UPLOAD] WARNING: moveToUploaded failed for %s -- file stays in /logs, ledger kept so it won't be re-uploaded\n",
                      pathBasename(paths[i]).c_str());
      }
    }
  }

  uploading = false;
  uploadTargetName = "";
  scanningEnabled = uploadPausedScanWasEnabled;
  uploadCurrentFile = "";
  uploadDoneFiles = uploadTotalFiles;  // guarantee the OLED/UI see a finished progress bar
  updateOLED(0);

  // uploadTotalFiles is the attempts-based progress denominator above, not
  // the distinct file count -- use paths.size() here so this summary reads
  // naturally ("Uploaded 23/25" rather than counting every per-service try).
  uploadLastResult = "Uploaded " + String(okCount) + "/" + String(paths.size());
  return okCount;
}

// ---- WDGoWars-only batch upload (called from web UI button) ----
// Uploads every file still pending for WDGoWars (gzip per file, same as
// the combined orchestrator's Phase 2), then moves any file that now has
// every *configured* service done -- not just WDGoWars -- to /uploaded/.

uint32_t uploadAllCsvsToWdgwars(int maxFiles) {
  if (!sdOk) { uploadLastResult = "SD not OK"; return 0; }
  if (cfg.wdgwarsApiKey.length() < 8) { uploadLastResult = "No API key"; return 0; }

  uploadPausedScanWasEnabled = scanningEnabled;
  scanningEnabled = false;
  uploading = true;
  uploadTargetName = "WDGW UL";
  uploadDoneFiles = 0;
  uploadFailedFiles = 0;
  uploadTotalFiles = 0;
  uploadCurrentFile = "";
  updateOLED(0);

  std::vector<String> paths = collectEligibleCsvs(maxFiles);
  uploadTotalFiles = (uint32_t)paths.size();

  if (paths.empty()) {
    uploading = false; uploadTargetName = "";
    scanningEnabled = uploadPausedScanWasEnabled;
    uploadLastResult = "No CSVs to upload";
    return 0;
  }

  ensureGzScratchDir();

  uint32_t okCount = 0;
  for (size_t i = 0; i < paths.size(); i++) {
    uploadCurrentFile = paths[i];
    updateOLED(0);

    String basename = pathBasename(paths[i]);
    FileUploadState st = loadUploadState(basename);
    if (!st.wdgwDone) {
      Serial.printf("[UPLOAD] File %u/%u: %s (heap free: %u bytes)\n",
                    (unsigned)(i + 1), (unsigned)paths.size(), basename.c_str(), (unsigned)ESP.getFreeHeap());
      bool ok;
      if (cfg.gzipUploads) {
        String scratch = gzScratchPathFor(basename);
        ok = gzipCompressFileToFile(paths[i], scratch) && uploadFileToWdgwars(scratch);
        SD.remove(scratch);
      } else {
        ok = uploadFileToWdgwars(paths[i]);
      }
      if (ok) { markServiceDone(basename, UploadService::Wdgw); st.wdgwDone = true; }
      else uploadFailedFiles++;
    }

    if (allConfiguredServicesDone(st)) {
      okCount++;
      // See the matching comment in uploadAllPending()'s finalize loop --
      // only clear the ledger once the move actually succeeds.
      if (moveToUploaded(paths[i])) {
        clearUploadState(basename);
      } else {
        Serial.printf("[UPLOAD] WARNING: moveToUploaded failed for %s -- file stays in /logs, ledger kept\n", basename.c_str());
      }
    }
    uploadDoneFiles++;
    updateOLED(0);
    if (i < paths.size() - 1) delay(1500);
  }

  uploading = false;
  uploadTargetName = "";
  scanningEnabled = uploadPausedScanWasEnabled;
  uploadCurrentFile = "";
  updateOLED(0);

  uploadLastResult = "WDGWars: " + String(okCount) + "/" + String(uploadTotalFiles);
  return okCount;
}

// ---- Wardrift-only batch upload (called from web UI button) ----
// Same shape as uploadAllCsvsToWdgwars() above, for Wardrift.

uint32_t uploadAllCsvsToWardrift(int maxFiles) {
  if (!sdOk) { uploadLastResult = "SD not OK"; return 0; }
  if (cfg.wardriftApiKey.length() < 8) { uploadLastResult = "No token set"; return 0; }

  uploadPausedScanWasEnabled = scanningEnabled;
  scanningEnabled = false;
  uploading = true;
  uploadTargetName = "Wardrift UL";
  uploadDoneFiles = 0;
  uploadFailedFiles = 0;
  uploadTotalFiles = 0;
  uploadCurrentFile = "";
  updateOLED(0);

  std::vector<String> paths = collectEligibleCsvs(maxFiles);
  uploadTotalFiles = (uint32_t)paths.size();

  if (paths.empty()) {
    uploading = false; uploadTargetName = "";
    scanningEnabled = uploadPausedScanWasEnabled;
    uploadLastResult = "No CSVs to upload";
    return 0;
  }

  ensureGzScratchDir();

  uint32_t okCount = 0;
  for (size_t i = 0; i < paths.size(); i++) {
    uploadCurrentFile = paths[i];
    updateOLED(0);

    String basename = pathBasename(paths[i]);
    FileUploadState st = loadUploadState(basename);
    if (!st.wardriftDone) {
      Serial.printf("[UPLOAD] File %u/%u: %s (heap free: %u bytes)\n",
                    (unsigned)(i + 1), (unsigned)paths.size(), basename.c_str(), (unsigned)ESP.getFreeHeap());
      // Always sent uncompressed -- see the matching comment in
      // uploadAllPending()'s Phase 3 for why (Wardrift doesn't honor
      // Content-Encoding: gzip on this endpoint).
      bool ok = uploadFileToWardrift(paths[i]);
      if (ok) { markServiceDone(basename, UploadService::Wardrift); st.wardriftDone = true; }
      else uploadFailedFiles++;
    }

    if (allConfiguredServicesDone(st)) {
      okCount++;
      // See the matching comment in uploadAllPending()'s finalize loop --
      // only clear the ledger once the move actually succeeds.
      if (moveToUploaded(paths[i])) {
        clearUploadState(basename);
      } else {
        Serial.printf("[UPLOAD] WARNING: moveToUploaded failed for %s -- file stays in /logs, ledger kept\n", basename.c_str());
      }
    }
    uploadDoneFiles++;
    updateOLED(0);
    if (i < paths.size() - 1) delay(1500);
  }

  uploading = false;
  uploadTargetName = "";
  scanningEnabled = uploadPausedScanWasEnabled;
  uploadCurrentFile = "";
  updateOLED(0);

  uploadLastResult = "Wardrift: " + String(okCount) + "/" + String(uploadTotalFiles);
  return okCount;
}
