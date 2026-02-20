#include "WebUI.h"
#include "Globals.h"
#include "Config.h"
#include "Dedup.h"
#include "SDUtils.h"
#include "Display.h"
#include "WigleUpload.h"
#include <ArduinoJson.h>

// ---------------- Embedded HTML ----------------
static const char INDEX_HTML[] PROGMEM = R"HTML(
<!doctype html>
<html>
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Piglet Wardriver</title>
<style>
  :root{
    --bg:#0b0f14;
    --card:#121925;
    --text:#e6edf6;
    --muted:#9fb0c3;
    --border:#253043;
    --input:#0f1622;
    --inputBorder:#2b3a52;
    --btn:#1d2a3d;
    --btnHover:#253650;
    --btnText:#e6edf6;
    --code:#0f1622;
    --ok:#2dd4bf;
    --bad:#fb7185;
    --warn:#fbbf24;
    --bar:#e6edf6;
    --barBg:#0f1622;
  }

  /* If the browser is light mode and you want to respect it, comment this block out.
     Right now we force dark by default (best for "darkmode"). */
  html{ color-scheme: dark; }

  body{
    font-family:system-ui,-apple-system,Segoe UI,Roboto,Arial,sans-serif;
    margin:16px;
    max-width:900px;
    background:var(--bg);
    color:var(--text);
  }

  h2,h3,h4{ color:var(--text); }

  .card{
    border:1px solid var(--border);
    border-radius:12px;
    padding:14px;
    margin:12px 0;
    background:var(--card);
    box-shadow: 0 6px 24px rgba(0,0,0,.25);
  }

  input,select,button,pre{
    padding:10px;
    border-radius:10px;
    border:1px solid var(--inputBorder);
    width:100%;
    box-sizing:border-box;
    background:var(--input);
    color:var(--text);
  }

  pre{
    overflow:auto;
    margin:0;
  }

  label{ color:var(--muted); }

  button{
    cursor:pointer;
    background:var(--btn);
    border:1px solid var(--inputBorder);
    color:var(--btnText);
    transition: background .12s ease, transform .05s ease;
  }

  button:hover{ background:var(--btnHover); }
  button:active{ transform: translateY(1px); }
  button:disabled{ opacity:.55; cursor:not-allowed; }

  a{ color:var(--ok); text-decoration:none; }
  a:hover{ text-decoration:underline; }

  .row{
    display:grid;
    grid-template-columns:1fr 1fr;
    gap:12px
  }

  code{
    background:var(--code);
    padding:2px 6px;
    border-radius:6px;
    border:1px solid var(--border);
  }

  /* WiGLE progress bar container */
  .barWrap{
    height:10px;
    border:1px solid var(--border);
    border-radius:8px;
    overflow:hidden;
    margin-top:8px;
    background:var(--barBg);
  }

  /* inner bar */
  #wigleBar{
    height:10px;
    width:0%;
    background:var(--bar);
  }

  /* Small helpers for status text color if you want them later */
  .ok{ color:var(--ok); }
  .bad{ color:var(--bad); }
  .warn{ color:var(--warn); }
  .muted{ color:var(--muted); }
    /* --- Friendly status UI (Patch 2) --- */
  .statusGrid{
    display:flex;
    flex-wrap:wrap;
    gap:8px;
    margin:8px 0 12px 0;
  }

  .pill{
    display:inline-flex;
    align-items:center;
    gap:8px;
    border:1px solid var(--border);
    border-radius:999px;
    padding:6px 10px;
    font-size:14px;
    background:var(--input);
    color:var(--text);
  }
  .pill.ok{ border-color: rgba(45,212,191,.6); }
  .pill.bad{ border-color: rgba(251,113,133,.6); }
  .pill.warn{ border-color: rgba(251,191,36,.6); }

  .kv{
    display:grid;
    grid-template-columns:1fr;
    gap:8px;
  }
  @media (min-width:700px){
    .kv{ grid-template-columns:1fr 1fr; }
  }

  .kv > div{
    display:flex;
    justify-content:space-between;
    gap:12px;
    border:1px solid var(--border);
    border-radius:10px;
    padding:10px 12px;
    background:var(--input);
  }

  .k{ color:var(--muted); }
  .v{ font-weight:600; color:var(--text); }
</style>
</head>
<body>
  <h2>Piglet Wardriver</h2>

  <div class="card">
    <h3>Status</h3>

    <div class="statusGrid">
      <div class="pill" id="pillScan">Scan: —</div>
      <div class="pill" id="pillSd">SD: —</div>
      <div class="pill" id="pillGps">GPS: —</div>
      <div class="pill" id="pillSta">STA: —</div>
      <div class="pill" id="pillWigle">WiGLE: —</div>
    </div>

    <div class="kv">
      <div><span class="k">2.4G Found</span><span class="v" id="vFound2g">—</span></div>
      <div id="row5g"><span class="k">5G Found</span><span class="v" id="vFound5g">—</span></div>

      <div><span class="k">STA IP</span><span class="v" id="vStaIp">—</span></div>
      <div><span class="k">AP Clients Seen</span><span class="v" id="vApSeen">—</span></div>

      <div><span class="k">De-dupe</span><span class="v" id="vDedup">—</span></div>
      <div><span class="k">Last Upload</span><span class="v" id="vLastUpload">—</span></div>
    </div>

    <details style="margin-top:10px">
      <summary style="cursor:pointer">Advanced</summary>
       <div class="kv" style="margin-top:8px">
        <div><span class="k">Scan Mode</span><span class="v" id="vScanMode">—</span></div>
        <div><span class="k">GPS Baud</span><span class="v" id="vGpsBaud">—</span></div>
        <div><span class="k">Home SSID</span><span class="v" id="vHomeSsid">—</span></div>
        <div><span class="k">Wardriver SSID</span><span class="v" id="vApSsid">—</span></div>
      </div>
      <div style="margin-top:8px">
        <a href="/status.json" target="_blank" rel="noopener">Open raw status.json</a>
      </div>
    </details>

    <div class="row" style="margin-top:12px">
      <button onclick="fetch('/start',{method:'POST'}).then(loadStatus)">Start Scan</button>
      <button onclick="fetch('/stop',{method:'POST'}).then(loadStatus)">Stop Scan</button>
    </div>

    <div class="row" style="margin-top:10px">
      <button onclick="wigleTest()">Test WiGLE Token</button>
      <button onclick="wigleUploadAll()">Upload all CSVs to WiGLE</button>
    </div>

    <div class="card" style="margin-top:12px">
      <h4 style="margin:0 0 8px 0">WiGLE</h4>
      <div id="wigleMsg" class="muted">—</div>
      <div class="barWrap">
        <div id="wigleBar"></div>
      </div>
    </div>
  </div>

  <div class="card">
    <h3>Config</h3>
    <div class="row">
      <div><label>WiGLE Basic Token (Encoded for Use)</label><input id="wigleBasicToken" placeholder="BASE64(apiName:apiToken)"></div>
      <div><label>GPS Baud</label><input id="gpsBaud" type="number" value="9600"></div>
      <div><label>Home SSID</label><input id="homeSsid"></div>
      <div><label>Home PSK</label><input id="homePsk" type="password"></div>
      <div><label>Wardriver SSID</label><input id="wardriverSsid"></div>
      <div><label>Wardriver PSK</label><input id="wardriverPsk" type="password"></div>
      <div><label>Scan Mode</label>
        <select id="scanMode">
          <option value="aggressive">aggressive</option>
          <option value="powersaving">powersaving</option>
        </select>
      </div>
      <div><label>Speed Units</label>
        <select id="speedUnits">
          <option value="kmh">km/h</option>
          <option value="mph">mph</option>
        </select>
      </div>
      <div><label>Board</label>
        <select id="board">
          <option value="auto">auto</option>
          <option value="s3">s3</option>
          <option value="exp">exp</option>
          <option value="c5">c5</option>
          <option value="c6">c6</option>
        </select>
      </div>
      <div><label>Battery ADC Pin (-1 = off)</label><input id="battPin" type="number" value="-1"></div>
      <div style="display:flex;align-items:flex-end;">
        <button onclick="saveCfg()">Save Config</button>
      </div>
    </div>
    <p style="margin-top:10px;">Config is stored at <code>/wardriver.cfg</code> on the SD card.</p>
  </div>

  <div class="card">
    <h3>SD Files</h3>
    <div id="files">Loading...</div>
  </div>

<script>
function setPill(id, text, cls){
  const el = document.getElementById(id);
  if(!el) return;
  el.classList.remove('ok','bad','warn');
  if (cls) el.classList.add(cls);
  el.textContent = text;
}

function wigleStatusText(s){
  if (s === 1) return "WiGLE: Valid";
  if (s === -1) return "WiGLE: Invalid";
  return "WiGLE: Unknown";
}

async function loadStatus(){
  const r = await fetch('/status.json');
  const j = await r.json();

  // Pills
  setPill('pillScan', `Scan: ${j.allowScan ? 'ACTIVE' : 'PAUSED'}`, j.allowScan ? 'ok' : 'warn');
  setPill('pillSd', `SD: ${j.sdOk ? 'OK' : 'FAIL'}`, j.sdOk ? 'ok' : 'bad');
  setPill('pillGps', `GPS: ${j.gpsFix ? 'LOCK' : 'NO FIX'}`, j.gpsFix ? 'ok' : 'warn');
  setPill('pillSta', `STA: ${j.wifiConnected ? 'CONNECTED' : 'DISCONNECTED'}`, j.wifiConnected ? 'ok' : 'warn');

  const wigleCls = (j.wigleTokenStatus === 1) ? 'ok' : (j.wigleTokenStatus === -1 ? 'bad' : 'warn');
  setPill('pillWigle', wigleStatusText(j.wigleTokenStatus), wigleCls);

  // Key values
  const setText = (id, val) => {
    const el = document.getElementById(id);
    if (el) el.textContent = (val === undefined || val === null || val === "") ? "\u2014" : String(val);
  };

  setText('vFound2g', j.found2g);
  setText('vFound5g', j.found5g);

  const row5g = document.getElementById('row5g');
  if (row5g) row5g.style.display = (j.c5Connected || j.found5g) ? "" : "none";
  setText('vStaIp', j.wifiConnected ? (j.staIp || '\u2014') : '\u2014');
  setText('vApSeen', j.apClientsSeen ? 'Yes' : 'No');
  setText('vDedup', `${j.seenCount || 0} seen, ${j.seenCollisions || 0} collisions`);

  const lastUp = j.uploadLastResult ? `${j.uploadLastResult} (HTTP ${j.wigleLastHttpCode || '\u2014'})` : '\u2014';
  setText('vLastUpload', lastUp);

  // Advanced
  setText('vScanMode', j?.config?.scanMode || '\u2014');
  setText('vGpsBaud',  j?.config?.gpsBaud || '\u2014');
  setText('vHomeSsid', j?.config?.homeSsid || '\u2014');
  setText('vApSsid',   j?.config?.wardriverSsid || '\u2014');

  // Keep existing config form fill-in
  for (const k of ['board','gpsBaud','homeSsid','wardriverSsid','wardriverPsk','scanMode','speedUnits','battPin']){
    if (j.config && (k in j.config)) {
      const el = document.getElementById(k);
      if (el) el.value = j.config[k];
    }
  }
}
async function loadFiles(){
  const r = await fetch('/files.json'); const j = await r.json();
  const el = document.getElementById('files');
  if(!j.ok){ el.textContent = 'SD not available'; return; }
  el.innerHTML = j.files.map(f => `
    <div style="display:flex;gap:8px;align-items:center;margin:8px 0;flex-wrap:wrap">
      <a href="/download?name=${encodeURIComponent(f.name)}">${f.name}</a>
      <span class="muted">${f.size} bytes</span>
      <button style="width:auto" onclick="wigleUploadOne('${f.name}')">Upload</button>
      <button style="width:auto" onclick="delFile('${f.name}')">Delete</button>
    </div>`).join('') || '(no files)';
}
async function delFile(name){
  await fetch('/delete?name='+encodeURIComponent(name), {method:'POST'});
  loadFiles();
}
async function saveCfg(){
  // Build key=value config (human readable, matches wardriver.cfg)
  const keys = ['board','wigleBasicToken','gpsBaud','homeSsid','homePsk','wardriverSsid','wardriverPsk','scanMode','speedUnits','battPin'];

  let body = "";
  body += "# Saved from Web UI\n";
  body += "# key=value\n";

  for (const k of keys){
    const el = document.getElementById(k);
    const v = el ? (el.value ?? "") : "";
    body += `${k}=${String(v).replace(/\r?\n/g, " ")}\n`; // keep it single-line per key
  }

  await fetch('/saveConfig', {
    method:'POST',
    headers:{'Content-Type':'text/plain'},
    body
  });

  await loadStatus();
  alert('Saved. Reboot device to apply WiFi/GPS changes.');
}
function setWigleMsg(t){ document.getElementById('wigleMsg').textContent = t; }

async function wigleTest(){
  setWigleMsg("Testing token...");
  const r = await fetch('/wigle/test', {method:'POST'});
  const j = await r.json().catch(()=>({ok:false,message:"Bad JSON"}));
  setWigleMsg(j.message || (j.ok ? "OK" : "FAIL"));
  await loadStatus();
}

async function wigleUploadAll(){
  setWigleMsg("Uploading all...");
  await fetch('/wigle/uploadAll', {method:'POST'});
  // progress will show via status polling
}

async function wigleUploadOne(name){
  setWigleMsg("Uploading " + name + " ...");
  await fetch('/wigle/upload?name='+encodeURIComponent(name), {method:'POST'});
  await loadFiles();
  await loadStatus();
}

// Poll status for upload progress and show a progress bar.
async function pollUpload(){
  const r = await fetch('/status.json'); const j = await r.json();
  const uploading = !!j.uploading;
  const done = j.uploadDoneFiles || 0;
  const total = j.uploadTotalFiles || 0;

  let pct = 0;
  if (total > 0) pct = Math.round((done/total)*100);

  const bar = document.getElementById('wigleBar');
  bar.style.width = pct + "%";
  bar.style.background = uploading ? "#333" : "#999";

  if (uploading){
    setWigleMsg(`Uploading... ${done}/${total} (${pct}%)`);
  } else if (j.uploadLastResult){
    setWigleMsg(j.uploadLastResult);
  }
}
setInterval(pollUpload, 1000);
loadStatus(); loadFiles();
</script>
</body>
</html>
)HTML";

// ---------------- Handlers ----------------

static void handleRoot() {
  server.sendHeader("Cache-Control", "no-store");
  server.send_P(200, "text/html", INDEX_HTML);
}

static void handleStatus() {
  StaticJsonDocument<1024> doc;

  bool allowScan = scanningEnabled && sdOk && (userScanOverride || !autoPaused);
  doc["scanningEnabled"] = scanningEnabled;
  doc["allowScan"] = allowScan;
  doc["userScanOverride"] = userScanOverride;
  doc["autoPaused"] = autoPaused;
  doc["sdOk"] = sdOk;
  doc["gpsFix"] = gpsHasFix;
  doc["found2g"] = networksFound2G;
  doc["found5g"] = wardriverIsC5() ? networksFound5G : 0;
  doc["c5Connected"] = wardriverIsC5();
  doc["wifiConnected"] = (WiFi.status() == WL_CONNECTED);
  doc["staIp"] = (WiFi.status() == WL_CONNECTED) ? WiFi.localIP().toString() : "";
  doc["apClientsSeen"] = apClientSeen;
  doc["seenCount"] = seenCount;
  doc["seenCollisions"] = seenCollisions;
  doc["seenTableSize"] = SEEN_TABLE_SIZE;
  doc["uploading"] = uploading;
  doc["uploadTotalFiles"] = uploadTotalFiles;
  doc["uploadDoneFiles"] = uploadDoneFiles;
  doc["uploadCurrentFile"] = uploadCurrentFile;
  doc["uploadLastResult"] = uploadLastResult;
  doc["wigleTokenStatus"] = wigleTokenStatus;
  doc["wigleLastHttpCode"] = wigleLastHttpCode;

  JsonObject c = doc.createNestedObject("config");
  c["wigleBasicToken"] = cfg.wigleBasicToken.length() ? "(set)" : "";
  c["homeSsid"] = cfg.homeSsid;
  c["homePsk"] = cfg.homePsk.length() ? "(set)" : "";
  c["wardriverSsid"] = cfg.wardriverSsid;
  c["wardriverPsk"] = cfg.wardriverPsk;
  c["gpsBaud"] = cfg.gpsBaud;
  c["scanMode"] = cfg.scanMode;
  c["board"] = cfg.board;
  c["speedUnits"] = cfg.speedUnits;
  c["battPin"] = cfg.battPin;

  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "application/json", "");
  WiFiClient client = server.client();
  serializeJson(doc, client);
}

static void addDirFiles(JsonArray arr, const char* dir) {
  File root = SD.open(dir);
  if (!root) return;

  File f = root.openNextFile();
  while (f) {
    const char* rawName = f.name();

    String fullPath = normalizeSdPath(dir, rawName);
    if (fullPath.length() == 0) {
      f.close();
      f = root.openNextFile();
      continue;
    }

    JsonObject o = arr.createNestedObject();
    o["name"] = fullPath;
    o["size"] = (uint32_t)f.size();

    f.close();
    f = root.openNextFile();
  }

  root.close();
}

static void handleFiles() {
  StaticJsonDocument<4096> doc;
  doc["ok"] = sdOk;

  JsonArray arr = doc.createNestedArray("files");
  if (sdOk) {
    addDirFiles(arr, "/logs");
    addDirFiles(arr, "/uploaded");
  }

  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "application/json", "");
  WiFiClient client = server.client();
  serializeJson(doc, client);
}

static void handleDownload() {
  if (!sdOk) { server.send(500, "text/plain", "SD not available"); return; }
  if (!server.hasArg("name")) { server.send(400, "text/plain", "Missing name"); return; }
  String name = server.arg("name");
  if (!isAllowedDataPath(name)) { server.send(403, "text/plain", "Forbidden"); return; }
  if (!SD.exists(name)) { server.send(404, "text/plain", "Not found"); return; }

  File f = SD.open(name, FILE_READ);
  server.streamFile(f, "text/csv");
  f.close();
}

static void handleDelete() {
  if (!sdOk) { server.send(500, "text/plain", "SD not available"); return; }
  if (!server.hasArg("name")) { server.send(400, "text/plain", "Missing name"); return; }
  String name = server.arg("name");
  if (!isAllowedDataPath(name)) { server.send(403, "text/plain", "Forbidden"); return; }
  bool ok = SD.remove(name);
  server.send(ok ? 200 : 500, "text/plain", ok ? "OK" : "FAIL");
}

static void handleStart() {
  scanningEnabled = true;
  userScanOverride = true;
  server.send(200, "text/plain", "OK");
}

static void handleStop() {
  scanningEnabled = false;
  userScanOverride = true;
  server.send(200, "text/plain", "OK");
}

static void handleSaveConfig() {
  if (!sdOk) { server.send(500, "text/plain", "SD not available"); return; }

  String body = server.arg("plain");
  body.trim();
  if (body.length() == 0) { server.send(400, "text/plain", "Empty body"); return; }

  bool any = false;

  // If someone posts JSON manually, still accept it (nice fallback)
  if (body[0] == '{') {
    DynamicJsonDocument doc(1024);
    DeserializationError err = deserializeJson(doc, body);
    if (err) { server.send(400, "text/plain", "Bad JSON"); return; }

    cfg.wigleBasicToken = doc["wigleBasicToken"] | cfg.wigleBasicToken;
    cfg.homeSsid        = doc["homeSsid"]        | cfg.homeSsid;
    cfg.homePsk         = doc["homePsk"]         | cfg.homePsk;
    cfg.wardriverSsid   = doc["wardriverSsid"]   | cfg.wardriverSsid;
    cfg.wardriverPsk    = doc["wardriverPsk"]    | cfg.wardriverPsk;
    cfg.gpsBaud         = doc["gpsBaud"]         | cfg.gpsBaud;
    cfg.scanMode        = doc["scanMode"]        | cfg.scanMode;

    any = true;
  } else {
    // Parse key=value lines
    int pos = 0;
    while (pos < body.length()) {
      int nl = body.indexOf('\n', pos);
      if (nl < 0) nl = body.length();
      String line = body.substring(pos, nl);
      pos = nl + 1;

      String k, v;
      if (parseKeyValueLine(line, k, v)) {
        cfgAssignKV(k, v);
        any = true;
      }
    }
  }

  if (!any) {
    server.send(400, "text/plain", "No valid key=value lines");
    return;
  }

  Serial.println("[CFG] Updated config from Web UI (in-RAM). Saving to SD...");
  bool ok = saveConfigToSD();
  server.send(ok ? 200 : 500, "text/plain", ok ? "OK" : "FAIL");
}

static void handleWigleTest() {
  bool ok = wigleTestToken();

  DynamicJsonDocument doc(384);
  doc["ok"] = ok;
  doc["tokenStatus"] = wigleTokenStatus;
  doc["httpCode"] = wigleLastHttpCode;
  doc["message"] = uploadLastResult;

  String out;
  serializeJson(doc, out);
  server.send(ok ? 200 : 400, "application/json", out);
}

static void handleWigleUploadAll() {
  if (!sdOk) { server.send(500, "text/plain", "SD not available"); return; }
  if (WiFi.status() != WL_CONNECTED) { server.send(400, "text/plain", "STA WiFi not connected"); return; }

  uint32_t okCount = uploadAllCsvsToWigle();

  DynamicJsonDocument doc(384);
  doc["ok"] = (okCount > 0);
  doc["uploaded"] = okCount;
  doc["total"] = uploadTotalFiles;
  doc["message"] = uploadLastResult;

  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

static void handleWigleUploadOne() {
  if (!sdOk) { server.send(500, "text/plain", "SD not available"); return; }
  if (WiFi.status() != WL_CONNECTED) { server.send(400, "text/plain", "STA WiFi not connected"); return; }
  if (!server.hasArg("name")) { server.send(400, "text/plain", "Missing name"); return; }

  String path = server.arg("name");
  if (!SD.exists(path)) { server.send(404, "text/plain", "Not found"); return; }

  uploading = true;
  uploadPausedScanWasEnabled = scanningEnabled;
  scanningEnabled = false;

  uploadTotalFiles = 1;
  uploadDoneFiles = 0;
  uploadCurrentFile = path;
  updateOLED(0);

  bool ok = uploadFileToWigle(path);

  uploadDoneFiles = 1;
  updateOLED(0);

  uploading = false;
  scanningEnabled = uploadPausedScanWasEnabled;
  uploadCurrentFile = "";
  updateOLED(0);

  if (ok) {
    moveToUploaded(path);
  }

  DynamicJsonDocument doc(384);
  doc["ok"] = ok;
  doc["httpCode"] = wigleLastHttpCode;
  doc["message"] = uploadLastResult;

  String out;
  serializeJson(doc, out);
  server.send(ok ? 200 : 500, "application/json", out);
}

// ---------------- Server Init ----------------

void startWebServer() {
  Serial.println("[WEB] Starting web server routes...");

  server.on("/", handleRoot);
  server.on("/status.json", handleStatus);
  server.on("/files.json", handleFiles);
  server.on("/download", handleDownload);
  server.on("/delete", HTTP_POST, handleDelete);
  server.on("/start",  HTTP_POST, handleStart);
  server.on("/stop",   HTTP_POST, handleStop);
  server.on("/saveConfig", HTTP_POST, handleSaveConfig);

  server.on("/wigle/test", HTTP_POST, handleWigleTest);
  server.on("/wigle/uploadAll", HTTP_POST, handleWigleUploadAll);
  server.on("/wigle/upload", HTTP_POST, handleWigleUploadOne);

  server.begin();
  Serial.println("[WEB] Server started");
}
