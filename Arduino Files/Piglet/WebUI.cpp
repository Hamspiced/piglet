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
  <link rel="icon" href="data:image/svg+xml,<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 100 100'><text y='.9em' font-size='90'>&#x1f437;</text></svg>">
<style>
  *,*::before,*::after{box-sizing:border-box}

  :root{
    --bg:#0a0e13;
    --card:#111820;
    --cardHover:#151d28;
    --text:#e2eaf4;
    --muted:#8899ab;
    --border:#1e2a3a;
    --input:#0c1219;
    --inputBorder:#243044;
    --inputFocus:#2d4a6f;
    --accent:#2dd4bf;
    --accentDim:rgba(45,212,191,.15);
    --btn:#182030;
    --btnHover:#1f2d42;
    --btnText:#e2eaf4;
    --primary:#2dd4bf;
    --primaryText:#0a0e13;
    --danger:#fb7185;
    --dangerDim:rgba(251,113,133,.12);
    --warn:#fbbf24;
    --warnDim:rgba(251,191,36,.12);
    --ok:#2dd4bf;
    --okDim:rgba(45,212,191,.12);
    --bad:#fb7185;
    --barBg:#182030;
    --radius:10px;
    --shadow:0 4px 24px rgba(0,0,0,.35);
  }

  html{color-scheme:dark}

  body{
    font-family:'Inter',system-ui,-apple-system,'Segoe UI',Roboto,sans-serif;
    margin:0;padding:20px;
    max-width:920px;
    margin-left:auto;margin-right:auto;
    background:var(--bg);
    color:var(--text);
    line-height:1.5;
    -webkit-font-smoothing:antialiased;
  }

  /* ---- Header ---- */
  .header{
    display:flex;align-items:center;gap:12px;
    margin-bottom:20px;padding-bottom:16px;
    border-bottom:1px solid var(--border);
  }
  .header .logo{font-size:32px;line-height:1}
  .header h1{font-size:22px;font-weight:700;margin:0;letter-spacing:-.3px}
  .header .sub{font-size:13px;color:var(--muted);margin:0}

  /* ---- Cards ---- */
  .card{
    border:1px solid var(--border);
    border-radius:14px;
    padding:18px;
    margin:14px 0;
    background:var(--card);
    box-shadow:var(--shadow);
    transition:border-color .2s;
  }
  .card:hover{border-color:color-mix(in srgb,var(--border) 60%,var(--accent))}
  .card h3{margin:0 0 14px 0;font-size:15px;font-weight:600;text-transform:uppercase;letter-spacing:.6px;color:var(--muted)}
  .card h4{margin:0 0 8px 0;font-size:14px;font-weight:600;color:var(--muted)}
  .inner-card{
    border:1px solid var(--border);
    border-radius:var(--radius);
    padding:14px;
    margin-top:14px;
    background:var(--input);
  }

  /* ---- Inputs ---- */
  input,select{
    padding:9px 12px;
    border-radius:8px;
    border:1px solid var(--inputBorder);
    width:100%;box-sizing:border-box;
    background:var(--input);
    color:var(--text);
    font-size:14px;
    transition:border-color .15s,box-shadow .15s;
    outline:none;
  }
  input:focus,select:focus{
    border-color:var(--accent);
    box-shadow:0 0 0 3px var(--accentDim);
  }
  input::placeholder{color:var(--muted);opacity:.6}

  label{
    display:block;
    font-size:12px;
    font-weight:500;
    color:var(--muted);
    margin-bottom:4px;
    text-transform:uppercase;
    letter-spacing:.4px;
  }

  /* ---- Buttons ---- */
  button{
    cursor:pointer;
    padding:9px 16px;
    border-radius:8px;
    border:1px solid var(--inputBorder);
    background:var(--btn);
    color:var(--btnText);
    font-size:14px;font-weight:500;
    transition:all .12s ease;
    outline:none;
    width:100%;
  }
  button:hover{background:var(--btnHover);border-color:var(--inputFocus)}
  button:active{transform:translateY(1px)}
  button:disabled{opacity:.45;cursor:not-allowed;transform:none}

  .btn-primary{
    background:var(--primary);color:var(--primaryText);
    border-color:var(--primary);font-weight:600;
  }
  .btn-primary:hover{background:color-mix(in srgb,var(--primary) 85%,#fff);border-color:transparent}

  .btn-danger{
    color:var(--danger);border-color:color-mix(in srgb,var(--danger) 40%,transparent);
    background:var(--dangerDim);
  }
  .btn-danger:hover{background:color-mix(in srgb,var(--danger) 20%,transparent)}

  .btn-sm{padding:6px 12px;font-size:13px;width:auto}

  /* ---- Layout helpers ---- */
  .row{display:grid;grid-template-columns:1fr 1fr;gap:10px}
  @media(max-width:520px){.row{grid-template-columns:1fr}}

  a{color:var(--accent);text-decoration:none;font-weight:500}
  a:hover{text-decoration:underline}
  code{background:var(--input);padding:2px 7px;border-radius:6px;border:1px solid var(--border);font-size:13px}

  /* ---- Status pills ---- */
  .statusGrid{display:flex;flex-wrap:wrap;gap:8px;margin-bottom:14px}
  .pill{
    display:inline-flex;align-items:center;gap:6px;
    border:1px solid var(--border);
    border-radius:999px;padding:5px 12px;
    font-size:13px;font-weight:500;
    background:var(--input);color:var(--text);
    transition:border-color .2s,background .2s;
  }
  .pill .dot{
    width:8px;height:8px;border-radius:50%;
    background:var(--muted);
    flex-shrink:0;
  }
  .pill.ok  .dot{background:var(--ok)}
  .pill.bad .dot{background:var(--bad)}
  .pill.warn .dot{background:var(--warn)}
  .pill.ok{border-color:rgba(45,212,191,.35);background:var(--okDim)}
  .pill.bad{border-color:rgba(251,113,133,.35);background:var(--dangerDim)}
  .pill.warn{border-color:rgba(251,191,36,.35);background:var(--warnDim)}

  /* ---- Key-value grid ---- */
  .kv{display:grid;grid-template-columns:1fr;gap:8px}
  @media(min-width:700px){.kv{grid-template-columns:1fr 1fr}}
  .kv>div{
    display:flex;justify-content:space-between;align-items:center;
    gap:12px;border:1px solid var(--border);
    border-radius:8px;padding:10px 12px;
    background:var(--input);transition:border-color .15s;
  }
  .kv>div:hover{border-color:color-mix(in srgb,var(--border) 50%,var(--accent))}
  .k{color:var(--muted);font-size:13px}
  .v{font-weight:600;font-size:14px;color:var(--text);text-align:right}

  /* ---- Progress bar ---- */
  .barWrap{
    height:8px;border-radius:8px;overflow:hidden;
    margin-top:10px;background:var(--barBg);
  }
  #wigleBar{
    height:8px;width:0%;border-radius:8px;
    background:var(--accent);
    transition:width .4s ease;
  }
  #wigleBar.active{
    background:linear-gradient(90deg,var(--accent),color-mix(in srgb,var(--accent) 60%,#fff));
    animation:barPulse 1.5s ease-in-out infinite;
  }
  @keyframes barPulse{0%,100%{opacity:1}50%{opacity:.65}}

  /* ---- File list ---- */
  .file-row{
    display:flex;align-items:center;gap:10px;flex-wrap:wrap;
    padding:8px 0;
    border-bottom:1px solid var(--border);
  }
  .file-row:last-child{border-bottom:none}
  .file-name{font-weight:500;font-size:14px;min-width:0;word-break:break-all}
  .file-size{color:var(--muted);font-size:12px;white-space:nowrap}
  .file-badge{
    font-size:11px;font-weight:600;text-transform:uppercase;letter-spacing:.5px;
    padding:2px 8px;border-radius:999px;
  }
  .file-badge.log{color:var(--accent);background:var(--okDim);border:1px solid rgba(45,212,191,.3)}
  .file-badge.uploaded{color:var(--muted);background:rgba(136,153,171,.1);border:1px solid rgba(136,153,171,.25)}
  .file-stats{color:var(--accent);font-size:12px;font-weight:500;white-space:nowrap}
  .file-actions{margin-left:auto;display:flex;gap:6px}

  /* ---- Config form ---- */
  .cfg-grid{display:grid;grid-template-columns:1fr 1fr;gap:14px}
  @media(max-width:520px){.cfg-grid{grid-template-columns:1fr}}
  .cfg-grid>div{display:flex;flex-direction:column}

  /* ---- Details/Advanced ---- */
  details summary{
    cursor:pointer;font-size:13px;font-weight:500;
    color:var(--muted);padding:4px 0;
    transition:color .15s;
  }
  details summary:hover{color:var(--text)}
  details[open] summary{margin-bottom:8px}

  /* ---- WiGLE status text ---- */
  .ok-text{color:var(--ok)} .bad-text{color:var(--bad)} .warn-text{color:var(--warn)} .muted{color:var(--muted)}

  /* ---- Utility ---- */
  .mt-sm{margin-top:10px} .mt-md{margin-top:14px}
  .gap-sm{gap:8px}
</style>
</head>
<body>

  <div class="header">
    <div class="logo">&#x1f437;</div>
    <div>
      <h1>Piglet Wardriver</h1>
      <p class="sub">ESP32 Wi-Fi Scanner &amp; Logger</p>
    </div>
  </div>

  <!-- ============ STATUS ============ -->
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
      <div><span class="k">2.4 GHz Found</span><span class="v" id="vFound2g">—</span></div>
      <div id="row5g"><span class="k">5 GHz Found</span><span class="v" id="vFound5g">—</span></div>
      <div><span class="k">STA IP</span><span class="v" id="vStaIp">—</span></div>
      <div><span class="k">AP Clients Seen</span><span class="v" id="vApSeen">—</span></div>
      <div><span class="k">De-dupe Table</span><span class="v" id="vDedup">—</span></div>
      <div><span class="k">Last Upload</span><span class="v" id="vLastUpload">—</span></div>
    </div>

    <details class="mt-md">
      <summary>Advanced Details</summary>
      <div class="kv">
        <div><span class="k">Scan Mode</span><span class="v" id="vScanMode">—</span></div>
        <div><span class="k">GPS Baud</span><span class="v" id="vGpsBaud">—</span></div>
        <div><span class="k">Home SSID</span><span class="v" id="vHomeSsid">—</span></div>
        <div><span class="k">Wardriver SSID</span><span class="v" id="vApSsid">—</span></div>
      </div>
      <div class="mt-sm">
        <a href="/status.json" target="_blank" rel="noopener">View raw status.json &rarr;</a>
      </div>
    </details>

    <div class="row mt-md">
      <button class="btn-primary" onclick="doStart()">&#9654; Start Scan</button>
      <button onclick="doStop()">&#9724; Stop Scan</button>
    </div>

    <!-- WiGLE section -->
    <div class="inner-card">
      <h4>WiGLE Upload</h4>
      <div id="wigleMsg" class="muted" style="font-size:14px">—</div>
      <div class="barWrap">
        <div id="wigleBar"></div>
      </div>
      <div class="row mt-sm">
        <button onclick="wigleTest()">Test Token</button>
        <button class="btn-primary" onclick="wigleUploadAll()">Upload All CSVs</button>
      </div>
    </div>
  </div>

  <!-- ============ CONFIG ============ -->
  <div class="card">
    <h3>Configuration</h3>
    <div class="cfg-grid">
      <div>
        <label>WiGLE Basic Token</label>
        <input id="wigleBasicToken" placeholder="Enter 'Encoded for use' token from wigle.net/account">
      </div>
      <div><label>GPS Baud Rate</label><input id="gpsBaud" type="number" value="9600"></div>
      <div><label>Home SSID</label><input id="homeSsid" placeholder="Your home Wi-Fi"></div>
      <div><label>Home PSK</label><input id="homePsk" type="password" placeholder="Password"></div>
      <div><label>Wardriver SSID</label><input id="wardriverSsid"></div>
      <div><label>Wardriver PSK</label><input id="wardriverPsk" type="password"></div>
      <div><label>Scan Mode</label>
        <select id="scanMode">
          <option value="aggressive">Aggressive</option>
          <option value="powersaving">Power Saving</option>
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
          <option value="auto">Auto Detect</option>
          <option value="s3">XIAO S3</option>
          <option value="exp">S3 Expansion</option>
          <option value="c5">XIAO C5</option>
          <option value="c6">XIAO C6</option>
        </select>
      </div>
      <div><label>Battery ADC Pin (-1 = off)</label><input id="battPin" type="number" value="-1"></div>
      <div><label>Max Auto-Upload at Boot (0-25)</label><input id="maxBootUploads" type="number" value="5" min="0" max="25"></div>
    </div>
    <div class="mt-md" style="max-width:220px">
      <button class="btn-primary" onclick="saveCfg()">Save Config</button>
    </div>
    <p style="margin-top:12px;font-size:13px;color:var(--muted)">Stored at <code>/wardriver.cfg</code> on the SD card. WiFi &amp; GPS changes require a reboot.<br>
    <strong>Max Auto-Upload:</strong> WiGLE allows 25 API calls/day. Set to 5 (default) to conserve quota, or 0 to disable auto-upload.</p>
  </div>

  <!-- ============ FILES ============ -->
  <div class="card">
    <h3>SD Card Files</h3>
    <div id="files" style="font-size:14px">Loading&hellip;</div>
  </div>

<script>
/* ---- Helpers ---- */
function $(id){return document.getElementById(id)}

function setPill(id,text,cls){
  const el=$(id);if(!el)return;
  el.classList.remove('ok','bad','warn');
  if(cls)el.classList.add(cls);
  // preserve the dot span
  const dot=el.querySelector('.dot');
  el.textContent='';
  if(dot)el.appendChild(dot);
  el.appendChild(document.createTextNode(' '+text));
}

function wigleStatusText(s){
  if(s===1)return 'WiGLE: Valid';
  if(s===-1)return 'WiGLE: Invalid';
  return 'WiGLE: Unknown';
}

function setWigleMsg(t,cls){
  const el=$('wigleMsg');
  el.textContent=t;
  el.className=cls||'muted';
}

function setText(id,val){
  const el=$(id);
  if(el)el.textContent=(val===undefined||val===null||val==='')?'\u2014':String(val);
}

function formatBytes(b){
  if(b<1024)return b+' B';
  if(b<1048576)return (b/1024).toFixed(1)+' KB';
  return (b/1048576).toFixed(1)+' MB';
}

/* ---- Masked config keys that should not be filled back into form ---- */
const maskedKeys=new Set(['homePsk']);

/* ---- Status ---- */
async function loadStatus(){
  try{
    const r=await fetch('/status.json');
    const j=await r.json();

    setPill('pillScan','Scan: '+(j.allowScan?'ACTIVE':'PAUSED'),j.allowScan?'ok':'warn');
    setPill('pillSd','SD: '+(j.sdOk?'OK':'FAIL'),j.sdOk?'ok':'bad');
    setPill('pillGps','GPS: '+(j.gpsFix?'LOCK':'NO FIX'),j.gpsFix?'ok':'warn');
    setPill('pillSta','STA: '+(j.wifiConnected?'CONNECTED':'OFF'),j.wifiConnected?'ok':'warn');

    const wCls=(j.wigleTokenStatus===1)?'ok':(j.wigleTokenStatus===-1?'bad':'warn');
    setPill('pillWigle',wigleStatusText(j.wigleTokenStatus),wCls);

    setText('vFound2g',j.found2g);
    setText('vFound5g',j.found5g);

    const row5g=$('row5g');
    if(row5g)row5g.style.display=(j.c5Connected||j.found5g)?'':'none';
    setText('vStaIp',j.wifiConnected?(j.staIp||'\u2014'):'\u2014');
    setText('vApSeen',j.apClientsSeen?'Yes':'No');
    setText('vDedup',(j.seenCount||0)+' seen, '+(j.seenCollisions||0)+' collisions');

    const lastUp=j.uploadLastResult?j.uploadLastResult+' (HTTP '+(j.wigleLastHttpCode||'\u2014')+')':'\u2014';
    setText('vLastUpload',lastUp);

    setText('vScanMode',j?.config?.scanMode||'\u2014');
    setText('vGpsBaud',j?.config?.gpsBaud||'\u2014');
    setText('vHomeSsid',j?.config?.homeSsid||'\u2014');
    setText('vApSsid',j?.config?.wardriverSsid||'\u2014');

    // Fill config form — skip masked/secret values
    for(const k of ['wigleBasicToken','board','gpsBaud','homeSsid','wardriverSsid','wardriverPsk','scanMode','speedUnits','battPin','maxBootUploads']){
      if(j.config&&(k in j.config)){
        const v=String(j.config[k]);
        if(maskedKeys.has(k)&&(v===''||v==='(set)'))continue;
        const el=$(k);
        if(el)el.value=v;
      }
    }
  }catch(e){console.error('loadStatus',e)}
}

/* ---- Files ---- */
async function loadFiles(){
  try{
    const r=await fetch('/files.json');const j=await r.json();
    const el=$('files');
    if(!j.ok){el.textContent='SD card not available';return;}
    if(!j.files||j.files.length===0){el.textContent='No files on SD card';return;}

    el.innerHTML=j.files.map(f=>{
      const isUploaded=f.uploaded;
      let badge=isUploaded
        ?'<span class="file-badge uploaded">uploaded</span>'
        :'<span class="file-badge log">log</span>';
      
      // Add upload stats if available
      let statsText='';
      if(isUploaded&&f.discoveredGps!==undefined){
        if(f.processing){
          statsText='<span class="file-stats">Processing...</span>';
        }else{
          statsText='<span class="file-stats">'+f.discoveredGps+' new, '+f.totalGps+' total</span>';
        }
      }
      
      const uploadBtn=isUploaded?''
        :'<button class="btn-sm" onclick="wigleUploadOne(\''+f.name.replace(/'/g,"\\'")+'\')">Upload</button>';
      return '<div class="file-row">'
        +'<a href="/download?name='+encodeURIComponent(f.name)+'">'+f.name+'</a> '
        +badge
        +statsText
        +'<span class="file-size">'+formatBytes(f.size)+'</span>'
        +'<span class="file-actions">'
        +uploadBtn
        +'<button class="btn-sm btn-danger" onclick="delFile(\''+f.name.replace(/'/g,"\\'")+'\')">Delete</button>'
        +'</span></div>';
    }).join('');
  }catch(e){console.error('loadFiles',e)}
}

/* ---- Actions ---- */
async function doStart(){
  await fetch('/start',{method:'POST'});
  await loadStatus();
}
async function doStop(){
  await fetch('/stop',{method:'POST'});
  await loadStatus();
}

async function delFile(name){
  if(!confirm('Delete '+name+'?'))return;
  await fetch('/delete?name='+encodeURIComponent(name),{method:'POST'});
  loadFiles();
}

async function saveCfg(){
  const keys=['board','wigleBasicToken','gpsBaud','homeSsid','homePsk','wardriverSsid','wardriverPsk','scanMode','speedUnits','battPin','maxBootUploads'];
  let body='# Saved from Web UI\n# key=value\n';
  for(const k of keys){
    const el=$(k);
    const v=el?(el.value??''):'';
    // Don't write empty secret fields (user didn't change them)
    if(maskedKeys.has(k)&&v==='')continue;
    body+=k+'='+String(v).replace(/\r?\n/g,' ')+'\n';
  }
  await fetch('/saveConfig',{method:'POST',headers:{'Content-Type':'text/plain'},body});
  await loadStatus();
  alert('Saved. Reboot to apply WiFi/GPS changes.');
}

async function wigleTest(){
  setWigleMsg('Testing token\u2026');
  try{
    const r=await fetch('/wigle/test',{method:'POST'});
    const j=await r.json().catch(()=>({ok:false,message:'Bad response'}));
    setWigleMsg(j.message||(j.ok?'Token valid':'Token invalid'),j.ok?'ok-text':'bad-text');
  }catch(e){setWigleMsg('Request failed','bad-text')}
  await loadStatus();
}

async function wigleUploadAll(){
  setWigleMsg('Starting upload\u2026');
  try{
    const r=await fetch('/wigle/uploadAll',{method:'POST'});
    const j=await r.json().catch(()=>null);
    if(j){
      const msg=j.message||(j.ok?j.uploaded+' file(s) uploaded':'Upload failed');
      setWigleMsg(msg,j.ok?'ok-text':'bad-text');
    }
  }catch(e){setWigleMsg('Upload request failed','bad-text')}
  await loadFiles();
  await loadStatus();
}

async function wigleUploadOne(name){
  setWigleMsg('Uploading '+name+'\u2026');
  try{
    const r=await fetch('/wigle/upload?name='+encodeURIComponent(name),{method:'POST'});
    const j=await r.json().catch(()=>null);
    if(j)setWigleMsg(j.message||(j.ok?'Uploaded':'Failed'),j.ok?'ok-text':'bad-text');
  }catch(e){setWigleMsg('Upload failed','bad-text')}
  await loadFiles();
  await loadStatus();
}

/* ---- Upload progress poller ---- */
async function pollUpload(){
  try{
    const r=await fetch('/status.json');const j=await r.json();
    const up=!!j.uploading;
    const done=j.uploadDoneFiles||0;
    const total=j.uploadTotalFiles||0;
    const pct=total>0?Math.round((done/total)*100):0;

    const bar=$('wigleBar');
    bar.style.width=pct+'%';
    bar.classList.toggle('active',up);

    if(up)setWigleMsg('Uploading\u2026 '+done+'/'+total+' ('+pct+'%)');
  }catch(e){/* silent */}
}
setInterval(pollUpload,1500);
loadStatus();loadFiles();
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
  c["wigleBasicToken"] = cfg.wigleBasicToken;  // Show actual token, not masked
  c["homeSsid"] = cfg.homeSsid;
  c["homePsk"] = cfg.homePsk.length() ? "(set)" : "";
  c["wardriverSsid"] = cfg.wardriverSsid;
  c["wardriverPsk"] = cfg.wardriverPsk;
  c["gpsBaud"] = cfg.gpsBaud;
  c["scanMode"] = cfg.scanMode;
  c["board"] = cfg.board;
  c["speedUnits"] = cfg.speedUnits;
  c["battPin"] = cfg.battPin;
  c["maxBootUploads"] = cfg.maxBootUploads;

  String output;
  serializeJson(doc, output);
  server.send(200, "application/json", output);
}

static void addDirFiles(JsonArray arr, const char* dir) {
  File root = SD.open(dir);
  if (!root) return;

  bool isUploaded = (String(dir) == "/uploaded");

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
    o["uploaded"] = isUploaded;

    // If uploaded, try to find stats in wigleHistory
    if (isUploaded) {
      String basename = pathBasename(fullPath);
      uint32_t fsize = f.size();
      
      for (uint8_t i = 0; i < wigleHistoryCount; i++) {
        if (wigleHistory[i].basename == basename && wigleHistory[i].fileSize == fsize) {
          o["discoveredGps"] = wigleHistory[i].discoveredGps;
          o["totalGps"] = wigleHistory[i].totalGps;
          o["processing"] = wigleHistory[i].wait;
          break;
        }
      }
    }

    f.close();
    f = root.openNextFile();
  }

  root.close();
}

static void handleFiles() {
  // History is loaded after boot uploads, but refresh if cache expired (24h)
  // Only refresh when connected to home network (STA), not in AP mode
  wifi_mode_t mode = WiFi.getMode();
  bool isSTA = (mode == WIFI_STA || mode == WIFI_AP_STA);
  bool staConnected = (WiFi.status() == WL_CONNECTED);
  
  // Check if history cache is stale (older than 24h)
  uint32_t now = millis();
  const uint32_t CACHE_DURATION_MS = 86400000;  // 24 hours
  bool cacheExpired = (wigleHistoryLastLoadMs == 0 || (now - wigleHistoryLastLoadMs) > CACHE_DURATION_MS);
  
  if (isSTA && staConnected && cfg.wigleBasicToken.length() > 0 && !apWindowActive && cacheExpired) {
    Serial.println("[WEB] Refreshing WiGLE history (cache expired)...");
    wigleLoadHistory();
  }
  
  // Increased from 4096 to handle upload stats for many files
  StaticJsonDocument<8192> doc;
  doc["ok"] = sdOk;

  JsonArray arr = doc.createNestedArray("files");
  if (sdOk) {
    addDirFiles(arr, "/logs");
    addDirFiles(arr, "/uploaded");
  }

  // Check if JSON buffer overflowed
  if (doc.overflowed()) {
    Serial.println("[WEB] WARNING: files.json buffer overflow!");
    server.send(507, "application/json", "{\"ok\":false,\"error\":\"Buffer overflow\"}");
    return;
  }

  String output;
  serializeJson(doc, output);
  server.send(200, "application/json", output);
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
  
  // History will auto-refresh when user next accesses /files.json
  
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
    // History will auto-refresh when user next accesses /files.json
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
