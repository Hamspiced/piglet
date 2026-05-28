#include "WigleUpload.h"
#include "Globals.h"
#include "SDUtils.h"
#include "Display.h"
#include <WiFiClientSecure.h>
#include <vector>

static const char*     WDGWARS_HOST = "wdgwars.pl";
static const uint16_t  WDGWARS_PORT = 443;

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
    Serial.println("[WiGLE] File does not exist on SD");
    uploadLastResult = "File missing";
    return false;
  }

  File f = SD.open(path, FILE_READ);
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

  // Stream the file
  uint8_t buf[1024];
  while (true) {
    int n = f.read(buf, sizeof(buf));
    if (n <= 0) break;
    client.write(buf, n);
    yield();
  }
  f.close();
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
    Serial.println("[WDGWars] File does not exist");
    return false;
  }

  File f = SD.open(path, FILE_READ);
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

  // V2 API: HTTP 202 Accepted — async job submitted
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
    Serial.printf("[WDGWars] Job %d submitted, polling for result...\n", jobId);
    String jobPath = String("/api/v2/upload-job/") + String(jobId);

    // Poll up to 15 times with 3 s intervals (45 s max total)
    for (int poll = 1; poll <= 15; poll++) {
      delay(3000); yield();

      WiFiClientSecure pc;
      pc.setInsecure(); pc.setTimeout(15000);
      if (!pc.connect(WDGWARS_HOST, WDGWARS_PORT)) {
        Serial.printf("[WDGWars] Poll %d: connect fail, retrying\n", poll);
        continue;
      }
      pc.print("GET " + jobPath + " HTTP/1.0\r\n");
      pc.print(String("Host: ") + WDGWARS_HOST + "\r\n");
      pc.print(String("X-API-Key: ") + cfg.wdgwarsApiKey + "\r\n");
      pc.print("Connection: close\r\n\r\n");

      uint32_t pw = millis();
      while (!pc.available() && pc.connected() && (millis() - pw) < 10000) { delay(100); yield(); }
      if (!pc.available()) { pc.stop(); continue; }

      pc.readStringUntil('\n');  // skip HTTP status line
      String pb = ""; bool pInBody = false;
      while (pc.connected() || pc.available()) {
        String ln = pc.readStringUntil('\n'); ln.trim();
        if (!pInBody) { if (ln.length() == 0) pInBody = true; }
        else { pb += ln; if (pb.length() > 512) break; }
      }
      pc.stop();
      Serial.printf("[WDGWars] Poll %d: %s\n", poll, pb.substring(0, 80).c_str());

      if (pb.indexOf("\"done\"") >= 0) {
        // Extract imported count from result object
        int imp = 0;
        int ii = pb.indexOf("\"imported\":");
        if (ii >= 0) imp = pb.substring(ii + 11).toInt();
        uploadLastResult = "WDGW OK (" + String(imp) + " net)";
        Serial.printf("[WDGWars] Job %d done — imported: %d\n", jobId, imp);
        return true;
      }
      if (pb.indexOf("\"failed\"") >= 0) {
        uploadLastResult = "WDGW: job failed";
        Serial.printf("[WDGWars] Job %d failed\n", jobId);
        return false;
      }
      // status still "processing" — keep polling
    }
    uploadLastResult = "WDGW: poll timeout";
    Serial.printf("[WDGWars] Job %d: no result after 45s\n", jobId);
    return false;
  }

  uploadLastResult = "WDGW fail (" + String(code) + ")";
  Serial.printf("[WDGWars] Upload FAILED HTTP %d: %s\n", code, body.c_str());
  return false;
}

// ---- Empty-file guard ----
// Returns true if the CSV contains at least one data row beyond the two
// mandatory WiGLE header lines.  Opens, reads two lines, checks for more.
static bool csvHasDataRows(const String& path) {
  File f = SD.open(path, FILE_READ);
  if (!f) return false;

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
    if (!csvHasDataRows(path)) {
      Serial.printf("[CLEANUP] Empty CSV, deleting: %s\n", pathBasename(path).c_str());
      SD.remove(path);
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

  // Collect candidate file paths in one pass (avoid rename while dir handle is open).
  // We intentionally do NOT apply maxFiles here so uploadTotalFiles reflects the
  // true number of eligible CSVs on disk; the loop below honours maxFiles.
  std::vector<String> paths;
  File root = SD.open("/logs");
  if (root) {
    File f = root.openNextFile();
    while (f) {
      String path = normalizeSdPath("/logs", f.name());
      bool isCsv = path.endsWith(".csv");
      bool isCurrent = (currentCsvPath.length() && path == currentCsvPath);
      f.close();
      if (isCsv && !isCurrent) paths.push_back(path);
      f = root.openNextFile();
    }
    root.close();
  }
  uploadTotalFiles = paths.size();

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
    paths.resize(filesToUpload);
  }

  // Now upload + move with no directory handle open
  uint32_t okCount = 0;
  for (size_t i = 0; i < paths.size(); i++) {
    uploadCurrentFile = paths[i];
    updateOLED(0);

    // Guard: skip and delete header-only files (no scan data worth uploading)
    if (!csvHasDataRows(paths[i])) {
      Serial.printf("[UPLOAD] Empty CSV (no data rows), deleting: %s\n",
                    pathBasename(paths[i]).c_str());
      SD.remove(paths[i]);
      uploadDoneFiles++;
      updateOLED(0);
      continue;
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

  // Collect candidate file paths in a single pass.
  std::vector<String> paths;
  File root = SD.open("/logs");
  if (root) {
    File f = root.openNextFile();
    while (f) {
      String path = normalizeSdPath("/logs", f.name());
      bool isCsv = path.endsWith(".csv");
      bool isCurrent = (currentCsvPath.length() && path == currentCsvPath);
      f.close();
      if (isCsv && !isCurrent) paths.push_back(path);
      f = root.openNextFile();
    }
    root.close();
  }
  uploadTotalFiles = paths.size();

  if (uploadTotalFiles == 0) {
    uploading = false; uploadTargetName = "";
    scanningEnabled = uploadPausedScanWasEnabled;
    uploadLastResult = "No CSVs to upload";
    return 0;
  }

  if (maxFiles > 0 && (uint32_t)maxFiles < uploadTotalFiles) {
    paths.resize((size_t)maxFiles);
  }

  uint32_t okCount = 0;
  for (size_t i = 0; i < paths.size(); i++) {
    uploadCurrentFile = paths[i];
    updateOLED(0);

    // Guard: skip and delete header-only files
    if (!csvHasDataRows(paths[i])) {
      Serial.printf("[UPLOAD] Empty CSV (no data rows), deleting: %s\n",
                    pathBasename(paths[i]).c_str());
      SD.remove(paths[i]);
      uploadDoneFiles++;
      updateOLED(0);
      continue;
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
