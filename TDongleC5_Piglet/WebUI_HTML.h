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
  input,select{padding:8px 10px;border-radius:7px;border:1px solid var(--inputBorder);width:100%;background:var(--input);color:var(--text);font-size:13px;outline:none}
  input:focus,select:focus{border-color:var(--accent);box-shadow:0 0 0 2px var(--accentDim)}
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
  .inner-card{border:1px solid var(--border);border-radius:8px;padding:12px;margin-top:12px;background:var(--input)}
  .cfg-grid{display:grid;grid-template-columns:1fr 1fr;gap:12px} @media(max-width:520px){.cfg-grid{grid-template-columns:1fr}} .cfg-grid>div{display:flex;flex-direction:column}
  .file-row{display:flex;align-items:center;gap:8px;flex-wrap:wrap;padding:6px 0;border-bottom:1px solid var(--border)} .file-row:last-child{border-bottom:none}
  .file-badge{font-size:10px;font-weight:600;text-transform:uppercase;padding:2px 6px;border-radius:999px}
  .file-badge.log{color:var(--accent);background:var(--okDim);border:1px solid rgba(45,212,191,.3)}
  .file-badge.uploaded{color:var(--muted);background:rgba(136,153,171,.1);border:1px solid rgba(136,153,171,.2)}
  .file-size{color:var(--muted);font-size:11px} .file-actions{margin-left:auto;display:flex;gap:4px}
  .muted{color:var(--muted)} .mt-sm{margin-top:8px} .mt-md{margin-top:12px}
</style>
</head>
<body>
  <div class="header"><div class="logo">&#x1f437;</div><div><h1>Piglet Wardriver</h1><p class="sub">T-Dongle C5 &mdash; ESP32-C5 Dual-Band</p></div></div>

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
  </div>

  <div class="card">
    <h3>Configuration</h3>
    <div class="cfg-grid">
      <div><label>WiGLE Basic Token</label><input id="wigleBasicToken" placeholder="Encoded token from wigle.net"><a href="https://wigle.net/account" target="_blank" rel="noopener" style="font-size:11px;margin-top:4px;display:inline-block;color:#2dd4bf">&rarr; Get your API Key here</a></div>
      <div><label>WDGoWars API Key</label><input id="wdgwarsApiKey" placeholder="Key from wdgwars.pl/profile"><a href="https://wdgwars.pl/profile/" target="_blank" rel="noopener" style="font-size:11px;margin-top:4px;display:inline-block;color:#2dd4bf">&rarr; Get your API Key here</a></div>
      <div><label>GPS Baud</label><input id="gpsBaud" type="number" value="9600"></div>
      <div><label>Home SSID</label><input id="homeSsid"></div>
      <div><label>Home PSK</label><input id="homePsk" type="password"></div>
      <div><label>Wardriver SSID</label><input id="wardriverSsid"></div>
      <div><label>Wardriver PSK</label><input id="wardriverPsk" type="password"></div>
      <div><label>Scan Mode</label><select id="scanMode"><option value="aggressive">Aggressive</option><option value="powersaving">Power Saving</option></select></div>
      <div><label>Speed Units</label><select id="speedUnits"><option value="kmh">km/h</option><option value="mph">mph</option></select></div>
      <div><label>Max Boot Uploads (-1=all, 0=off)</label><input id="maxBootUploads" type="number" value="25" min="-1"></div>
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
    <div id="files" style="font-size:13px">Loading&hellip;</div>
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
const maskedKeys=new Set(['homePsk']);
function setWdgwarsMsg(t,cls){const el=$('wdgwarsMsg');el.textContent=t;el.className=cls||'muted'}

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
  for(const k of ['wigleBasicToken','wdgwarsApiKey','gpsBaud','homeSsid','wardriverSsid','wardriverPsk','scanMode','speedUnits','maxBootUploads']){
    if(j.config&&(k in j.config)){const v=String(j.config[k]);if(maskedKeys.has(k)&&(v===''||v==='(set)'))continue;const el=$(k);if(el)el.value=v}}
}catch(e){console.error(e)}}

async function loadFiles(){try{
  const r=await fetch('/files.json');const j=await r.json();const el=$('files');
  if(!j.ok){el.textContent='SD not available';return}
  if(!j.files||j.files.length===0){el.textContent='No files';return}
  el.innerHTML=j.files.map(f=>{
    const isUp=f.name.startsWith('/uploaded');
    const badge=isUp?'<span class="file-badge uploaded">uploaded</span>':'<span class="file-badge log">log</span>';
    const upBtn=isUp?'':'<button class="btn-sm" onclick="wigleUploadOne(\''+f.name.replace(/'/g,"\\\'")+'\')">Upload</button>';
    return '<div class="file-row"><a href="/download?name='+encodeURIComponent(f.name)+'">'+f.name+'</a> '+badge+
      '<span class="file-size">'+formatBytes(f.size)+'</span><span class="file-actions">'+upBtn+
      '<button class="btn-sm btn-danger" onclick="delFile(\''+f.name.replace(/'/g,"\\\'")+'\')">Delete</button></span></div>'
  }).join('')
}catch(e){console.error(e)}}

async function doStart(){await fetch('/start',{method:'POST'});await loadStatus()}
async function doStop(){await fetch('/stop',{method:'POST'});await loadStatus()}
async function delFile(name){
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

async function doSave(){
  const keys=['wigleBasicToken','wdgwarsApiKey','gpsBaud','homeSsid','homePsk','wardriverSsid','wardriverPsk','scanMode','speedUnits','maxBootUploads'];
  let body='# Saved from Web UI\n';
  for(const k of keys){const el=$(k);const v=el?(el.value??''):'';if(maskedKeys.has(k)&&v==='')continue;body+=k+'='+String(v).replace(/\r?\n/g,' ')+'\n'}
  await fetch('/saveConfig',{method:'POST',headers:{'Content-Type':'text/plain'},body});
  await loadStatus();
}
async function saveCfg(){try{await doSave();alert('Config saved. Reboot to apply WiFi/GPS changes.')}catch(e){alert('Save failed: '+e)}}
async function saveAndReboot(){
  try{await doSave();}catch(e){alert('Save failed: '+e);return;}
  const ov=$('rebootOverlay');ov.style.display='flex';
  try{await fetch('/reboot',{method:'POST'});}catch(e){}
  let t=12;$('rebootCountdown').textContent=t+'s';
  const iv=setInterval(()=>{t--;$('rebootCountdown').textContent=t>0?t+'s':'';if(t<=0){clearInterval(iv);$('rebootMsg').textContent='Device rebooted. Reconnect and refresh.';setTimeout(()=>location.reload(),2000);}},1000);
}
async function wigleTest(){setWigleMsg('Testing...');try{const r=await fetch('/wigle/test',{method:'POST'});const j=await r.json().catch(()=>({ok:false,message:'Bad response'}));setWigleMsg(j.message||(j.ok?'Valid':'Invalid'),j.ok?'ok-text':'bad-text')}catch(e){setWigleMsg('Failed','bad-text')}await loadStatus()}
async function wigleUploadAll(){setWigleMsg('Uploading...');try{const r=await fetch('/wigle/uploadAll',{method:'POST'});const j=await r.json().catch(()=>null);if(j){setWigleMsg(j.message||(j.ok?j.uploaded+' uploaded':'Failed'),j.ok?'ok-text':'bad-text')}}catch(e){setWigleMsg('Failed','bad-text')}await loadFiles();await loadStatus()}
async function wigleUploadOne(name){setWigleMsg('Uploading '+name+'...');try{const r=await fetch('/wigle/upload?name='+encodeURIComponent(name),{method:'POST'});const j=await r.json().catch(()=>null);if(j)setWigleMsg(j.message||(j.ok?'OK':'Failed'),j.ok?'ok-text':'bad-text')}catch(e){setWigleMsg('Failed','bad-text')}await loadFiles();await loadStatus()}
async function wdgwarsTest(){setWdgwarsMsg('Testing...');try{const r=await fetch('/wdgwars/test',{method:'POST'});const j=await r.json().catch(()=>({ok:false,message:'Bad response'}));setWdgwarsMsg(j.message||(j.ok?'Key valid':'Key invalid'),j.ok?'ok-text':'bad-text')}catch(e){setWdgwarsMsg('Failed','bad-text')}}
async function wdgwarsUploadAll(){setWdgwarsMsg('Uploading...');try{const r=await fetch('/wdgwars/uploadAll',{method:'POST'});const j=await r.json().catch(()=>null);if(j){setWdgwarsMsg(j.message||(j.ok?j.uploaded+' uploaded':'Failed'),j.ok?'ok-text':'bad-text')}}catch(e){setWdgwarsMsg('Failed','bad-text')}await loadFiles();}
async function pollUpload(){try{const r=await fetch('/status.json');const j=await r.json();const up=!!j.uploading;const done=j.uploadDoneFiles||0;const total=j.uploadTotalFiles||0;const pct=total>0?Math.round((done/total)*100):0;const bar=$('wigleBar');bar.style.width=pct+'%';bar.classList.toggle('active',up);if(up){const fail=j.uploadFailedFiles||0;const failStr=fail>0?' F:'+fail:'';setWigleMsg('Uploading '+done+'/'+total+' ('+pct+'%)'+failStr)}}catch(e){}}
setInterval(pollUpload,1500);loadStatus();loadFiles();
</script>
</body>
</html>
)HTML";
