#pragma once
#include <Arduino.h>

static const char INDEX_HTML[] PROGMEM = R"HTML(
<!doctype html>
<html>
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Piglet Wardriver (T-Dongle C5)</title>
  <link rel="icon" href="data:image/svg+xml,<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 100 100'><text y='.9em' font-size='90'>&#x1f437;</text></svg>">
<style>
  *,*::before,*::after{box-sizing:border-box}
  :root{--bg:#0a0e13;--card:#111820;--text:#e2eaf4;--muted:#8899ab;--border:#1e2a3a;--input:#0c1219;--inputBorder:#243044;--accent:#2dd4bf;--accentDim:rgba(45,212,191,.15);--btn:#182030;--btnHover:#1f2d42;--primary:#2dd4bf;--primaryText:#0a0e13;--danger:#fb7185;--dangerDim:rgba(251,113,133,.12);--ok:#2dd4bf;--okDim:rgba(45,212,191,.12);--bad:#fb7185;--warn:#fbbf24;--warnDim:rgba(251,191,36,.12);--barBg:#182030}
  html{color-scheme:dark}
  body{font-family:system-ui,-apple-system,sans-serif;margin:0;padding:16px;max-width:900px;margin-left:auto;margin-right:auto;background:var(--bg);color:var(--text);line-height:1.5}
  .header{display:flex;align-items:center;gap:10px;margin-bottom:16px;padding-bottom:12px;border-bottom:1px solid var(--border)}
  .header .logo{font-size:28px} .header h1{font-size:20px;font-weight:700;margin:0} .header .sub{font-size:12px;color:var(--muted);margin:0}
  .card{border:1px solid var(--border);border-radius:12px;padding:14px;margin:12px 0;background:var(--card)}
  .card h3{margin:0 0 10px 0;font-size:13px;font-weight:600;text-transform:uppercase;letter-spacing:.5px;color:var(--muted)}
  input,select,textarea{padding:8px 10px;border-radius:7px;border:1px solid var(--inputBorder);width:100%;box-sizing:border-box;background:var(--input);color:var(--text);font-size:13px;font-family:inherit;outline:none}
  input:focus,select:focus,textarea:focus{border-color:var(--accent);box-shadow:0 0 0 2px var(--accentDim)}
  input::placeholder,textarea::placeholder{color:var(--muted);opacity:.6}
  textarea{resize:vertical;line-height:1.4}
  label{display:block;font-size:11px;font-weight:500;color:var(--muted);margin-bottom:3px;text-transform:uppercase;letter-spacing:.3px}
  button{cursor:pointer;padding:8px 14px;border-radius:7px;border:1px solid var(--inputBorder);background:var(--btn);color:var(--text);font-size:13px;width:100%;transition:background .1s}
  button:hover{background:var(--btnHover)} button:active{transform:translateY(1px)} button:disabled{opacity:.4;cursor:not-allowed}
  .btn-primary{background:var(--primary);color:var(--primaryText);border-color:var(--primary);font-weight:600}
  .btn-sm{padding:5px 10px;font-size:12px;width:auto}
  .btn-danger{color:var(--danger);border-color:rgba(251,113,133,.3);background:var(--dangerDim)}
  .row{display:grid;grid-template-columns:1fr 1fr;gap:8px}
  @media(max-width:520px){.row{grid-template-columns:1fr}}
  a{color:var(--accent);text-decoration:none} a:hover{text-decoration:underline}
  code{background:var(--input);padding:2px 6px;border-radius:5px;border:1px solid var(--border);font-size:12px}
  .statusGrid{display:flex;flex-wrap:wrap;gap:6px;margin-bottom:10px}
  .pill{display:inline-flex;align-items:center;gap:5px;border:1px solid var(--border);border-radius:999px;padding:4px 10px;font-size:12px;font-weight:500;background:var(--input)}
  .pill .dot{width:7px;height:7px;border-radius:50%;background:var(--muted);flex-shrink:0}
  .pill.ok .dot{background:var(--ok)} .pill.bad .dot{background:var(--bad)} .pill.warn .dot{background:var(--warn)}
  .pill.ok{border-color:rgba(45,212,191,.3);background:var(--okDim)} .pill.bad{border-color:rgba(251,113,133,.3);background:var(--dangerDim)} .pill.warn{border-color:rgba(251,191,36,.3);background:var(--warnDim)}
  .kv{display:grid;grid-template-columns:1fr;gap:6px} @media(min-width:600px){.kv{grid-template-columns:1fr 1fr}}
  .kv>div{display:flex;justify-content:space-between;gap:8px;border:1px solid var(--border);border-radius:7px;padding:8px 10px;background:var(--input)}
  .k{color:var(--muted);font-size:12px} .v{font-weight:600;font-size:13px;text-align:right}
  .barWrap{height:7px;border-radius:7px;overflow:hidden;margin-top:8px;background:var(--barBg)}
  #wigleBar{height:7px;width:0%;border-radius:7px;background:var(--accent);transition:width .4s}
  #wigleBar.active{animation:pulse 1.5s ease-in-out infinite} @keyframes pulse{0%,100%{opacity:1}50%{opacity:.6}}
  #fwModalBar{height:7px;width:0%;border-radius:7px;background:var(--accent);transition:width .4s}
  #fwModalBar.active{animation:pulse 1.5s ease-in-out infinite}
  #fwModalBar.indeterminate{width:30%!important;animation:barSweep 1.1s ease-in-out infinite;transition:none}
  @keyframes barSweep{0%{transform:translateX(-120%)}100%{transform:translateX(420%)}}
  .inner-card{border:1px solid var(--border);border-radius:8px;padding:12px;margin-top:12px;background:var(--input)}
  .cfg-grid{display:grid;grid-template-columns:1fr 1fr;gap:12px} @media(max-width:520px){.cfg-grid{grid-template-columns:1fr}} .cfg-grid>div{display:flex;flex-direction:column}
  .file-row{display:flex;align-items:center;gap:8px;flex-wrap:wrap;padding:6px 0;border-bottom:1px solid var(--border)} .file-row:last-child{border-bottom:none}
  .file-badge{font-size:10px;font-weight:600;text-transform:uppercase;padding:2px 6px;border-radius:999px}
  .file-badge.log{color:var(--danger);background:var(--dangerDim);border:1px solid rgba(251,113,133,.3)}
  .file-badge.uploaded{color:var(--muted);background:rgba(136,153,171,.1);border:1px solid rgba(136,153,171,.2)}
  .file-size{color:var(--muted);font-size:11px} .file-actions{margin-left:auto;display:flex;gap:4px}
  .muted{color:var(--muted)} .mt-sm{margin-top:8px} .mt-md{margin-top:12px}
</style>
</head>
<body>
  <div class="header"><div class="logo">&#x1f437;</div><div><h1>Piglet Wardriver</h1><p class="sub">T-Dongle C5 &mdash; ESP32-C5 Dual-Band &mdash; v2.51</p></div></div>

  <div class="card">
    <h3>Status</h3>
    <div class="statusGrid">
      <div class="pill" id="pillScan"><span class="dot"></span>Scan: —</div>
      <div class="pill" id="pillSd"><span class="dot"></span>SD: —</div>
      <div class="pill" id="pillGps"><span class="dot"></span>GPS: —</div>
      <div class="pill" id="pillSta"><span class="dot"></span>STA: —</div>
      <div class="pill" id="pillWigle"><span class="dot"></span>WiGLE: —</div>
    </div>
    <div class="kv">
      <div><span class="k">2.4 GHz</span><span class="v" id="vFound2g">—</span></div>
      <div><span class="k">5 GHz</span><span class="v" id="vFound5g">—</span></div>
      <div><span class="k">STA IP</span><span class="v" id="vStaIp">—</span></div>
      <div><span class="k">AP Clients</span><span class="v" id="vApSeen">—</span></div>
      <div><span class="k">Last Upload</span><span class="v" id="vLastUpload">—</span></div>
    </div>
    <div class="row mt-md">
      <button class="btn-primary" onclick="doStart()">&#9654; Start Scan</button>
      <button onclick="doStop()">&#9724; Stop Scan</button>
    </div>
    <div class="inner-card">
      <h3 style="margin:0 0 6px 0">WiGLE Upload</h3>
      <div id="wigleMsg" class="muted" style="font-size:13px">—</div>
      <div class="barWrap"><div id="wigleBar"></div></div>
      <div class="row mt-sm">
        <button onclick="wigleTest()">Test Token</button>
        <button class="btn-primary" onclick="wigleUploadAll()">Upload All CSVs</button>
      </div>
    </div>
    <div class="inner-card">
      <h3 style="margin:0 0 6px 0">WDGoWars</h3>
      <div id="wdgwarsMsg" class="muted" style="font-size:13px">—</div>
      <div class="row mt-sm">
        <button onclick="wdgwarsTest()">Test API Key</button>
        <button class="btn-primary" onclick="wdgwarsUploadAll()">Upload All CSVs</button>
      </div>
    </div>
    <div class="inner-card">
      <h3 style="margin:0 0 6px 0">Wardrift</h3>
      <div id="wardriftMsg" class="muted" style="font-size:13px">—</div>
      <div class="row mt-sm">
        <button onclick="wardriftTest()">Test API Key</button>
        <button class="btn-primary" onclick="wardriftUploadAll()">Upload All CSVs</button>
      </div>
    </div>
  </div>

  <div class="card">
    <h3>Firmware Update</h3>
    <div class="kv">
      <div><span class="k">Current</span><span class="v" id="vFwCurrent">—</span></div>
      <div><span class="k">Available</span><span class="v" id="vFwAvailable">—</span></div>
    </div>
    <div id="fwNotes" class="muted mt-sm" style="font-size:12px"></div>
    <div id="fwMsg" class="muted mt-sm" style="font-size:13px">—</div>
    <div class="row mt-sm">
      <button onclick="fwCheck()">Check for Update</button>
      <button class="btn-primary" id="fwDownloadBtn" onclick="fwDownloadOpen()" disabled>Download &amp; Install Firmware</button>
    </div>
    <div class="row mt-sm">
      <button id="fwStageNodeBtn" onclick="fwStageNode()">Store Piglet Node Firmware</button>
    </div>
    <div id="fwStageNodeMsg" class="muted mt-sm" style="font-size:12px"></div>
    <p style="margin-top:6px;font-size:11px;color:var(--muted)">Downloads the latest <code>PigletNode.ota.bin</code> to <code>/Firmware/</code> so this Core can push it to connected headless PigletNode nodes -- works even if this board's own firmware is current.</p>
    <div class="row mt-sm">
      <button class="btn-danger" id="fwClearBtn" onclick="fwClearFirmware()">Delete Staged Firmware Files</button>
    </div>
    <div id="fwClearMsg" class="muted mt-sm" style="font-size:11px"></div>
    <p style="margin-top:6px;font-size:11px;color:var(--muted)">Deletes everything in <code>/Firmware/</code> on the SD card -- use this to clear a stuck or partial download before retrying. Disabled while a download or install is active.</p>
  </div>

  <div class="card" id="meshNodesCard" style="display:none">
    <h3>Mesh Nodes</h3>
    <div id="meshNodes" style="font-size:13px">—</div>
    <div id="meshNodesLoadMsg" class="muted mt-sm" style="font-size:11px"></div>
    <div class="inner-card">
      <h3 style="margin:0 0 6px 0">Channel Overrides</h3>
      <div id="chOverrides" style="font-size:13px">—</div>
      <div class="row mt-sm">
        <input id="chOvrMac" placeholder="Node MAC, e.g. AA:BB:CC:DD:EE:FF" autocomplete="off" autocapitalize="off" spellcheck="false">
        <div class="row" style="grid-template-columns:1fr 1fr">
          <input id="chOvrStart" type="number" placeholder="Start ch">
          <input id="chOvrEnd" type="number" placeholder="End ch">
        </div>
      </div>
      <div class="row mt-sm"><button class="btn-primary" onclick="chOvrSet()">Set Override</button></div>
      <div id="chOvrMsg" class="muted mt-sm" style="font-size:12px"></div>
      <p style="margin-top:6px;font-size:11px;color:var(--muted)">Pins a node to a fixed channel range instead of the automatic even split. Channels must be valid (1-14, or 5GHz: 36-177).</p>
    </div>
  </div>

  <div class="card">
    <h3>Configuration</h3>
    <div class="cfg-grid">
      <div><label>WiGLE Basic Token</label><input id="wigleBasicToken" placeholder="Encoded token from wigle.net" autocomplete="off" autocapitalize="off" autocorrect="off" spellcheck="false"><a href="https://wigle.net/account" target="_blank" rel="noopener" style="font-size:11px;margin-top:4px;display:inline-block;color:#2dd4bf">&rarr; Get your API Key here</a></div>
      <div><label>WDGoWars API Key</label><input id="wdgwarsApiKey" placeholder="Key from wdgwars.pl/profile" autocomplete="off" autocapitalize="off" autocorrect="off" spellcheck="false"><a href="https://wdgwars.pl/profile/" target="_blank" rel="noopener" style="font-size:11px;margin-top:4px;display:inline-block;color:#2dd4bf">&rarr; Get your API Key here</a></div>
      <div><label>Wardrift API Key</label><input id="wardriftApiKey" placeholder="API key (wdk_...) from wardrift.net" autocomplete="off" autocapitalize="off" autocorrect="off" spellcheck="false"><a href="https://wardrift.net" target="_blank" rel="noopener" style="font-size:11px;margin-top:4px;display:inline-block;color:#2dd4bf">&rarr; Get your API Key at wardrift.net</a></div>
      <div><label>Device Name</label><input id="deviceName" placeholder="e.g. rover1 (optional, used in filenames)" autocomplete="off"></div>
      <div><label>GPS Baud</label><input id="gpsBaud" type="number" value="9600"></div>
      <div><label>Home SSID</label><input id="homeSsid" autocomplete="off"></div>
      <div><label>Home PSK</label><input id="homePsk" type="password" autocomplete="new-password"></div>
      <div><label>Wardriver SSID</label><input id="wardriverSsid" autocomplete="off"></div>
      <div><label>Wardriver PSK</label><input id="wardriverPsk" type="password" autocomplete="new-password"></div>
      <div><label>Scan Mode</label><select id="scanMode"><option value="aggressive">Aggressive</option><option value="powersaving">Power Saving</option></select></div>
      <div><label>Speed Units</label><select id="speedUnits"><option value="kmh">km/h</option><option value="mph">mph</option></select></div>
      <div><label>Max Boot Uploads (-1=all, 0=off)</label><input id="maxBootUploads" type="number" value="25" min="-1"></div>
      <div><label>Mesh Mode On Boot</label><select id="meshModeOnBoot"><option value="none">None</option><option value="core">Core</option><option value="node">Node</option></select></div>
      <div><label>Rotate Screen 180&deg; (requires reboot)</label><select id="rotateScreen180"><option value="false">Normal</option><option value="true">Rotated 180&deg;</option></select></div>
      <div><label>Upload When Home Network Seen</label><select id="uploadOnNetworkSeen"><option value="true">Enabled (default)</option><option value="false">Disabled</option></select></div>
      <div><label>Wardrive Go BLE Bridge (Mesh Node/Core page only)</label><select id="wardriveGoBridge"><option value="false">Disabled (default)</option><option value="true">Enabled</option></select></div>
      <div><label>Auto-Update Headless PigletNode Nodes (Core mode)</label><select id="autoUpdateHeadlessNodes"><option value="false">Disabled (default)</option><option value="true">Enabled</option></select></div>
    </div>
    <div class="mt-md">
      <label>Network Whitelist &mdash; SSIDs to Scan but Never Log (up to 10, one per line)</label>
      <textarea id="ssidWhitelist" rows="4" placeholder="e.g.&#10;MyHomeNetwork&#10;MyOfficeWifi"></textarea>
      <p style="margin:6px 0 0;font-size:11px;color:var(--muted)">Exact, case-sensitive match. Still counted in Found totals but never written to the CSV log.</p>
    </div>
    <div class="row mt-md">
      <button class="btn-primary" onclick="saveCfg()">Save Config</button>
      <button onclick="saveAndReboot()" title="Save and restart device">Save &amp; Reboot</button>
    </div>
    <p style="margin-top:10px;font-size:12px;color:var(--muted)">Stored at <code>/wardriver.cfg</code>. WiFi/GPS changes need a reboot.<br>
    <strong>Max Boot Uploads:</strong> <code>-1</code>=all &bull; <code>0</code>=disabled &bull; <code>1+</code>=capped. Upload All buttons above are always unlimited.</p>
  </div>

  <div class="card">
    <h3>SD Card Files</h3>
    <div style="display:flex;flex-wrap:wrap;gap:8px;margin-bottom:10px">
      <button class="btn-sm" onclick="loadFilesClick()">&#8635; Load Files</button>
      <button class="btn-sm" onclick="downloadAllClick()">&#11015; Download All (CSV.GZ)</button>
      <button class="btn-sm btn-danger" onclick="deleteAllLogs()">&#128465; Delete All</button>
    </div>
    <div id="files" style="font-size:13px">Click "Load Files" to view SD card contents.</div>
    <div style="margin-top:8px">
      <button id="filesLoadMoreBtn" class="btn-sm" style="display:none" onclick="loadMoreFiles()">Load More</button>
    </div>
  </div>

<!-- AP keep-alive prompt (shown ~30 s before the AP window expires) -->
<div id="apKeepAliveOverlay" style="display:none;position:fixed;inset:0;background:rgba(10,14,19,.92);z-index:9998;flex-direction:column;align-items:center;justify-content:center;text-align:center;gap:12px;padding:24px">
  <div style="font-size:40px">&#x23f3;</div>
  <h2 style="margin:0;color:#e2eaf4">Stay in WebUI?</h2>
  <p style="color:#8899ab;margin:0;font-size:13px;max-width:360px">The AP is about to close and the device will start scanning. Tap "Stay" to keep the WebUI open for another 5 minutes.</p>
  <p id="apKeepAliveCountdown" style="font-size:36px;font-weight:700;color:#2dd4bf;margin:0">30s</p>
  <div class="row">
    <button class="btn-primary" onclick="apKeepAliveYes()">Stay</button>
    <button onclick="apKeepAliveNo()">Start Scanning Now</button>
  </div>
</div>

<!-- Firmware download/install modal (shown while fwDownloadOpen() is active) -->
<div id="fwModalOverlay" style="display:none;position:fixed;inset:0;background:rgba(10,14,19,.92);z-index:9998;align-items:center;justify-content:center;padding:24px">
  <div class="card" style="max-width:360px;width:100%;margin:0;text-align:center;display:flex;flex-direction:column;align-items:center;gap:12px">
    <div style="font-size:36px">&#x1f437;</div>
    <h2 id="fwModalTitle" style="margin:0;color:#e2eaf4">Downloading Firmware...</h2>
    <p id="fwModalHint" style="color:#8899ab;margin:0;font-size:11px">The device pauses scanning, waits for memory to free up, then connects -- this can take up to a couple of minutes. Please wait.</p>
    <div class="barWrap" style="width:100%"><div id="fwModalBar"></div></div>
    <p id="fwModalPct" style="color:#8899ab;margin:0;font-size:13px">0%</p>
    <pre id="fwModalLog" style="width:100%;max-height:200px;overflow-y:auto;text-align:left;background:#0d1117;border:1px solid #30363d;border-radius:7px;padding:8px;font-size:11px;line-height:1.5;color:#8899ab;white-space:pre-wrap;word-break:break-word"></pre>
    <div class="row" style="width:100%">
      <button id="fwModalCancelBtn" onclick="fwDownloadModalCancel()">Cancel</button>
      <button id="fwModalCloseBtn" onclick="fwDownloadModalClose()" style="display:none">Close</button>
      <button class="btn-primary" id="fwModalInstallBtn" onclick="fwInstallNow()" style="display:none">Install Now</button>
    </div>
  </div>
</div>

<!-- Reboot overlay -->
<div id="rebootOverlay" style="display:none;position:fixed;inset:0;background:rgba(10,14,19,.97);z-index:9999;flex-direction:column;align-items:center;justify-content:center;text-align:center;gap:12px;padding:24px">
  <div style="font-size:40px">&#x1f437;</div>
  <h2 style="margin:0;color:#e2eaf4">Rebooting...</h2>
  <p id="rebootMsg" style="color:#8899ab;margin:0">Config saved &mdash; device is restarting.</p>
  <p id="rebootCountdown" style="font-size:36px;font-weight:700;color:#2dd4bf;margin:0"></p>
  <p style="color:#8899ab;font-size:12px">Reconnect and refresh once the device comes back online.</p>
</div>

<script>
function $(id){return document.getElementById(id)}
function setPill(id,text,cls){const el=$(id);if(!el)return;el.classList.remove('ok','bad','warn');if(cls)el.classList.add(cls);const dot=el.querySelector('.dot');el.textContent='';if(dot)el.appendChild(dot);el.appendChild(document.createTextNode(' '+text))}
function setText(id,val){const el=$(id);if(el)el.textContent=(val===undefined||val===null||val==='')?'\u2014':String(val)}
function setWigleMsg(t,cls){const el=$('wigleMsg');el.textContent=t;el.className=cls||'muted'}
function formatBytes(b){if(b<1024)return b+' B';if(b<1048576)return(b/1024).toFixed(1)+' KB';return(b/1048576).toFixed(1)+' MB'}
// UI button-press logging -- fire-and-forget, visible on the serial console
// so field debugging can see exactly which control was pressed.
function logBtn(label){try{ fetch('/uiLog?msg='+encodeURIComponent(label+' button pressed'),{method:'POST'}); }catch(e){}}
const maskedKeys=new Set(['homePsk']);
function setWdgwarsMsg(t,cls){const el=$('wdgwarsMsg');el.textContent=t;el.className=cls||'muted'}
function setWardriftMsg(t,cls){const el=$('wardriftMsg');el.textContent=t;el.className=cls||'muted'}

async function loadStatus(){try{
  const r=await fetch('/status.json');const j=await r.json();
  setPill('pillScan','Scan: '+(j.allowScan?'ACTIVE':'PAUSED'),j.allowScan?'ok':'warn');
  setPill('pillSd','SD: '+(j.sdOk?'OK':'FAIL'),j.sdOk?'ok':'bad');
  setPill('pillGps','GPS: '+(j.gpsFix?'LOCK':'NO FIX'),j.gpsFix?'ok':'warn');
  setPill('pillSta','STA: '+(j.wifiConnected?'ON':'OFF'),j.wifiConnected?'ok':'warn');
  const wC=(j.wigleTokenStatus===1)?'ok':(j.wigleTokenStatus===-1?'bad':'warn');
  setPill('pillWigle','WiGLE: '+(j.wigleTokenStatus===1?'Valid':j.wigleTokenStatus===-1?'Invalid':'Unknown'),wC);
  setText('vFound2g',j.found2g);setText('vFound5g',j.found5g);
  setText('vStaIp',j.wifiConnected?(j.staIp||'\u2014'):'\u2014');
  setText('vApSeen',j.apClientsSeen?'Yes':'No');
  const lu=j.uploadLastResult?j.uploadLastResult+' (HTTP '+(j.wigleLastHttpCode||'\u2014')+')':'\u2014';
  setText('vLastUpload',lu);
  for(const k of ['wigleBasicToken','wdgwarsApiKey','wardriftApiKey','deviceName','gpsBaud','homeSsid','wardriverSsid','wardriverPsk','scanMode','speedUnits','maxBootUploads','meshModeOnBoot','rotateScreen180','uploadOnNetworkSeen','wardriveGoBridge','autoUpdateHeadlessNodes']){
    if(j.config&&(k in j.config)){const v=String(j.config[k]);if(maskedKeys.has(k)&&(v===''||v==='(set)'))continue;const el=$(k);if(el)el.value=v}}
  if(j.config){
    const lines=[];
    for(let i=1;i<=10;i++){const v=j.config['ssidWhitelist'+i];if(v)lines.push(v);}
    const wlEl=$('ssidWhitelist');
    if(wlEl)wlEl.value=lines.join('\n');
  }
}catch(e){console.error(e)}}

const FILES_PAGE_SIZE=50;
let filesLoaded=false;
let filesOffset=0;
let filesLoading=false;

function renderFileRow(f){
  const isUp=f.name.startsWith('/uploaded');
  const badge=isUp?'<span class="file-badge uploaded">uploaded</span>':'<span class="file-badge log">Not Uploaded</span>';
  const upBtn=isUp?'':'<button class="btn-sm" onclick="wigleUploadOne(\''+f.name.replace(/'/g,"\\\'")+'\')">Upload</button>';
  return '<div class="file-row"><a href="/download?name='+encodeURIComponent(f.name)+'">'+f.name+'</a> '+badge+
    '<span class="file-size">'+formatBytes(f.size)+'</span><span class="file-actions">'+upBtn+
    '<button class="btn-sm btn-danger" onclick="delFile(\''+f.name.replace(/'/g,"\\\'")+'\')">Delete</button></span></div>'
}

function setFilesLoadMoreVisible(visible){const btn=$('filesLoadMoreBtn');if(btn)btn.style.display=visible?'':'none'}

async function fetchFilesPage(reset){
  if(filesLoading)return;
  filesLoading=true;
  setFilesLoadMoreVisible(false);
  const el=$('files');
  try{
    const r=await fetch('/files.json?offset='+filesOffset+'&limit='+FILES_PAGE_SIZE);
    const j=await r.json();
    if(!j.ok){el.textContent='SD not available';filesLoading=false;return}
    const rowsHtml=(j.files||[]).map(renderFileRow).join('');
    if(reset){
      el.innerHTML=(j.files&&j.files.length)?rowsHtml:'No files';
    }else if(rowsHtml){
      el.insertAdjacentHTML('beforeend',rowsHtml);
    }
    filesOffset+=(j.files?j.files.length:0);
    setFilesLoadMoreVisible(!!j.hasMore);
  }catch(e){
    console.error(e);
    if(reset)el.textContent='Failed to load files';
  }
  filesLoading=false;
}

async function loadFiles(){
  filesLoaded=true;
  filesOffset=0;
  $('files').innerHTML='Loading\u2026';
  await fetchFilesPage(true);
}

// Thin wrapper so the "Load Files" button logs a press every time it's
// clicked, without loadFiles() itself (reused internally to silently
// refresh the list after other actions) logging a false positive.
async function loadFilesClick(){logBtn('Load Files');await loadFiles();}

async function loadMoreFiles(){logBtn('Load More');await fetchFilesPage(false)}

function downloadAllClick(){logBtn('Download All (CSV.GZ)');window.location.href='/downloadAll';}

// Re-pulls the first page only if the operator has already opened the Files
// list this session -- keeps actions elsewhere on the page (WiGLE/WDGoWars/
// Wardrift uploads) from silently triggering a full directory walk when
// nobody is even looking at the Files card.
async function refreshFilesIfLoaded(){if(filesLoaded)await loadFiles()}

async function doStart(){logBtn('Start Scan');await fetch('/start',{method:'POST'});await loadStatus()}
async function doStop(){logBtn('Stop Scan');await fetch('/stop',{method:'POST'});await loadStatus()}
async function delFile(name){
  logBtn('Delete File ('+name+')');
  if(!confirm('Delete '+name+'?'))return;
  const el=$('files');
  el.innerHTML='<span style="color:#8899ab">Deleting...</span>';
  try{
    const r=await fetch('/delete?name='+encodeURIComponent(name),{method:'POST'});
    if(!r.ok){el.innerHTML='<span style="color:#fb7185">Delete failed ('+r.status+')</span>';return;}
  }catch(e){
    el.innerHTML='<span style="color:#fb7185">Delete error: '+e+'</span>';
    return;
  }
  await loadFiles();
}
async function deleteAllLogs(){
  logBtn('Delete All');
  if(!confirm('Delete ALL log files from /logs and /uploaded?\nThis cannot be undone.'))return;
  const el=$('files');
  el.innerHTML='<span style="color:#8899ab">Deleting all logs\u2026</span>';
  try{
    const r=await fetch('/deleteAll',{method:'POST'});
    if(!r.ok){el.innerHTML='<span style="color:#fb7185">Delete failed ('+r.status+')</span>';return;}
  }catch(e){
    el.innerHTML='<span style="color:#fb7185">Error: '+e+'</span>';
    return;
  }
  await loadFiles();
}

async function doSave(){
  const keys=['wigleBasicToken','wdgwarsApiKey','wardriftApiKey','deviceName','gpsBaud','homeSsid','homePsk','wardriverSsid','wardriverPsk','scanMode','speedUnits','maxBootUploads','meshModeOnBoot','rotateScreen180','uploadOnNetworkSeen','wardriveGoBridge','autoUpdateHeadlessNodes'];
  let body='# Saved from Web UI\n';
  for(const k of keys){const el=$(k);const v=el?(el.value??''):'';if(maskedKeys.has(k)&&v==='')continue;body+=k+'='+String(v).replace(/\r?\n/g,' ')+'\n'}
  const wlEl=$('ssidWhitelist');
  const wlLines=(wlEl?wlEl.value:'').split(/\r?\n/).map(s=>s.trim()).filter(s=>s.length>0).slice(0,10);
  for(let i=0;i<10;i++){body+='ssidWhitelist'+(i+1)+'='+(wlLines[i]||'')+'\n'}
  await fetch('/saveConfig',{method:'POST',headers:{'Content-Type':'text/plain'},body});
  await loadStatus();
}
async function saveCfg(){logBtn('Save Config');try{await doSave();alert('Config saved. Reboot to apply WiFi/GPS changes.')}catch(e){alert('Save failed: '+e)}}
async function saveAndReboot(){
  logBtn('Save & Reboot');
  try{await doSave();}catch(e){alert('Save failed: '+e);return;}
  const ov=$('rebootOverlay');ov.style.display='flex';
  try{await fetch('/reboot',{method:'POST'});}catch(e){}
  let t=12;$('rebootCountdown').textContent=t+'s';
  const iv=setInterval(()=>{t--;$('rebootCountdown').textContent=t>0?t+'s':'';if(t<=0){clearInterval(iv);$('rebootMsg').textContent='Device rebooted. Reconnect and refresh.';setTimeout(()=>location.reload(),2000);}},1000);
}
async function wigleTest(){logBtn('WiGLE Test Token');setWigleMsg('Testing...');try{const r=await fetch('/wigle/test',{method:'POST'});const j=await r.json().catch(()=>({ok:false,message:'Bad response'}));setWigleMsg(j.message||(j.ok?'Valid':'Invalid'),j.ok?'ok-text':'bad-text')}catch(e){setWigleMsg('Failed','bad-text')}await loadStatus()}
async function wigleUploadAll(){logBtn('WiGLE Upload All CSVs');setWigleMsg('Uploading...');try{const r=await fetch('/wigle/uploadAll',{method:'POST'});const j=await r.json().catch(()=>null);if(j){setWigleMsg(j.message||(j.ok?j.uploaded+' uploaded':'Failed'),j.ok?'ok-text':'bad-text')}}catch(e){setWigleMsg('Failed','bad-text')}await refreshFilesIfLoaded();await loadStatus()}
async function wigleUploadOne(name){logBtn('Upload File ('+name+')');setWigleMsg('Uploading '+name+'...');try{const r=await fetch('/wigle/upload?name='+encodeURIComponent(name),{method:'POST'});const j=await r.json().catch(()=>null);if(j)setWigleMsg(j.message||(j.ok?'OK':'Failed'),j.ok?'ok-text':'bad-text')}catch(e){setWigleMsg('Failed','bad-text')}await loadFiles();await loadStatus()}
async function wdgwarsTest(){logBtn('WDGoWars Test API Key');setWdgwarsMsg('Testing...');try{const r=await fetch('/wdgwars/test',{method:'POST'});const j=await r.json().catch(()=>({ok:false,message:'Bad response'}));setWdgwarsMsg(j.message||(j.ok?'Key valid':'Key invalid'),j.ok?'ok-text':'bad-text')}catch(e){setWdgwarsMsg('Failed','bad-text')}}
async function wdgwarsUploadAll(){logBtn('WDGoWars Upload All CSVs');setWdgwarsMsg('Uploading...');try{const r=await fetch('/wdgwars/uploadAll',{method:'POST'});const j=await r.json().catch(()=>null);if(j){setWdgwarsMsg(j.message||(j.ok?j.uploaded+' uploaded':'Failed'),j.ok?'ok-text':'bad-text')}}catch(e){setWdgwarsMsg('Failed','bad-text')}await refreshFilesIfLoaded();}
async function wardriftTest(){logBtn('Wardrift Test API Key');setWardriftMsg('Testing...');try{const r=await fetch('/wardrift/test',{method:'POST'});const j=await r.json().catch(()=>({ok:false,message:'Bad response'}));setWardriftMsg(j.message||(j.ok?'Key valid':'Key invalid'),j.ok?'ok-text':'bad-text')}catch(e){setWardriftMsg('Failed','bad-text')}}
async function wardriftUploadAll(){logBtn('Wardrift Upload All CSVs');setWardriftMsg('Uploading...');try{const r=await fetch('/wardrift/uploadAll',{method:'POST'});const j=await r.json().catch(()=>null);if(j){setWardriftMsg(j.message||(j.ok?j.uploaded+' uploaded':'Failed'),j.ok?'ok-text':'bad-text')}}catch(e){setWardriftMsg('Failed','bad-text')}await refreshFilesIfLoaded();}
let apModalShown=false,apModalDismissed=false,apModalIv=null;
function hideApKeepAlive(){const ov=$('apKeepAliveOverlay');if(ov)ov.style.display='none';if(apModalIv){clearInterval(apModalIv);apModalIv=null;}apModalShown=false;}
function showApKeepAlive(ms){const ov=$('apKeepAliveOverlay');if(!ov||apModalShown)return;apModalShown=true;ov.style.display='flex';let s=Math.max(0,Math.ceil(ms/1000));$('apKeepAliveCountdown').textContent=s+'s';apModalIv=setInterval(()=>{s--;if(s<=0){clearInterval(apModalIv);apModalIv=null;return;}$('apKeepAliveCountdown').textContent=s+'s';},1000);}
async function apKeepAliveYes(){logBtn('AP Keep-Alive Stay');try{await fetch('/extend',{method:'POST'});}catch(e){}apModalDismissed=true;hideApKeepAlive();}
async function apKeepAliveNo(){logBtn('AP Keep-Alive Start Scanning Now');hideApKeepAlive();try{await fetch('/start',{method:'POST'});}catch(e){}}
function handleApTimer(j){if(!j||!j.apActive){hideApKeepAlive();return;}const rem=j.apRemainingMs|0;const lead=j.apExtendPromptLeadMs|30000;if(rem>lead)apModalDismissed=false;if(!apModalDismissed&&rem>0&&rem<=lead)showApKeepAlive(rem);}
async function pollUpload(){try{const r=await fetch('/status.json');const j=await r.json();const up=!!j.uploading;const done=j.uploadDoneFiles||0;const total=j.uploadTotalFiles||0;const pct=total>0?Math.round((done/total)*100):0;const bar=$('wigleBar');bar.style.width=pct+'%';bar.classList.toggle('active',up);if(up){const fail=j.uploadFailedFiles||0;const failStr=fail>0?' F:'+fail:'';setWigleMsg('Uploading '+done+'/'+total+' ('+pct+'%)'+failStr)}handleApTimer(j);}catch(e){}}

function setFwMsg(t,cls){const el=$('fwMsg');el.textContent=t;el.className=cls||'muted'}
async function loadUpdateStatus(){try{
  const r=await fetch('/update/status');const j=await r.json();
  setText('vFwCurrent',j.currentVersion);
  setText('vFwAvailable',j.available?j.availableVersion:'Up to date');
  $('fwNotes').textContent=j.notes||'';
  $('fwDownloadBtn').disabled=!j.available||j.downloading||j.installing;
  $('fwClearBtn').disabled=j.downloading||j.installing;
  if(j.downloading||j.installing){/* modal poller owns fwMsg while active */}
  else if(j.downloadError)setFwMsg(j.downloadError,'bad-text');
  else if(j.installError)setFwMsg(j.installError,'bad-text');
  else if(j.downloaded)setFwMsg('Downloaded \u2014 ready to install','ok-text');
  else if(j.available)setFwMsg('Update available: '+j.availableVersion);
  else if(j.checkError)setFwMsg(j.checkError,'bad-text');
  else if(j.checked)setFwMsg('Up to date','ok-text');
  return j;
}catch(e){console.error(e)}}
async function fwCheck(){logBtn('Check for Update');setFwMsg('Checking...');try{const r=await fetch('/update/check',{method:'POST'});const j=await r.json().catch(()=>null);if(j&&!j.ok)setFwMsg(j.message||'Check failed','bad-text')}catch(e){setFwMsg('Check failed','bad-text')}await loadUpdateStatus();}
function setFwClearMsg(t,cls){const el=$('fwClearMsg');el.textContent=t;el.className=cls||'muted'}
async function fwClearFirmware(){
  logBtn('Delete Staged Firmware Files');
  if(!confirm('Delete all files in /Firmware on the SD card?\nThis removes any downloaded-but-not-installed image and anything staged for mesh nodes.'))return;
  setFwClearMsg('Deleting...');
  try{
    const r=await fetch('/update/clearFirmware',{method:'POST'});
    const j=await r.json().catch(()=>null);
    if(j&&j.ok)setFwClearMsg('Cleared','ok-text');
    else setFwClearMsg((j&&j.error)||'Delete failed','bad-text');
  }catch(e){setFwClearMsg('Request failed','bad-text')}
  await loadUpdateStatus();
}

let fwModalIv=null;
function fwModalShow(title){
  $('fwModalTitle').textContent=title;
  $('fwModalLog').textContent='';
  const bar=$('fwModalBar');
  bar.style.width='0%';
  bar.classList.remove('active','indeterminate');
  $('fwModalPct').textContent='Starting...';
  $('fwModalHint').style.display='';
  $('fwModalCancelBtn').style.display='';
  $('fwModalCloseBtn').style.display='none';
  $('fwModalInstallBtn').style.display='none';
  $('fwModalOverlay').style.display='flex';
}
function fwModalHide(){if(fwModalIv){clearInterval(fwModalIv);fwModalIv=null;}$('fwModalOverlay').style.display='none';}
function fwModalRender(j){
  $('fwModalLog').textContent=j.log||'';
  const log=$('fwModalLog');log.scrollTop=log.scrollHeight;
  const bar=$('fwModalBar');
  const haveSize=j.totalBytes>0;
  if(j.downloading&&!haveSize){
    // Connecting / following GitHub's redirect to the CDN -- no byte total
    // yet, so sweep an indeterminate bar and surface the stage text instead
    // of sitting stuck at 0% with no visible activity.
    bar.classList.remove('active');
    bar.classList.add('indeterminate');
    $('fwModalPct').textContent=j.downloadStage||'Connecting...';
    $('fwModalHint').style.display='';
  }else{
    const pct=haveSize?Math.round((j.downloadedBytes/j.totalBytes)*100):0;
    bar.classList.remove('indeterminate');
    bar.style.width=pct+'%';
    bar.classList.toggle('active',j.downloading);
    $('fwModalPct').textContent=(j.downloading?pct+'%':(j.downloadStage||''));
    $('fwModalHint').style.display='none';
  }
  if(j.downloaded&&!j.installing){
    $('fwModalTitle').textContent='Download Complete';
    $('fwModalCancelBtn').style.display='none';
    $('fwModalCloseBtn').style.display='';
    $('fwModalInstallBtn').style.display='';
  }
  if(j.installing){
    $('fwModalTitle').textContent='Installing Firmware...';
    $('fwModalCancelBtn').style.display='none';
    $('fwModalCloseBtn').style.display='none';
    $('fwModalInstallBtn').style.display='none';
  }
  if(!j.downloading&&!j.downloaded&&!j.installing&&j.downloadError){
    $('fwModalTitle').textContent='Download Failed';
    $('fwModalCancelBtn').style.display='none';
    $('fwModalCloseBtn').style.display='';
    $('fwModalInstallBtn').style.display='none';
  }
}
async function fwModalPoll(){const j=await loadUpdateStatus();if(j)fwModalRender(j);}
async function fwDownloadOpen(){
  logBtn('Download & Install Firmware');
  fwModalShow('Downloading Firmware...');
  try{
    const r=await fetch('/update/downloadStart',{method:'POST'});
    const j=await r.json().catch(()=>null);
    if(j&&!j.ok){
      $('fwModalLog').textContent=j.error||'Could not start download';
      $('fwModalCancelBtn').style.display='none';
      $('fwModalCloseBtn').style.display='';
      return;
    }
  }catch(e){
    $('fwModalLog').textContent='Request failed';
    $('fwModalCancelBtn').style.display='none';
    $('fwModalCloseBtn').style.display='';
    return;
  }
  fwModalIv=setInterval(fwModalPoll,500);
  fwModalPoll();
}
async function fwDownloadModalCancel(){logBtn('Cancel Firmware Download');try{await fetch('/update/downloadCancel',{method:'POST'});}catch(e){}fwModalHide();await loadUpdateStatus();}
async function fwDownloadModalClose(){fwModalHide();await loadUpdateStatus();}
async function fwInstallNow(){
  logBtn('Install Now');
  if(fwModalIv){clearInterval(fwModalIv);fwModalIv=null;}
  $('fwModalTitle').textContent='Installing Firmware...';
  $('fwModalCancelBtn').style.display='none';
  $('fwModalCloseBtn').style.display='none';
  $('fwModalInstallBtn').style.display='none';
  try{
    const r=await fetch('/update/installFromSD',{method:'POST'});
    const j=await r.json().catch(()=>null);
    if(j&&!j.ok){
      $('fwModalLog').textContent+=(j.error||'Install failed')+'\n';
      $('fwModalTitle').textContent='Install Failed';
      $('fwModalCloseBtn').style.display='';
    }
  }catch(e){
    $('fwModalTitle').textContent='Rebooting...';
    $('fwModalLog').textContent+='Device is rebooting...\n';
  }
  await loadUpdateStatus();
}
// Slowed from 2000ms -- the firmware modal has its own dedicated 500ms
// poller (fwModalIv) while active; a slower idle cadence leaves the narrow
// MALLOC_CAP_DMA pool (shared with GitHub's own TLS handshake) less
// contended at rest.
setInterval(loadUpdateStatus,5000);

function setFwStageNodeMsg(t,cls){const el=$('fwStageNodeMsg');el.textContent=t;el.className=cls||'muted'}
async function fwStageNode(){
  logBtn('Store Piglet Node Firmware');
  setFwStageNodeMsg('Downloading PigletNode.ota.bin...');$('fwStageNodeBtn').disabled=true;
  try{
    const r=await fetch('/update/stageNode',{method:'POST'});
    const j=await r.json().catch(()=>null);
    if(j&&j.ok)setFwStageNodeMsg('Staged OK','ok-text');
    else setFwStageNodeMsg((j&&j.error)||'Staging failed','bad-text');
  }catch(e){setFwStageNodeMsg('Request failed','bad-text')}
  $('fwStageNodeBtn').disabled=false;
  await loadMeshNodes();
}

function otaPhaseText(p){switch(p){case 1:return'Starting';case 2:return'Sending';case 3:return'Done';case 4:return'Failed';default:return'Idle';}}
// Self-rescheduling (setTimeout, not setInterval) so the cadence can adapt:
// fast (1000ms) for a few seconds around an active node OTA push, slow
// (6000ms) otherwise -- leaves the narrow MALLOC_CAP_DMA pool less
// contended at rest.
let meshNodesTimer=null;
let meshNodesBusyUntilMs=0;
function scheduleMeshNodes(ms){if(meshNodesTimer)clearTimeout(meshNodesTimer);meshNodesTimer=setTimeout(loadMeshNodes,ms);}
async function loadMeshNodes(){
  let nextMs=6000;
  try{
  const r=await fetch('/mesh/nodes.json');const j=await r.json();
  const card=$('meshNodesCard');
  if(!j.coreActive){card.style.display='none';scheduleMeshNodes(nextMs);return;}
  card.style.display='';
  const el=$('meshNodes');
  const ota=j.ota||{};
  const busy=ota.phase===1||ota.phase===2;
  if(busy)meshNodesBusyUntilMs=Date.now()+5000;
  nextMs=Date.now()<meshNodesBusyUntilMs?1000:6000;
  const loadEl=$('meshNodesLoadMsg');
  if(loadEl)loadEl.textContent=ota.loadMsg?('Staged on SD: '+ota.loadMsg):'';
  if(!j.nodes||j.nodes.length===0){el.innerHTML='<span class="muted">No nodes connected</span>';scheduleMeshNodes(nextMs);return;}
  el.innerHTML=j.nodes.map(n=>{
    const isBusy=busy&&ota.nodeIdx===n.idx;
    const pct=ota.totalBytes>0?Math.round((ota.sentBytes/ota.totalBytes)*100):0;
    let statusTxt=n.board+(n.isPiglet?'':' (non-Piglet)');
    if(isBusy)statusTxt+=' - '+otaPhaseText(ota.phase)+' '+pct+'%';
    else if(ota.nodeIdx===n.idx&&ota.msg)statusTxt+=' - '+ota.msg;
    const actionBtn=isBusy
      ?'<button class="btn-sm btn-danger" onclick="meshOtaCancel()">Cancel</button>'
      :'<button class="btn-sm" onclick="meshOtaStart('+n.idx+')"'+(busy?' disabled':'')+'>Update</button>';
    return '<div class="file-row"><span>'+n.mac+'</span><span class="file-size">'+statusTxt+'</span><span class="file-actions">'+actionBtn+'</span></div>';
  }).join('');
  }catch(e){console.error(e)}
  scheduleMeshNodes(nextMs);
}
async function meshOtaStart(idx){logBtn('Mesh Node Update (idx='+idx+')');try{const r=await fetch('/mesh/ota/start?idx='+idx,{method:'POST'});const j=await r.json().catch(()=>null);if(j&&!j.ok)alert(j.msg||'Could not start update')}catch(e){alert('Request failed')}await loadMeshNodes();}
async function meshOtaCancel(){logBtn('Mesh OTA Cancel');try{await fetch('/mesh/ota/cancel',{method:'POST'});}catch(e){}await loadMeshNodes();}
// loadMeshNodes() self-reschedules (see scheduleMeshNodes() above) -- no
// setInterval needed here.

function setChOvrMsg(t,cls){const el=$('chOvrMsg');el.textContent=t;el.className=cls||'muted'}
async function loadChOverrides(){try{
  const r=await fetch('/mesh/channelOverrides');const j=await r.json();
  const el=$('chOverrides');
  if(!j.overrides||j.overrides.length===0){el.innerHTML='<span class="muted">No overrides set</span>';return;}
  el.innerHTML=j.overrides.map(o=>
    '<div class="file-row"><span>'+o.mac+'</span><span class="file-size">ch '+o.chStart+'-'+o.chEnd+'</span>'
    +'<span class="file-actions"><button class="btn-sm btn-danger" onclick="chOvrClear(\''+o.mac+'\')">Remove</button></span></div>'
  ).join('');
}catch(e){console.error(e)}}
async function chOvrSet(){
  logBtn('Set Channel Override');
  const mac=($('chOvrMac').value||'').trim();
  const chStart=$('chOvrStart').value, chEnd=$('chOvrEnd').value;
  if(!mac||chStart===''||chEnd===''){setChOvrMsg('MAC, start, and end channel are required','bad-text');return;}
  setChOvrMsg('Saving...');
  try{
    const body='mac='+encodeURIComponent(mac)+'&chStart='+encodeURIComponent(chStart)+'&chEnd='+encodeURIComponent(chEnd);
    const r=await fetch('/mesh/channelOverrides',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body});
    const j=await r.json().catch(()=>null);
    if(j&&j.ok){setChOvrMsg('Saved','ok-text');$('chOvrMac').value='';$('chOvrStart').value='';$('chOvrEnd').value='';}
    else setChOvrMsg((j&&j.error)||'Save failed','bad-text');
  }catch(e){setChOvrMsg('Request failed','bad-text')}
  await loadChOverrides();
}
async function chOvrClear(mac){
  logBtn('Remove Channel Override ('+mac+')');
  try{await fetch('/mesh/channelOverrides/clear',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'mac='+encodeURIComponent(mac)});}catch(e){}
  await loadChOverrides();
}
// No recurring poll -- channel overrides only change via explicit admin
// action (chOvrSet()/chOvrClear() above already refresh this list right
// after mutating), so a background interval here was pure idle DMA-pool
// chatter with no benefit.

setInterval(pollUpload,1500);loadStatus();loadUpdateStatus();loadMeshNodes();loadChOverrides();
</script>
</body>
</html>
)HTML";
