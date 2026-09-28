#include "WigleUpload.h"
#include "Globals.h"
#include "SDUtils.h"
#include "Display.h"
#include <WiFiClientSecure.h>
#include <vector>

static const char*     WDGWARS_HOST = "wdgwars.pl";
static const uint16_t  WDGWARS_PORT = 443;

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
};
static std::vector<WdgwarsPendingJob> wdgwarsPendingJobs;
static const uint32_t WDGWARS_JOB_CHECK_INTERVAL_MS = 4000;            // min gap between checks
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

    if (!client.connect(WIGLE_HOST, WIGLE_PORT)) {
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

  if (!client.connect(WDGWARS_HOST, WDGWARS_PORT)) {
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

  // HTTP request — WDGoWars V2 async endpoint
  client.print("POST /api/v2/upload-csv HTTP/1.0\r\n");
  client.print(String("Host: ") + WDGWARS_HOST + "\r\n");
  client.print(String("X-API-Key: ") + cfg.wdgwarsApiKey + "\r\n");
  client.print(String("Content-Type: multipart/form-data; boundary=") + boundary + "\r\n");
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
    wdgwarsPendingJobs.push_back({ jobId, filename, nowMs, nowMs });
    uploadLastResult = "WDGW: queued (job " + String(jobId) + ")";
    Serial.printf("[WDGWars] Job %d submitted for %s — result will be checked in background\n",
                  jobId, filename.c_str());
    return true;
  }

  uploadLastResult = "WDGW fail (" + String(code) + ")";
  Serial.printf("[WDGWars] Upload FAILED HTTP %d: %s\n", code, body.c_str());
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

  // Find the least-recently-checked job that's due for a check
  int idx = -1;
  for (size_t i = 0; i < wdgwarsPendingJobs.size(); i++) {
    if (nowMs - wdgwarsPendingJobs[i].lastCheckMs >= WDGWARS_JOB_CHECK_INTERVAL_MS) {
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

  WiFiClientSecure pc;
  pc.setInsecure();
  pc.setTimeout(8000);
  if (!pc.connect(WDGWARS_HOST, WDGWARS_PORT)) return true;

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

// ---- Batch upload ----

uint32_t uploadAllCsvsToWigle(int maxFiles) {
  if (!sdOk) { uploadLastResult = "SD not OK"; return 0; }

  // Pause scanning during upload to avoid SD contention + WiFi scan interference
  uploadPausedScanWasEnabled = scanningEnabled;
  scanningEnabled = false;

  uploading = true;
  uploadDoneFiles = 0;
  uploadFailedFiles = 0;
  uploadTotalFiles = 0;
  uploadCurrentFile = "";
  updateOLED(0);

  // First pass: count
  File root = SD.open("/logs");
  if (root) {
    File f = root.openNextFile();
    while (f) {
      String name = normalizeSdPath("/logs", f.name());
      bool isCsv = name.endsWith(".csv");
      bool isCurrent = (currentCsvPath.length() && name == currentCsvPath);
      if (isCsv && !isCurrent) uploadTotalFiles++;
      f.close();
      f = root.openNextFile();
    }
    root.close();
  }

  if (uploadTotalFiles == 0) {
    uploading = false;
    scanningEnabled = uploadPausedScanWasEnabled;
    uploadLastResult = "No CSVs to upload";
    return 0;
  }

  // Skip token pre-check to avoid 30-45s delay at start of batch upload.
  // If token is invalid, first upload will fail with 401/403.
  // Note: wigleTokenStatus may be stale; web UI still validates on demand.

  // Apply maxFiles limit if specified
  uint32_t filesToUpload = uploadTotalFiles;
  if (maxFiles > 0 && (uint32_t)maxFiles < uploadTotalFiles) {
    filesToUpload = (uint32_t)maxFiles;
    Serial.printf("[WiGLE] Limiting upload to %d of %d files (maxBootUploads)\n", 
                  filesToUpload, uploadTotalFiles);
  }
  
  // Second pass: collect file paths first (avoid rename while dir handle is open)
  std::vector<String> paths;
  paths.reserve(uploadTotalFiles + 4);

  root = SD.open("/logs");
  if (root) {
    File f = root.openNextFile();
    while (f) {
      String path = normalizeSdPath("/logs", f.name());
      bool isCsv = path.endsWith(".csv");
      bool isCurrent = (currentCsvPath.length() && path == currentCsvPath);
      f.close();

      if (isCsv && !isCurrent) {
        paths.push_back(path);
        // Stop collecting if we've hit the limit
        if (maxFiles > 0 && paths.size() >= filesToUpload) break;
      }

      f = root.openNextFile();
    }
    root.close();
  }

  // Now upload + move with no directory handle open
  uint32_t okCount = 0;
  for (size_t i = 0; i < paths.size(); i++) {
    uploadCurrentFile = paths[i];
    updateOLED(0);
    Serial.printf("[UPLOAD] File %u/%u: %s (heap free: %u bytes)\n",
                  (unsigned)(i + 1), (unsigned)paths.size(),
                  pathBasename(paths[i]).c_str(), (unsigned)ESP.getFreeHeap());

    // Guard: skip and delete header-only files (no scan data worth uploading)
    {
      uint32_t emptySz = 0;
      if (!csvHasDataRows(paths[i], &emptySz)) {
        Serial.printf("[UPLOAD] Empty CSV (%u bytes, no data rows), deleting: %s\n",
                      (unsigned)emptySz, pathBasename(paths[i]).c_str());
        if (!SD.remove(paths[i])) {
          Serial.printf("[UPLOAD] WARNING: SD.remove failed for %s -- file left in place\n", pathBasename(paths[i]).c_str());
        }
        uploadDoneFiles++;
        updateOLED(0);
        continue;
      }
    }

    // Step 1: WDGoWars first (if API key configured)
    bool wdgOk = false;
    if (cfg.wdgwarsApiKey.length() >= 8) {
      uploadTargetName = "WDGW UL";
      updateOLED(0);
      wdgOk = uploadFileToWdgwars(paths[i]);
      if (!wdgOk) uploadFailedFiles++;
      Serial.printf("[WDGWars] %s -> %s\n",
                    pathBasename(paths[i]).c_str(), wdgOk ? "OK" : "FAIL");
      if (i < paths.size() - 1 || cfg.wigleBasicToken.length() >= 8)
        delay(1500);  // brief settle between TLS sessions
    }

    // Step 2: WiGLE (if token configured)
    bool wigleOk = false;
    if (cfg.wigleBasicToken.length() >= 8) {
      uploadTargetName = "WiGLE UL";
      updateOLED(0);
      wigleOk = uploadFileToWigle(paths[i]);
      if (!wigleOk) uploadFailedFiles++;
    }

    // Move the file once at least one upload succeeded
    if (wigleOk || wdgOk) {
      okCount++;
      moveToUploaded(paths[i]);
    }

    uploadDoneFiles++;
    updateOLED(0);

    // Give TLS stack time to fully clean up between uploads
    if (i < paths.size() - 1) {  // Don't delay after last file
      delay(2000);
    }
  }

  uploading = false;
  uploadTargetName = "";
  scanningEnabled = uploadPausedScanWasEnabled;
  uploadCurrentFile = "";
  updateOLED(0);

  uploadLastResult = "Uploaded " + String(okCount) + "/" + String(uploadTotalFiles);
  return okCount;
}

// ---- WDGoWars-only batch upload (called from web UI button) ----
// Uploads all completed CSVs to WDGoWars only, then moves them.

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

  // Count eligible files
  File root = SD.open("/logs");
  if (root) {
    File f = root.openNextFile();
    while (f) {
      String name = normalizeSdPath("/logs", f.name());
      if (name.endsWith(".csv") && !(currentCsvPath.length() && name == currentCsvPath))
        uploadTotalFiles++;
      f.close(); f = root.openNextFile();
    }
    root.close();
  }

  if (uploadTotalFiles == 0) {
    uploading = false; uploadTargetName = "";
    scanningEnabled = uploadPausedScanWasEnabled;
    uploadLastResult = "No CSVs to upload";
    return 0;
  }

  uint32_t filesToUpload = uploadTotalFiles;
  if (maxFiles > 0 && (uint32_t)maxFiles < uploadTotalFiles)
    filesToUpload = (uint32_t)maxFiles;

  std::vector<String> paths;
  paths.reserve(filesToUpload + 2);
  root = SD.open("/logs");
  if (root) {
    File f = root.openNextFile();
    while (f) {
      String path = normalizeSdPath("/logs", f.name());
      bool isCsv = path.endsWith(".csv");
      bool isCurrent = (currentCsvPath.length() && path == currentCsvPath);
      f.close();
      if (isCsv && !isCurrent) {
        paths.push_back(path);
        if (maxFiles > 0 && paths.size() >= filesToUpload) break;
      }
      f = root.openNextFile();
    }
    root.close();
  }

  uint32_t okCount = 0;
  for (size_t i = 0; i < paths.size(); i++) {
    uploadCurrentFile = paths[i];
    updateOLED(0);
    Serial.printf("[UPLOAD] File %u/%u: %s (heap free: %u bytes)\n",
                  (unsigned)(i + 1), (unsigned)paths.size(),
                  pathBasename(paths[i]).c_str(), (unsigned)ESP.getFreeHeap());

    // Guard: skip and delete header-only files
    {
      uint32_t emptySz = 0;
      if (!csvHasDataRows(paths[i], &emptySz)) {
        Serial.printf("[UPLOAD] Empty CSV (%u bytes, no data rows), deleting: %s\n",
                      (unsigned)emptySz, pathBasename(paths[i]).c_str());
        if (!SD.remove(paths[i])) {
          Serial.printf("[UPLOAD] WARNING: SD.remove failed for %s -- file left in place\n", pathBasename(paths[i]).c_str());
        }
        uploadDoneFiles++;
        updateOLED(0);
        continue;
      }
    }

    bool ok = uploadFileToWdgwars(paths[i]);
    if (ok) { okCount++; moveToUploaded(paths[i]); }
    else    { uploadFailedFiles++; }
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
