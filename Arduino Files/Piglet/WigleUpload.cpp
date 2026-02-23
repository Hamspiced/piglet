#include "WigleUpload.h"
#include "Globals.h"
#include "SDUtils.h"
#include "Display.h"
#include <WiFiClientSecure.h>
#include <vector>

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

// ---- Batch upload ----

uint32_t uploadAllCsvsToWigle(int maxFiles) {
  if (!sdOk) { uploadLastResult = "SD not OK"; return 0; }

  // Pause scanning during upload to avoid SD contention + WiFi scan interference
  uploadPausedScanWasEnabled = scanningEnabled;
  scanningEnabled = false;

  uploading = true;
  uploadDoneFiles = 0;
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

    bool ok = uploadFileToWigle(paths[i]);
    if (ok) {
      okCount++;
      moveToUploaded(paths[i]);
    }

    uploadDoneFiles++;
    updateOLED(0);
    
    // Give TLS stack time to fully clean up between uploads
    if (i < paths.size() - 1) {  // Don't delay after last file
      delay(2000);  // Increased from 1.5s to 2s
    }
  }

  uploading = false;
  scanningEnabled = uploadPausedScanWasEnabled;
  uploadCurrentFile = "";
  updateOLED(0);

  uploadLastResult = "Uploaded " + String(okCount) + "/" + String(uploadTotalFiles);
  return okCount;
}

// ---- Load upload history from WiGLE ----

void wigleLoadHistory() {
  // Cache results for 24 hours to avoid rate limiting
  const uint32_t CACHE_DURATION_MS = 86400000;  // 24 hours
  uint32_t now = millis();
  
  if (wigleHistoryLastLoadMs > 0 && (now - wigleHistoryLastLoadMs) < CACHE_DURATION_MS) {
    Serial.printf("[WiGLE] History cached (%d entries, loaded %d ms ago)\n", 
                  wigleHistoryCount, now - wigleHistoryLastLoadMs);
    return;
  }
  
  Serial.println("[WiGLE] Loading upload history from API...");
  
  wigleHistoryCount = 0;
  
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WiGLE] No STA WiFi - skipping history load");
    return;
  }
  
  if (cfg.wigleBasicToken.length() < 8) {
    Serial.println("[WiGLE] No token - skipping history load");
    return;
  }

  Serial.println("[WiGLE] History: Creating TLS client...");
  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(15000);  // Reduced from 20s to 15s

  Serial.println("[WiGLE] History: Connecting to api.wigle.net:443...");
  if (!client.connect(WIGLE_HOST, WIGLE_PORT)) {
    Serial.println("[WiGLE] History: TLS connect failed");
    return;
  }
  
  Serial.println("[WiGLE] History: Connected, sending request...");

  // Request up to 50 most recent transactions
  client.print("GET /api/v2/file/transactions?pagestart=0&pageend=50 HTTP/1.0\r\n");
  client.print(String("Host: ") + WIGLE_HOST + "\r\n");
  client.print(String("Authorization: Basic ") + cfg.wigleBasicToken + "\r\n");
  client.print("Connection: close\r\n\r\n");

  // Wait for response
  uint32_t waitStart = millis();
  while (!client.available() && client.connected() && (millis() - waitStart) < 10000) {
    delay(10);
    yield();
  }

  // Skip headers
  Serial.println("[WiGLE] History: Skipping HTTP headers...");
  bool inHeaders = true;
  while (client.connected() && inHeaders) {
    String line = client.readStringUntil('\n');
    if (line.length() < 3) inHeaders = false;
  }
  Serial.println("[WiGLE] History: Headers skipped, parsing JSON...");

  // Parse JSON response incrementally
  // Look for patterns like: {"transid":"...", "fileName":"WiGLE_...", "fileSize":123, 
  // "discoveredGps":45, "totalGps":67, "wait":null}
  
  String buffer = "";
  uint32_t parseStart = millis();
  uint32_t lastActivity = millis();
  uint32_t lastLogTime = millis();
  uint32_t bytesRead = 0;
  const uint32_t PARSE_TIMEOUT = 15000;  // 15s total timeout
  const uint32_t IDLE_TIMEOUT = 5000;    // 5s idle timeout
  
  while ((client.connected() || client.available()) && 
         (millis() - parseStart) < PARSE_TIMEOUT) {
    if (client.available()) {
      lastActivity = millis();  // Reset idle timer
      char c = client.read();
      bytesRead++;
      buffer += c;
      
      // Log progress every 2 seconds
      if ((millis() - lastLogTime) > 2000) {
        Serial.printf("[WiGLE] History: Read %d bytes, found %d files\n", bytesRead, wigleHistoryCount);
        lastLogTime = millis();
      }
      
      // Process when we hit end of a transaction object
      if (c == '}' && buffer.indexOf("fileName") > 0) {
        
        // Extract fileName
        int fnStart = buffer.indexOf("\"fileName\":\"");
        if (fnStart > 0) {
          fnStart += 12; // length of "fileName":"
          int fnEnd = buffer.indexOf("\"", fnStart);
          if (fnEnd > fnStart) {
            String fileName = buffer.substring(fnStart, fnEnd);
            
            // Extract fileSize
            int fsStart = buffer.indexOf("\"fileSize\":", fnEnd);
            uint32_t fileSize = 0;
            if (fsStart > 0) {
              fsStart += 11; // length of "fileSize":
              int fsEnd = buffer.indexOf(",", fsStart);
              if (fsEnd < 0) fsEnd = buffer.indexOf("}", fsStart);
              if (fsEnd > fsStart) {
                fileSize = buffer.substring(fsStart, fsEnd).toInt();
              }
            }
            
            // Extract discoveredGps
            int dgStart = buffer.indexOf("\"discoveredGps\":");
            uint32_t discovered = 0;
            if (dgStart > 0) {
              dgStart += 16; // length of "discoveredGps":
              int dgEnd = buffer.indexOf(",", dgStart);
              if (dgEnd < 0) dgEnd = buffer.indexOf("}", dgStart);
              if (dgEnd > dgStart) {
                discovered = buffer.substring(dgStart, dgEnd).toInt();
              }
            }
            
            // Extract totalGps
            int tgStart = buffer.indexOf("\"totalGps\":");
            uint32_t total = 0;
            if (tgStart > 0) {
              tgStart += 11; // length of "totalGps":
              int tgEnd = buffer.indexOf(",", tgStart);
              if (tgEnd < 0) tgEnd = buffer.indexOf("}", tgStart);
              if (tgEnd > tgStart) {
                total = buffer.substring(tgStart, tgEnd).toInt();
              }
            }
            
            // Check wait status (wait:null means processed, wait:"..." means still processing)
            bool isWaiting = (buffer.indexOf("\"wait\":null") < 0);
            
            // Store in history array if we have space
            if (wigleHistoryCount < WIGLE_HISTORY_MAX) {
              wigleHistory[wigleHistoryCount].basename = fileName;
              wigleHistory[wigleHistoryCount].fileSize = fileSize;
              wigleHistory[wigleHistoryCount].discoveredGps = discovered;
              wigleHistory[wigleHistoryCount].totalGps = total;
              wigleHistory[wigleHistoryCount].wait = isWaiting;
              wigleHistoryCount++;
            }
          }
        }
        
        buffer = ""; // Reset for next object
      }
      
      // Keep buffer from growing too large
      if (buffer.length() > 1024) {
        buffer = buffer.substring(buffer.length() - 512);
      }
    } else {
      // No data available - check for idle timeout
      if ((millis() - lastActivity) > IDLE_TIMEOUT) {
        Serial.println("[WiGLE] History: Idle timeout - no data for 5s");
        break;
      }
      delay(10);
      yield();
    }
  }
  
  if ((millis() - parseStart) >= PARSE_TIMEOUT) {
    Serial.println("[WiGLE] History: Parse timeout (15s)");
  }
  
  // Ensure TLS connection is fully closed and resources freed
  Serial.println("[WiGLE] History: Closing TLS connection...");
  client.stop();
  
  // CRITICAL: ESP32-C5/C6 TLS stack needs time to fully release resources
  // Without this, subsequent TLS connections will fail
  Serial.println("[WiGLE] History: Waiting for TLS stack to release resources...");
  delay(1000);  // Increased from 100ms
  yield();
  
  // Force any pending cleanup
  Serial.printf("[WiGLE] History: Heap after cleanup: %u bytes\n", ESP.getFreeHeap());
  
  // Update cache timestamp
  wigleHistoryLastLoadMs = millis();
  
  Serial.printf("[WiGLE] History: Loaded %d file stats (cached for 24h)\n", wigleHistoryCount);
}
