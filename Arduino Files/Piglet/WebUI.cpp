#include "WebUI.h"
#include "Globals.h"
#include "Config.h"
#include "SDUtils.h"
#include "Display.h"
#include "WigleUpload.h"
#include "UploadState.h"
#include "GzipDeflate.h"
#include "MeshNode.h"
#include "FirmwareUpdate.h"
#include <ArduinoJson.h>
#include "esp_heap_caps.h"
#include "JsonPsramAllocator.h"

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
  input,select,textarea{
    padding:9px 12px;
    border-radius:8px;
    border:1px solid var(--inputBorder);
    width:100%;box-sizing:border-box;
    background:var(--input);
    color:var(--text);
    font-size:14px;
    font-family:inherit;
    transition:border-color .15s,box-shadow .15s;
    outline:none;
  }
  input:focus,select:focus,textarea:focus{
    border-color:var(--accent);
    box-shadow:0 0 0 3px var(--accentDim);
  }
  input::placeholder,textarea::placeholder{color:var(--muted);opacity:.6}
  textarea{resize:vertical;line-height:1.4}

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
  #fwModalBar{
    height:8px;width:0%;border-radius:8px;
    background:var(--accent);
    transition:width .4s ease;
  }
  #fwModalBar.active{
    background:linear-gradient(90deg,var(--accent),color-mix(in srgb,var(--accent) 60%,#fff));
    animation:barPulse 1.5s ease-in-out infinite;
  }
  @keyframes barPulse{0%,100%{opacity:1}50%{opacity:.65}}
  /* Indeterminate sweep -- shown while connecting/resolving a redirect,
     before a byte total is known, so the bar isn't just sitting stuck at
     0% width with no visible activity. */
  #fwModalBar.indeterminate{
    width:30%!important;
    background:linear-gradient(90deg,var(--accent),color-mix(in srgb,var(--accent) 60%,#fff));
    animation:barSweep 1.1s ease-in-out infinite;
    transition:none;
  }
  @keyframes barSweep{0%{transform:translateX(-120%)}100%{transform:translateX(420%)}}

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
  .file-badge.log{color:var(--danger);background:var(--dangerDim);border:1px solid rgba(251,113,133,.3)}
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
<p class="sub">ESP32 Wi-Fi Scanner &amp; Logger &mdash; v2.51</p>
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
      <div><span class="k">Last Upload</span>
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

    <!-- WDGoWars section -->
    <div class="inner-card">
      <h4>WDGoWars</h4>
      <div id="wdgwarsMsg" class="muted" style="font-size:14px">—</div>
      <div class="row mt-sm">
        <button onclick="wdgwarsTest()">Test API Key</button>
        <button class="btn-primary" onclick="wdgwarsUploadAll()">Upload All CSVs</button>
      </div>
    </div>

    <!-- Wardrift section -->
    <div class="inner-card">
      <h4>Wardrift</h4>
      <div id="wardriftMsg" class="muted" style="font-size:14px">—</div>
      <div class="row mt-sm">
        <button onclick="wardriftTest()">Test API Key</button>
        <button class="btn-primary" onclick="wardriftUploadAll()">Upload All CSVs</button>
      </div>
    </div>
  </div>

  <!-- ============ FIRMWARE UPDATE ============ -->
  <div class="card">
    <h3>Firmware Update</h3>
    <div class="kv">
      <div><span class="k">Current Version</span><span class="v" id="vFwCurrent">—</span></div>
      <div><span class="k">Latest Available</span><span class="v" id="vFwAvailable">—</span></div>
    </div>
    <div id="fwNotes" class="muted mt-sm" style="font-size:13px"></div>
    <div id="fwMsg" class="muted mt-sm" style="font-size:14px">—</div>
    <div class="row mt-sm">
      <button onclick="fwCheck()">Check for Update</button>
      <button class="btn-primary" id="fwDownloadBtn" onclick="fwDownloadOpen()" disabled>Download &amp; Install Firmware</button>
    </div>
    <p style="margin-top:12px;font-size:12px;color:var(--muted)">Downloads over HTTPS from GitHub Releases to the SD card first; only the size is checked before flashing (no code-signing). Requires STA Wi-Fi. You choose when to install once the download finishes, and the device reboots automatically once the install finishes.</p>
    <div class="row mt-sm">
      <button id="fwStageNodeBtn" onclick="fwStageNode()">Store Piglet Node Firmware</button>
    </div>
    <div id="fwStageNodeMsg" class="muted mt-sm" style="font-size:13px"></div>
    <p style="margin-top:8px;font-size:12px;color:var(--muted)">Downloads the latest <code>PigletNode.ota.bin</code> release asset to <code>/Firmware/</code> so this Core can push it to connected headless PigletNode nodes over ESP-NOW &mdash; works even if this board's own firmware is already current.</p>
    <div class="row mt-sm">
      <button class="btn-danger" id="fwClearBtn" onclick="fwClearFirmware()">Delete Staged Firmware Files</button>
    </div>
    <div id="fwClearMsg" class="muted mt-sm" style="font-size:12px"></div>
    <p style="margin-top:8px;font-size:12px;color:var(--muted)">Deletes everything currently in <code>/Firmware/</code> on the SD card (this board's downloaded-but-not-yet-installed image, plus anything staged for mesh nodes) &mdash; use this to clear out a stuck or partial download before retrying. Disabled while a download or install is in progress.</p>
  </div>

  <!-- ============ MESH NODES (Core mode only) ============ -->
  <div class="card" id="meshNodesCard" style="display:none">
    <h3>Mesh Nodes</h3>
    <div id="meshNodes" style="font-size:14px">—</div>
    <div id="meshNodesLoadMsg" class="muted mt-sm" style="font-size:12px"></div>
    <p style="margin-top:12px;font-size:12px;color:var(--muted)">Pushes a staged <code>/Firmware/&lt;Board&gt;.ota.bin</code> image to the selected node over ESP-NOW. The node verifies a SHA-256 of the whole image before switching boot partition, so a bad transfer leaves it running its old firmware.</p>

    <div class="inner-card">
      <h4>Channel Overrides</h4>
      <div id="chOverrides" style="font-size:14px">—</div>
      <div class="row mt-sm">
        <input id="chOvrMac" placeholder="Node MAC, e.g. AA:BB:CC:DD:EE:FF" autocomplete="off" autocapitalize="off" spellcheck="false">
        <div class="row" style="grid-template-columns:1fr 1fr">
          <input id="chOvrStart" type="number" placeholder="Start ch (e.g. 1)">
          <input id="chOvrEnd" type="number" placeholder="End ch (e.g. 14)">
        </div>
      </div>
      <div class="row mt-sm">
        <button class="btn-primary" onclick="chOvrSet()">Set Override</button>
      </div>
      <div id="chOvrMsg" class="muted mt-sm" style="font-size:13px"></div>
      <p style="margin-top:8px;font-size:12px;color:var(--muted)">Pins a node to a fixed channel range instead of the automatic even split -- e.g. a node with a directional antenna aimed at 5GHz only. Channel numbers must be valid Wi-Fi channels (1-14, or 5GHz: 36-177). Persisted to SD and survives a reboot.</p>
    </div>
  </div>

  <!-- ============ CONFIG ============ -->
  <div class="card">
    <h3>Configuration</h3>
    <div class="cfg-grid">
      <div>
        <label>WiGLE Basic Token</label>
        <input id="wigleBasicToken" placeholder="Enter 'Encoded for use' token from wigle.net/account" autocomplete="off" autocapitalize="off" autocorrect="off" spellcheck="false">
        <a href="https://wigle.net/account" target="_blank" rel="noopener" style="font-size:12px;margin-top:5px;display:inline-block">&rarr; Get your API Key here</a>
      </div>
      <div>
        <label>WDGoWars API Key</label>
        <input id="wdgwarsApiKey" placeholder="API key from wdgwars.pl/profile (leave empty to disable)" autocomplete="off" autocapitalize="off" autocorrect="off" spellcheck="false">
        <a href="https://wdgwars.pl/profile/" target="_blank" rel="noopener" style="font-size:12px;margin-top:5px;display:inline-block">&rarr; Get your API Key here</a>
      </div>
      <div>
        <label>Wardrift API Key</label>
        <input id="wardriftApiKey" placeholder="API key (wdk_...) from wardrift.net (leave empty to disable)" autocomplete="off" autocapitalize="off" autocorrect="off" spellcheck="false">
        <a href="https://wardrift.net" target="_blank" rel="noopener" style="font-size:12px;margin-top:5px;display:inline-block">&rarr; Get your API Key at wardrift.net</a>
      </div>
      <div>
        <label>Device Name</label>
        <input id="deviceName" placeholder="e.g. rover1 &mdash; identifies this device in WiGLE uploads (optional)" autocomplete="off">
      </div>
      <div><label>GPS Baud Rate</label><input id="gpsBaud" type="number" value="9600"></div>
      <div><label>Home SSID</label><input id="homeSsid" placeholder="Your home Wi-Fi" autocomplete="off"></div>
      <div><label>Home PSK</label><input id="homePsk" type="password" placeholder="Password" autocomplete="new-password"></div>
      <div><label>Wardriver SSID</label><input id="wardriverSsid" autocomplete="off"></div>
      <div><label>Wardriver PSK</label><input id="wardriverPsk" type="password" autocomplete="new-password"></div>
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
          <option value="c3">XIAO C3 (2.4 GHz, headless)</option>
        </select>
      </div>
      <div><label>Battery ADC Pin (-1 = off)</label><input id="battPin" type="number" value="-1"></div>
      <div><label>Battery Test (elapsed time log)</label>
        <select id="batteryTest">
          <option value="false">Disabled</option>
          <option value="true">Enabled</option>
        </select>
      </div>
      <div><label>Max Files to Auto-Upload at Boot (-1=all, 0=off)</label><input id="maxBootUploads" type="number" value="25" min="-1" max="999"></div>
      <div><label>Mesh Mode on Boot</label>
        <select id="meshModeOnBoot">
          <option value="none">None &mdash; Solo Wardriving (default)</option>
          <option value="node">Node &mdash; Mesh Scanner</option>
          <option value="core">Core &mdash; Mesh Coordinator</option>
        </select>
      </div>
      <div><label>Rotate Screen 180&deg; (requires reboot)</label>
        <select id="rotateScreen180">
          <option value="false">Normal</option>
          <option value="true">Rotated 180&deg;</option>
        </select>
      </div>
      <div><label>Upload When Home Network Seen (while wardriving)</label>
        <select id="uploadOnNetworkSeen" onchange="syncUploadOnNetworkSeen()">
          <option value="true">Enabled &mdash; Connect &amp; Upload Automatically (default)</option>
          <option value="false">Disabled</option>
        </select>
      </div>
      <div><label>Auto-Start Wardriving After Uploads (requires reboot)</label>
        <select id="autoStartAfterUpload" onchange="syncUploadOnNetworkSeen()">
          <option value="false">Disabled &mdash; Stay on Home Wi-Fi (default)</option>
          <option value="true">Enabled &mdash; Disconnect and Wardrive Immediately</option>
        </select>
      </div>
      <div><label>Wardrive Go BLE Bridge (Mesh Node/Core page only)</label>
        <select id="wardriveGoBridge">
          <option value="false">Disabled (default)</option>
          <option value="true">Enabled &mdash; Advertise to Wardrive Go / WarBLE Go</option>
        </select>
      </div>
      <div><label>Auto-Update Headless PigletNode Nodes (Core mode)</label>
        <select id="autoUpdateHeadlessNodes">
          <option value="false">Disabled (default)</option>
          <option value="true">Enabled &mdash; Push Staged Firmware Automatically</option>
        </select>
      </div>
    </div>
    <p style="margin-top:8px;font-size:12px;color:var(--muted)">
      <strong>Auto-Update Headless PigletNode Nodes:</strong> silently pushes the staged <code>/Firmware/PigletNode.ota.bin</code> image over ESP-NOW to any connected headless node running different firmware &mdash; no on-device confirmation, since those nodes have no screen. XIAO-based nodes always use the on-screen confirm prompt instead. Use an SD card rated at least 16GB for sustained-write reliability; use at your own risk.
    </p>
    <p style="margin-top:8px;font-size:12px;color:var(--muted)">
      <strong>Upload When Home Network Seen</strong> and <strong>Auto-Start Wardriving After Uploads</strong> are mutually exclusive &mdash; enabling Auto-Start always disables Upload When Home Network Seen.
    </p>
    <div class="mt-md">
      <label>Network Whitelist &mdash; SSIDs to Scan but Never Log (up to 10, one per line)</label>
      <textarea id="ssidWhitelist" rows="4" placeholder="e.g.&#10;MyHomeNetwork&#10;MyOfficeWifi"></textarea>
      <p style="margin:6px 0 0;font-size:12px;color:var(--muted)">Exact, case-sensitive match. These networks still count toward Found totals but are never written to the CSV log.</p>
    </div>
    <div class="row mt-md">
      <button class="btn-primary" onclick="saveCfg()">Save Config</button>
      <button onclick="saveAndReboot()" title="Save config then restart the device">Save &amp; Reboot</button>
    </div>
    <p style="margin-top:12px;font-size:13px;color:var(--muted)">Stored at <code>/wardriver.cfg</code> on the SD card. WiFi &amp; GPS changes require a reboot.<br>
    <strong>Max Files at Boot:</strong> <code>-1</code> = upload all files every boot &bull; <code>0</code> = disabled (use web buttons manually) &bull; <code>1+</code> = upload up to N files (WiGLE allows 25 API calls/day).
    <strong>Upload All CSVs</strong> buttons above always upload every file regardless of this setting.</p>
  </div>

  <!-- ============ FILES ============ -->
  <div class="card">
    <h3>SD Card Files</h3>
    <div style="display:flex;flex-wrap:wrap;gap:8px;margin-bottom:10px">
      <button class="btn-sm" onclick="loadFilesClick()">&#8635; Load Files</button>
      <button class="btn-sm" onclick="downloadAllClick()">&#11015; Download All (CSV.GZ)</button>
      <button class="btn-sm btn-danger" onclick="deleteAllLogs()">&#128465; Delete All</button>
    </div>
    <div id="files" style="font-size:14px">Click "Load Files" to view SD card contents.</div>
    <div style="margin-top:10px">
      <button id="filesLoadMoreBtn" class="btn-sm" style="display:none" onclick="loadMoreFiles()">Load More</button>
    </div>
  </div>

<!-- AP keep-alive prompt (shown ~30 s before the extended AP window expires) -->
<div id="apKeepAliveOverlay" style="display:none;position:fixed;inset:0;background:rgba(10,14,19,.92);z-index:9998;flex-direction:column;align-items:center;justify-content:center;text-align:center;gap:14px;padding:32px">
  <div style="font-size:48px">&#x23f3;</div>
  <h2 style="margin:0;font-size:22px">Stay in WebUI?</h2>
  <p style="color:var(--muted);margin:0;font-size:15px;max-width:420px">The AP is about to close and the device will start scanning. Tap "Stay" to keep the WebUI open for another 5 minutes.</p>
  <p id="apKeepAliveCountdown" style="font-size:44px;font-weight:700;color:var(--accent);margin:0">30s</p>
  <div class="row gap-sm">
    <button class="btn-primary" onclick="apKeepAliveYes()">Stay</button>
    <button onclick="apKeepAliveNo()">Start Scanning Now</button>
  </div>
</div>

<!-- Firmware download/install modal (shown while fwDownloadOpen() is active) -->
<div id="fwModalOverlay" style="display:none;position:fixed;inset:0;background:rgba(10,14,19,.92);z-index:9998;align-items:center;justify-content:center;padding:32px">
  <div class="card" style="max-width:420px;width:100%;margin:0;text-align:center;display:flex;flex-direction:column;align-items:center;gap:14px">
    <div style="font-size:40px">&#x1f437;</div>
    <h2 id="fwModalTitle" style="margin:0;font-size:22px">Downloading Firmware...</h2>
    <p id="fwModalHint" style="color:var(--muted);margin:0;font-size:12px">The device pauses scanning, waits for memory to free up, then connects &mdash; this can take up to a couple of minutes. Please wait.</p>
    <div class="barWrap" style="width:100%"><div id="fwModalBar"></div></div>
    <p id="fwModalPct" style="color:var(--muted);margin:0;font-size:14px">0%</p>
    <pre id="fwModalLog" style="width:100%;max-height:220px;overflow-y:auto;text-align:left;background:#0d1117;border:1px solid #30363d;border-radius:8px;padding:10px;font-size:12px;line-height:1.5;color:var(--muted);white-space:pre-wrap;word-break:break-word"></pre>
    <div class="row gap-sm" style="width:100%">
      <button id="fwModalCancelBtn" onclick="fwDownloadModalCancel()">Cancel</button>
      <button id="fwModalCloseBtn" onclick="fwDownloadModalClose()" style="display:none">Close</button>
      <button class="btn-primary" id="fwModalInstallBtn" onclick="fwInstallNow()" style="display:none">Install Now</button>
    </div>
  </div>
</div>

<!-- Reboot overlay (hidden until saveAndReboot() is triggered) -->
<div id="rebootOverlay" style="display:none;position:fixed;inset:0;background:rgba(10,14,19,.97);z-index:9999;flex-direction:column;align-items:center;justify-content:center;text-align:center;gap:14px;padding:32px">
  <div style="font-size:48px">&#x1f437;</div>
  <h2 style="margin:0;font-size:22px">Rebooting...</h2>
  <p id="rebootMsg" style="color:var(--muted);margin:0;font-size:15px">Config saved &mdash; device is restarting.</p>
  <p id="rebootCountdown" style="font-size:44px;font-weight:700;color:var(--accent);margin:0"></p>
  <p style="color:var(--muted);font-size:13px;margin-top:6px">Reconnect and refresh once the device comes back online.</p>
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

/* ---- UI button-press logging -- fire-and-forget, visible on the serial
   console so field debugging can see exactly which control was pressed. ---- */
function logBtn(label){
  try{ fetch('/uiLog?msg='+encodeURIComponent(label+' button pressed'),{method:'POST'}); }catch(e){}
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

    const lastUp=j.uploadLastResult?j.uploadLastResult+' (HTTP '+(j.wigleLastHttpCode||'\u2014')+')':'\u2014';
    setText('vLastUpload',lastUp);

    setText('vScanMode',j?.config?.scanMode||'\u2014');
    setText('vGpsBaud',j?.config?.gpsBaud||'\u2014');
    setText('vHomeSsid',j?.config?.homeSsid||'\u2014');
    setText('vApSsid',j?.config?.wardriverSsid||'\u2014');

    // Fill config form — skip masked/secret values
    for(const k of ['wigleBasicToken','wdgwarsApiKey','wardriftApiKey','deviceName','board','gpsBaud','homeSsid','wardriverSsid','wardriverPsk','scanMode','speedUnits','battPin','batteryTest','maxBootUploads','meshModeOnBoot','rotateScreen180','autoStartAfterUpload','uploadOnNetworkSeen','wardriveGoBridge','autoUpdateHeadlessNodes']){
      if(j.config&&(k in j.config)){
        const v=String(j.config[k]);
        if(maskedKeys.has(k)&&(v===''||v==='(set)'))continue;
        const el=$(k);
        if(el)el.value=v;
      }
    }

    // Network whitelist: join the 10 flat ssidWhitelistN keys into one
    // newline-separated textarea value (skipping empty/unused slots).
    if(j.config){
      const lines=[];
      for(let i=1;i<=10;i++){
        const v=j.config['ssidWhitelist'+i];
        if(v)lines.push(v);
      }
      const wlEl=$('ssidWhitelist');
      if(wlEl)wlEl.value=lines.join('\n');
    }

    syncUploadOnNetworkSeen();
  }catch(e){console.error('loadStatus',e)}
}

/* ---- Mutual exclusion: Upload On Network Seen vs Auto-Start After Upload ----
   The two features can never both be active. Auto-Start wins: enabling it
   forces Upload On Network Seen off and greys it out client-side; the
   firmware enforces the same rule server-side on load/save regardless of
   what the client sends. */
function syncUploadOnNetworkSeen(){
  const auto=$('autoStartAfterUpload');
  const uns=$('uploadOnNetworkSeen');
  if(!auto||!uns)return;
  const autoOn=auto.value==='true';
  uns.disabled=autoOn;
  if(autoOn)uns.value='false';
}

/* ---- Files (loaded on demand, paginated 50 at a time) ---- */
const FILES_PAGE_SIZE=50;
let filesLoaded=false;
let filesOffset=0;
let filesLoading=false;

function renderFileRow(f){
  const isUploaded=f.uploaded;
  let badge=isUploaded
    ?'<span class="file-badge uploaded">uploaded</span>'
    :'<span class="file-badge log">Not Uploaded</span>';

  const uploadBtn=isUploaded?''
    :'<button class="btn-sm" onclick="wigleUploadOne(\''+f.name.replace(/'/g,"\\'")+'\')">Upload</button>';
  return '<div class="file-row">'
    +'<a href="/download?name='+encodeURIComponent(f.name)+'">'+f.name+'</a> '
    +badge
    +'<span class="file-size">'+formatBytes(f.size)+'</span>'
    +'<span class="file-actions">'
    +uploadBtn
    +'<button class="btn-sm btn-danger" onclick="delFile(\''+f.name.replace(/'/g,"\\'")+'\')">Delete</button>'
    +'</span></div>';
}

function setFilesLoadMoreVisible(visible){
  const btn=$('filesLoadMoreBtn');
  if(btn)btn.style.display=visible?'':'none';
}

async function fetchFilesPage(reset){
  if(filesLoading)return;
  filesLoading=true;
  setFilesLoadMoreVisible(false);
  const el=$('files');
  try{
    const r=await fetch('/files.json?offset='+filesOffset+'&limit='+FILES_PAGE_SIZE);
    const j=await r.json();
    if(!j.ok){el.innerHTML='SD card not available';filesLoading=false;return;}

    const rowsHtml=(j.files||[]).map(renderFileRow).join('');
    if(reset){
      el.innerHTML=(j.files&&j.files.length)?rowsHtml:'No files on SD card';
    }else if(rowsHtml){
      el.insertAdjacentHTML('beforeend',rowsHtml);
    }
    filesOffset+=(j.files?j.files.length:0);
    setFilesLoadMoreVisible(!!j.hasMore);
  }catch(e){
    console.error('loadFiles',e);
    if(reset)el.innerHTML='Failed to load files';
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
async function loadFilesClick(){
  logBtn('Load Files');
  await loadFiles();
}

async function loadMoreFiles(){
  logBtn('Load More');
  await fetchFilesPage(false);
}

function downloadAllClick(){
  logBtn('Download All (CSV.GZ)');
  window.location.href='/downloadAll';
}

// Re-pulls the first page only if the operator has already opened the Files
// list this session -- keeps actions elsewhere on the page (WiGLE/WDGoWars/
// Wardrift uploads) from silently triggering a full directory walk when
// nobody is even looking at the Files card.
async function refreshFilesIfLoaded(){
  if(filesLoaded)await loadFiles();
}

/* ---- Actions ---- */
async function doStart(){
  logBtn('Start Scan');
  await fetch('/start',{method:'POST'});
  await loadStatus();
}
async function doStop(){
  logBtn('Stop Scan');
  await fetch('/stop',{method:'POST'});
  await loadStatus();
}

async function delFile(name){
  logBtn('Delete File ('+name+')');
  if(!confirm('Delete '+name+'?'))return;
  const el=$('files');
  const prev=el.innerHTML;
  el.innerHTML='<span style="color:var(--muted)">Deleting...</span>';
  try{
    const r=await fetch('/delete?name='+encodeURIComponent(name),{method:'POST'});
    if(!r.ok) el.innerHTML='<span style="color:var(--bad)">Delete failed ('+r.status+')</span>';
  }catch(e){
    el.innerHTML='<span style="color:var(--bad)">Delete error: '+e+'</span>';
    return;
  }
  await loadFiles();
}

async function deleteAllLogs(){
  logBtn('Delete All');
  if(!confirm('Delete ALL log files from /logs and /uploaded?\nThis cannot be undone.'))return;
  const el=$('files');
  el.innerHTML='<span style="color:var(--muted)">Deleting all logs\u2026</span>';
  try{
    const r=await fetch('/deleteAll',{method:'POST'});
    const j=await r.json().catch(()=>null);
    if(!r.ok){el.innerHTML='<span style="color:var(--bad)">Delete failed ('+r.status+')</span>';return;}
  }catch(e){
    el.innerHTML='<span style="color:var(--bad)">Error: '+e+'</span>';
    return;
  }
  await loadFiles();
}

/* ---- Shared save logic used by both Save and Save+Reboot ---- */
async function doSave(){
  const keys=['board','wigleBasicToken','wdgwarsApiKey','wardriftApiKey','deviceName','gpsBaud','homeSsid','homePsk','wardriverSsid','wardriverPsk','scanMode','speedUnits','battPin','batteryTest','maxBootUploads','meshModeOnBoot','rotateScreen180','autoStartAfterUpload','uploadOnNetworkSeen','wardriveGoBridge','autoUpdateHeadlessNodes'];
  let body='# Saved from Web UI\n# key=value\n';
  for(const k of keys){
    const el=$(k);
    const v=el?(el.value??''):'';
    if(maskedKeys.has(k)&&v==='')continue;
    body+=k+'='+String(v).replace(/\r?\n/g,' ')+'\n';
  }

  // Network whitelist: split the textarea into non-empty, trimmed lines
  // (capped at 10), then always emit all 10 ssidWhitelistN keys -- including
  // blank ones -- so removing an entry actually clears that slot on save
  // instead of leaving a stale value behind.
  const wlEl=$('ssidWhitelist');
  const wlLines=(wlEl?wlEl.value:'').split(/\r?\n/).map(s=>s.trim()).filter(s=>s.length>0).slice(0,10);
  for(let i=0;i<10;i++){
    body+='ssidWhitelist'+(i+1)+'='+(wlLines[i]||'')+'\n';
  }

  const r=await fetch('/saveConfig',{method:'POST',headers:{'Content-Type':'text/plain'},body});
  await loadStatus();
  return r;
}

async function saveCfg(){
  logBtn('Save Config');
  try{
    await doSave();
    alert('Config saved. Reboot the device to apply WiFi / GPS changes.');
  }catch(e){alert('Save failed: '+e);}
}

async function saveAndReboot(){
  logBtn('Save & Reboot');
  try{ await doSave(); }catch(e){ alert('Save failed: '+e); return; }

  // Show the reboot overlay
  const ov=$('rebootOverlay');
  ov.style.display='flex';

  // Ask the device to reboot (fire-and-forget; it will restart before responding)
  try{ await fetch('/reboot',{method:'POST'}); }catch(e){}

  // Count down while the device restarts (~10 s)
  let t=12;
  $('rebootCountdown').textContent=t+'s';
  const iv=setInterval(()=>{
    t--;
    $('rebootCountdown').textContent=t>0?t+'s':'';
    if(t<=0){
      clearInterval(iv);
      $('rebootMsg').textContent='Device rebooted. Reconnect and refresh this page.';
      // Attempt to auto-reload after a further 2 s
      setTimeout(()=>location.reload(),2000);
    }
  },1000);
}

function setWdgwarsMsg(t,cls){
  const el=$('wdgwarsMsg');
  el.textContent=t;
  el.className=cls||'muted';
}

async function wdgwarsTest(){
  logBtn('WDGoWars Test API Key');
  setWdgwarsMsg('Testing key\u2026');
  try{
    const r=await fetch('/wdgwars/test',{method:'POST'});
    const j=await r.json().catch(()=>({ok:false,message:'Bad response'}));
    setWdgwarsMsg(j.message||(j.ok?'Key valid':'Key invalid'),j.ok?'ok-text':'bad-text');
  }catch(e){setWdgwarsMsg('Request failed','bad-text')}
}

async function wdgwarsUploadAll(){
  logBtn('WDGoWars Upload All CSVs');
  setWdgwarsMsg('Uploading all CSVs to WDGoWars\u2026');
  try{
    const r=await fetch('/wdgwars/uploadAll',{method:'POST'});
    const j=await r.json().catch(()=>null);
    if(j){
      const msg=j.message||(j.ok?j.uploaded+' file(s) uploaded':'Upload failed');
      setWdgwarsMsg(msg,j.ok?'ok-text':'bad-text');
    }
  }catch(e){setWdgwarsMsg('Upload request failed','bad-text')}
  await refreshFilesIfLoaded();
}

function setWardriftMsg(t,cls){
  const el=$('wardriftMsg');
  el.textContent=t;
  el.className=cls||'muted';
}

async function wardriftTest(){
  logBtn('Wardrift Test API Key');
  setWardriftMsg('Testing key\u2026');
  try{
    const r=await fetch('/wardrift/test',{method:'POST'});
    const j=await r.json().catch(()=>({ok:false,message:'Bad response'}));
    setWardriftMsg(j.message||(j.ok?'Key valid':'Key invalid'),j.ok?'ok-text':'bad-text');
  }catch(e){setWardriftMsg('Request failed','bad-text')}
}

async function wardriftUploadAll(){
  logBtn('Wardrift Upload All CSVs');
  setWardriftMsg('Uploading all CSVs to Wardrift\u2026');
  try{
    const r=await fetch('/wardrift/uploadAll',{method:'POST'});
    const j=await r.json().catch(()=>null);
    if(j){
      const msg=j.message||(j.ok?j.uploaded+' file(s) uploaded':'Upload failed');
      setWardriftMsg(msg,j.ok?'ok-text':'bad-text');
    }
  }catch(e){setWardriftMsg('Upload request failed','bad-text')}
  await refreshFilesIfLoaded();
}

async function wigleTest(){
  logBtn('WiGLE Test Token');
  setWigleMsg('Testing token\u2026');
  try{
    const r=await fetch('/wigle/test',{method:'POST'});
    const j=await r.json().catch(()=>({ok:false,message:'Bad response'}));
    setWigleMsg(j.message||(j.ok?'Token valid':'Token invalid'),j.ok?'ok-text':'bad-text');
  }catch(e){setWigleMsg('Request failed','bad-text')}
  await loadStatus();
}

async function wigleUploadAll(){
  logBtn('WiGLE Upload All CSVs');
  setWigleMsg('Starting upload\u2026');
  try{
    const r=await fetch('/wigle/uploadAll',{method:'POST'});
    const j=await r.json().catch(()=>null);
    if(j){
      const msg=j.message||(j.ok?j.uploaded+' file(s) uploaded':'Upload failed');
      setWigleMsg(msg,j.ok?'ok-text':'bad-text');
    }
  }catch(e){setWigleMsg('Upload request failed','bad-text')}
  await refreshFilesIfLoaded();
  await loadStatus();
}

async function wigleUploadOne(name){
  logBtn('Upload File ('+name+')');
  setWigleMsg('Uploading '+name+'\u2026');
  try{
    const r=await fetch('/wigle/upload?name='+encodeURIComponent(name),{method:'POST'});
    const j=await r.json().catch(()=>null);
    if(j)setWigleMsg(j.message||(j.ok?'Uploaded':'Failed'),j.ok?'ok-text':'bad-text');
  }catch(e){setWigleMsg('Upload failed','bad-text')}
  await loadFiles();
  await loadStatus();
}

/* ---- AP keep-alive modal ----
   The firmware reports apActive / apRemainingMs / apExtendPromptLeadMs on
   /status.json. When the AP is up and remaining time is at or below the lead
   window, we show the modal. "Stay" hits /extend (resets the 5 min timer).
   "Start Scanning Now" hits /start (force-closes the AP). */
let apModalShown=false;
let apModalDismissed=false;
let apModalIv=null;

function hideApKeepAlive(){
  const ov=$('apKeepAliveOverlay');
  if(ov)ov.style.display='none';
  if(apModalIv){clearInterval(apModalIv);apModalIv=null;}
  apModalShown=false;
}

function showApKeepAlive(initialRemainingMs){
  const ov=$('apKeepAliveOverlay');
  if(!ov||apModalShown)return;
  apModalShown=true;
  ov.style.display='flex';
  let remaining=Math.max(0,Math.ceil(initialRemainingMs/1000));
  $('apKeepAliveCountdown').textContent=remaining+'s';
  apModalIv=setInterval(()=>{
    remaining--;
    if(remaining<=0){clearInterval(apModalIv);apModalIv=null;return;}
    $('apKeepAliveCountdown').textContent=remaining+'s';
  },1000);
}

async function apKeepAliveYes(){
  logBtn('AP Keep-Alive Stay');
  try{await fetch('/extend',{method:'POST'});}catch(e){}
  apModalDismissed=true;  // require remaining to recover before re-showing
  hideApKeepAlive();
}
async function apKeepAliveNo(){
  logBtn('AP Keep-Alive Start Scanning Now');
  hideApKeepAlive();
  try{await fetch('/start',{method:'POST'});}catch(e){}
  // AP will go down; the page becomes unreachable until reconnect.
}

function handleApTimer(j){
  if(!j||!j.apActive){hideApKeepAlive();return;}
  const remaining=j.apRemainingMs|0;
  const lead=j.apExtendPromptLeadMs|30000;
  // Re-arm the prompt once remaining recovers above the lead (post-extend).
  if(remaining>lead)apModalDismissed=false;
  if(!apModalDismissed&&remaining>0&&remaining<=lead){
    showApKeepAlive(remaining);
  }
}

/* ---- Upload progress poller (also drives the AP keep-alive modal) ---- */
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

    handleApTimer(j);
  }catch(e){/* silent */}
}

/* ---- Firmware Update (self-update from GitHub Releases) ----
   Two-step flow: "Download & Install Firmware" opens a modal and kicks off
   a non-blocking download to the SD card (fwDownloadStart(), ticked from
   loop() so the modal can poll live progress). Once the download finishes,
   the modal swaps its Cancel button for "Install Now", which triggers a
   quick local SD->flash step and reboot. */
function setFwMsg(t,cls){
  const el=$('fwMsg');
  el.textContent=t;
  el.className=cls||'muted';
}

async function loadUpdateStatus(){
  try{
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
  }catch(e){console.error('loadUpdateStatus',e)}
}

async function fwCheck(){
  logBtn('Check for Update');
  setFwMsg('Checking\u2026');
  try{
    const r=await fetch('/update/check',{method:'POST'});
    const j=await r.json().catch(()=>null);
    if(j&&!j.ok)setFwMsg(j.message||'Check failed','bad-text');
  }catch(e){setFwMsg('Check request failed','bad-text')}
  await loadUpdateStatus();
}

function setFwClearMsg(t,cls){
  const el=$('fwClearMsg');
  el.textContent=t;
  el.className=cls||'muted';
}

async function fwClearFirmware(){
  logBtn('Delete Staged Firmware Files');
  if(!confirm('Delete all files in /Firmware on the SD card?\nThis removes any downloaded-but-not-installed image and anything staged for mesh nodes.'))return;
  setFwClearMsg('Deleting\u2026');
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
  $('fwModalPct').textContent='Starting\u2026';
  $('fwModalHint').style.display='';
  $('fwModalCancelBtn').style.display='';
  $('fwModalCloseBtn').style.display='none';
  $('fwModalInstallBtn').style.display='none';
  $('fwModalOverlay').style.display='flex';
}

function fwModalHide(){
  if(fwModalIv){clearInterval(fwModalIv);fwModalIv=null;}
  $('fwModalOverlay').style.display='none';
}

function fwModalRender(j){
  $('fwModalLog').textContent=j.log||'';
  const log=$('fwModalLog');
  log.scrollTop=log.scrollHeight;

  const bar=$('fwModalBar');
  const haveSize=j.totalBytes>0;
  if(j.downloading&&!haveSize){
    // Connecting / following GitHub's redirect to the CDN -- no byte total
    // yet, so sweep an indeterminate bar instead of sitting stuck at 0%
    // width with no visible activity, and surface the stage text (e.g.
    // "Connecting (attempt 1/3)...", "Redirected to CDN -- settling...")
    // so the user can see work is actually happening in the background.
    bar.classList.remove('active');
    bar.classList.add('indeterminate');
    $('fwModalPct').textContent=j.downloadStage||'Connecting\u2026';
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

async function fwModalPoll(){
  const j=await loadUpdateStatus();
  if(j)fwModalRender(j);
}

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

async function fwDownloadModalCancel(){
  logBtn('Cancel Firmware Download');
  try{await fetch('/update/downloadCancel',{method:'POST'});}catch(e){}
  fwModalHide();
  await loadUpdateStatus();
}

async function fwDownloadModalClose(){
  fwModalHide();
  await loadUpdateStatus();
}

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
    // Expected once the device restarts mid-response on success.
    $('fwModalTitle').textContent='Rebooting...';
    $('fwModalLog').textContent+='Device is rebooting\u2026\n';
  }
  await loadUpdateStatus();
}

function setFwStageNodeMsg(t,cls){
  const el=$('fwStageNodeMsg');
  el.textContent=t;
  el.className=cls||'muted';
}

async function fwStageNode(){
  logBtn('Store Piglet Node Firmware');
  setFwStageNodeMsg('Downloading PigletNode.ota.bin\u2026');
  $('fwStageNodeBtn').disabled=true;
  try{
    const r=await fetch('/update/stageNode',{method:'POST'});
    const j=await r.json().catch(()=>null);
    if(j&&j.ok)setFwStageNodeMsg('Staged OK \u2014 ready to push to connected nodes','ok-text');
    else setFwStageNodeMsg((j&&j.error)||'Staging failed','bad-text');
  }catch(e){setFwStageNodeMsg('Request failed','bad-text')}
  $('fwStageNodeBtn').disabled=false;
  await loadMeshNodes();
}
// Slowed from 2000ms -- the firmware-update modal has its own dedicated
// 500ms poller (fwModalIv) while a download/install is actually active, so
// this background interval only needs to catch "an update appeared" /
// error-state changes when the modal is closed. A slower idle cadence
// leaves the narrow MALLOC_CAP_DMA pool (shared with GitHub's own TLS
// handshake) less contended at rest.
setInterval(loadUpdateStatus,5000);

/* ---- Mesh Nodes (Core-pushed OTA to connected nodes) ---- */
function otaPhaseText(p){
  switch(p){
    case 1: return 'Starting';
    case 2: return 'Sending';
    case 3: return 'Done';
    case 4: return 'Failed';
    default: return 'Idle';
  }
}

// Self-rescheduling (setTimeout, not setInterval) so the cadence can adapt:
// fast (1000ms) for a few seconds around an active node OTA push, slow
// (6000ms) otherwise. Mesh nodes/OTA progress isn't time-critical when
// nothing is pushing, and most boards never use this card at all -- a slow
// idle default leaves the narrow MALLOC_CAP_DMA pool (shared with GitHub's
// own Check-for-Update TLS handshake) less contended at rest.
let meshNodesTimer=null;
let meshNodesBusyUntilMs=0;
function scheduleMeshNodes(ms){
  if(meshNodesTimer)clearTimeout(meshNodesTimer);
  meshNodesTimer=setTimeout(loadMeshNodes,ms);
}
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
      if(isBusy)statusTxt+=' \u2014 '+otaPhaseText(ota.phase)+' '+pct+'%';
      else if(ota.nodeIdx===n.idx&&ota.msg)statusTxt+=' \u2014 '+ota.msg;
      const actionBtn=isBusy
        ?'<button class="btn-sm btn-danger" onclick="meshOtaCancel()">Cancel</button>'
        :'<button class="btn-sm" onclick="meshOtaStart('+n.idx+')"'+(busy?' disabled':'')+'>Update</button>';
      return '<div class="file-row"><span class="file-name">'+n.mac+'</span>'
        +'<span class="file-size">'+statusTxt+'</span>'
        +'<span class="file-actions">'+actionBtn+'</span></div>';
    }).join('');
  }catch(e){console.error('loadMeshNodes',e)}
  scheduleMeshNodes(nextMs);
}

async function meshOtaStart(idx){
  logBtn('Mesh Node Update (idx='+idx+')');
  try{
    const r=await fetch('/mesh/ota/start?idx='+idx,{method:'POST'});
    const j=await r.json().catch(()=>null);
    if(j&&!j.ok)alert(j.msg||'Could not start update');
  }catch(e){alert('Request failed')}
  await loadMeshNodes();
}

async function meshOtaCancel(){
  logBtn('Mesh OTA Cancel');
  try{await fetch('/mesh/ota/cancel',{method:'POST'});}catch(e){}
  await loadMeshNodes();
}
// loadMeshNodes() self-reschedules (see scheduleMeshNodes() above) -- no
// setInterval needed here.

/* ---- Manual per-node channel-range overrides ---- */
function setChOvrMsg(t,cls){
  const el=$('chOvrMsg');
  el.textContent=t;
  el.className=cls||'muted';
}

async function loadChOverrides(){
  try{
    const r=await fetch('/mesh/channelOverrides');const j=await r.json();
    const el=$('chOverrides');
    if(!j.overrides||j.overrides.length===0){el.innerHTML='<span class="muted">No overrides set</span>';return;}
    el.innerHTML=j.overrides.map(o=>
      '<div class="file-row"><span class="file-name">'+o.mac+'</span>'
      +'<span class="file-size">ch '+o.chStart+'-'+o.chEnd+'</span>'
      +'<span class="file-actions"><button class="btn-sm btn-danger" onclick="chOvrClear(\''+o.mac+'\')">Remove</button></span></div>'
    ).join('');
  }catch(e){console.error('loadChOverrides',e)}
}

async function chOvrSet(){
  logBtn('Set Channel Override');
  const mac=($('chOvrMac').value||'').trim();
  const chStart=$('chOvrStart').value;
  const chEnd=$('chOvrEnd').value;
  if(!mac||chStart===''||chEnd===''){setChOvrMsg('MAC, start channel, and end channel are all required','bad-text');return;}
  setChOvrMsg('Saving\u2026');
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
  try{
    await fetch('/mesh/channelOverrides/clear',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'mac='+encodeURIComponent(mac)});
  }catch(e){}
  await loadChOverrides();
}
// No recurring poll -- channel overrides only change via explicit admin
// action (chOvrSet()/chOvrClear() below already refresh this list right
// after mutating), so a background interval here was pure idle DMA-pool
// chatter with no benefit.

setInterval(pollUpload,1500);
loadStatus();loadUpdateStatus();loadMeshNodes();loadChOverrides();
</script>
</body>
</html>
)HTML";

// ---------------- Handlers ----------------

static void handlePing() {
  Serial.printf("[WEB] /ping  heap=%u\n", ESP.getFreeHeap());
  server.send(200, "text/plain", "pong");
}

// Fire-and-forget sink for Web UI button-press logging (see logBtn() in
// INDEX_HTML) -- makes field debugging able to see exactly which control
// was pressed and when, directly in the serial console.
static void handleUiLog() {
  String msg = server.hasArg("msg") ? server.arg("msg") : "(unknown)";
  Serial.printf("[UI] %s\n", msg.c_str());
  server.send(200, "text/plain", "OK");
}

static void handleRoot() {
  Serial.printf("[WEB] / requested  heap=%u largestBlock=%u largestBlock8bit=%u\n",
                (unsigned)ESP.getFreeHeap(),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  server.sendHeader("Cache-Control", "no-store");
  server.send_P(200, "text/html", INDEX_HTML);
  Serial.println("[WEB] / send_P done");
}

static void handleStatus() {
  // Heap allocation: StaticJsonDocument<N> puts N bytes on the Arduino loop
  // task stack (8192 bytes). Even 1024 bytes plus WebServer call chain overhead
  // risks a stack overflow.  DynamicJsonDocument allocates from the heap instead.
  // Bumped from 2048 -> 3072 to leave headroom for the 10 ssidWhitelistN
  // string fields (each up to a full 32-byte SSID) added to "config".
  JsonDocument doc(psramJsonAllocator());

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

  // AP timer state for the WebUI's keep-alive modal.
  doc["apActive"]   = apWindowActive;
  doc["apExtended"] = apExtended;
  if (apWindowActive) {
    uint32_t elapsed, budget;
    if (apExtended) {
      elapsed = millis() - apExtendedStartMs;
      budget  = AP_EXTENDED_WINDOW_MS;
    } else {
      elapsed = millis() - apStartMs;
      budget  = AP_WINDOW_MS;
    }
    doc["apRemainingMs"] = (elapsed >= budget) ? 0 : (budget - elapsed);
  } else {
    doc["apRemainingMs"] = 0;
  }
  doc["apExtendPromptLeadMs"] = AP_EXTEND_PROMPT_LEAD_MS;
  doc["uploading"] = uploading;
  doc["uploadTotalFiles"] = uploadTotalFiles;
  doc["uploadDoneFiles"] = uploadDoneFiles;
  doc["uploadCurrentFile"] = uploadCurrentFile;
  doc["uploadLastResult"] = uploadLastResult;
  doc["wigleTokenStatus"] = wigleTokenStatus;
  doc["wigleLastHttpCode"] = wigleLastHttpCode;

  JsonObject c = doc.createNestedObject("config");
  c["wigleBasicToken"] = cfg.wigleBasicToken;
  c["wdgwarsApiKey"]   = cfg.wdgwarsApiKey;
  c["wardriftApiKey"]  = cfg.wardriftApiKey;
  c["homeSsid"] = cfg.homeSsid;
  c["homePsk"] = cfg.homePsk.length() ? "(set)" : "";
  c["wardriverSsid"] = cfg.wardriverSsid;
  c["wardriverPsk"] = cfg.wardriverPsk;
  c["gpsBaud"] = cfg.gpsBaud;
  c["scanMode"] = cfg.scanMode;
  c["board"] = cfg.board;
  c["speedUnits"] = cfg.speedUnits;
  c["battPin"] = cfg.battPin;
  c["batteryTest"] = cfg.batteryTest;
  c["maxBootUploads"] = cfg.maxBootUploads;
  c["deviceName"]     = cfg.deviceName;
  c["meshModeOnBoot"] = cfg.meshModeOnBoot;
  c["rotateScreen180"] = cfg.rotateScreen180;
  c["autoStartAfterUpload"] = cfg.autoStartAfterUpload;
  c["uploadOnNetworkSeen"] = cfg.uploadOnNetworkSeen;
  c["wardriveGoBridge"] = cfg.wardriveGoBridge;
  c["autoUpdateHeadlessNodes"] = cfg.autoUpdateHeadlessNodes;
  for (uint8_t i = 0; i < 10; i++) {
    c[String("ssidWhitelist") + String(i + 1)] = cfg.ssidWhitelist[i];
  }

  String output;
  output.reserve(measureJson(doc) + 1);
  serializeJson(doc, output);
  server.send(200, "application/json", output);
}

// Minimum internal free heap to keep in reserve while walking a directory.
// Measurement showed each fopen()/fclose() cycle here (via SD.open()'s
// underlying VFS layer) permanently loses roughly 100-130 bytes of internal
// heap with no recovery -- unaffected by shrinking the file's stdio buffer
// via setBufferSize(), so the loss isn't that buffer; it's deeper in the
// Arduino core's newlib/VFS fopen() plumbing, outside this codebase's
// control. The Web UI now only ever requests one page (default 50 files) at
// a time via offset/limit, which keeps a normal page far under this floor;
// it remains as a safety net for a worst-case jump to a very large offset
// on a directory with many thousands of files.
static const uint32_t DIR_LISTING_HEAP_FLOOR = 24576;

// Walks /logs then /uploaded as one continuous, stably-ordered sequence,
// skipping the first `offset` entries and collecting up to `limit` of them
// into `arr`. Peeking one entry past the page sets `hasMore` without
// including it, so the Web UI's "Load More" button only appears when there
// really is another page -- this avoids walking (and opening) the entire
// remainder of a large directory just to report a total count. Returns true
// if the heap-floor circuit breaker had to stop the walk early.
static bool collectFilesPage(JsonArray arr, uint32_t offset, uint32_t limit, bool& hasMore) {
  hasMore = false;
  uint32_t index = 0;
  bool floorHit = false;
  const char* dirs[] = { "/logs", "/uploaded" };

  for (const char* dir : dirs) {
    if (floorHit) break;
    File root = SD.open(dir);
    if (!root) continue;
    bool isUploaded = (String(dir) == "/uploaded");

    File f = root.openNextFile();
    while (f) {
      if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL) < DIR_LISTING_HEAP_FLOOR) {
        f.close();
        floorHit = true;
        break;
      }

      if (index < offset) {
        // Still skipping toward the requested page -- nothing to build yet.
        f.close();
        index++;
        f = root.openNextFile();
        continue;
      }

      if ((uint32_t)arr.size() >= limit) {
        // Page is already full; this file's mere existence means there's
        // another page, so stop without doing any more work on it.
        f.close();
        hasMore = true;
        break;
      }

      // Every fopen() in the Arduino core's VFS layer unconditionally
      // allocates a 4096-byte stdio buffer via setvbuf() (DEFAULT_FILE_BUFFER_SIZE
      // in vfs_api.cpp), even though this loop never reads file content --
      // .size() below is served from the fstat() already cached in the File's
      // constructor. Shrinking it immediately after open (must be the very
      // next call per the library's own doc comment) reduces peak per-file
      // memory pressure/fragmentation.
      f.setBufferSize(64);

      String fullPath = normalizeSdPath(dir, f.name());
      if (fullPath.length() > 0) {
        JsonObject o = arr.createNestedObject();
        o["name"] = fullPath;
        o["size"] = (uint32_t)f.size();
        o["uploaded"] = isUploaded;
      }
      f.close();
      index++;
      f = root.openNextFile();
    }
    root.close();
  }

  return floorHit;
}

static void handleFiles() {
  // 4096-byte STATIC doc would use half the entire 8192-byte task stack.
  // Use dynamic (heap) allocation instead.
  JsonDocument doc(psramJsonAllocator());
  doc["ok"] = sdOk;

  JsonArray arr = doc.createNestedArray("files");
  bool hasMore   = false;
  bool truncated = false;
  if (sdOk) {
    uint32_t offset = server.hasArg("offset") ? (uint32_t)server.arg("offset").toInt() : 0;
    uint32_t limit  = server.hasArg("limit")  ? (uint32_t)server.arg("limit").toInt()  : 50;
    if (limit == 0 || limit > 200) limit = 50;  // sane bounds -- Web UI always asks for 50
    truncated = collectFilesPage(arr, offset, limit, hasMore);
  }
  doc["hasMore"]   = hasMore;
  doc["truncated"] = truncated;

  // Check if JSON buffer overflowed
  if (doc.overflowed()) {
    Serial.println("[WEB] WARNING: files.json buffer overflow!");
    server.send(507, "application/json", "{\"ok\":false,\"error\":\"Buffer overflow\"}");
    return;
  }

  String output;
  output.reserve(measureJson(doc) + 1);
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

static void handleDeleteAll() {
  if (!sdOk) { server.send(500, "application/json", "{\"ok\":false,\"error\":\"SD not available\"}"); return; }

  uint32_t deleted = 0;
  bool truncated = false;
  const char* dirs[] = { "/logs", "/uploaded" };
  for (const char* dir : dirs) {
    File root = SD.open(dir);
    if (!root) continue;
    std::vector<String> paths;
    File f = root.openNextFile();
    while (f) {
      f.setBufferSize(64);  // avoid the 4KB default stdio buffer -- see addDirFiles()
      String p = normalizeSdPath(dir, f.name());
      f.close();
      // Skip dot-prefixed housekeeping entries (the .gzscratch gzip scratch
      // directory, the .upload_state.cfg per-file upload ledger) -- "Delete
      // All" should only ever touch user wardriving data. Wiping the ledger
      // here used to be harmless only because every CSV it referenced was
      // also being deleted in the same pass; the heap-floor circuit breaker
      // below can stop that pass early, which left any not-yet-deleted CSVs
      // looking "never uploaded" on the next upload round.
      if (p.length() > 0 && !isHiddenSdEntry(pathBasename(p))) paths.push_back(p);

      // Same heap-floor circuit breaker as addDirFiles() -- a directory with
      // enough accumulated files can drain internal heap via this same
      // listing loop before the delete pass below even starts. Stopping
      // early here still deletes everything collected so far; the operator
      // can click "Delete All" again afterward (heap fully recovers on
      // reboot, or partially recovers here once fewer files remain) to
      // finish the rest.
      if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL) < DIR_LISTING_HEAP_FLOOR) {
        Serial.printf("[WEB] handleDeleteAll(%s): stopping listing early (heap floor reached)\n", dir);
        truncated = true;
        break;
      }

      f = root.openNextFile();
    }
    root.close();
    for (const String& p : paths) {
      if (currentCsvPath.length() > 0 && p == currentCsvPath) continue;
      if (SD.remove(p)) deleted++;
    }
  }

  JsonDocument doc(psramJsonAllocator());
  doc["ok"] = true;
  doc["deleted"] = deleted;
  doc["truncated"] = truncated;
  String out; out.reserve(measureJson(doc) + 1); serializeJson(doc, out);
  server.send(200, "application/json", out);
}

static void handleStart() {
  scanningEnabled = true;
  userScanOverride = true;
  // If the AP is still up, request an immediate teardown so scanning can begin.
  if (apWindowActive) {
    Serial.println("[WEB] /start -> force-close AP");
    apForceClose = true;
  }
  server.send(200, "text/plain", "OK");
}

static void handleExtend() {
  if (!apWindowActive) {
    server.send(409, "text/plain", "AP not active");
    return;
  }
  apExtended = true;
  apExtendedStartMs = millis();
  Serial.println("[WEB] /extend -> AP extended window reset");
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
    JsonDocument doc(psramJsonAllocator());
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

  cfgEnforceMutualExclusion();

  Serial.println("[CFG] Updated config from Web UI (in-RAM). Saving to SD...");
  bool ok = saveConfigToSD();
  server.send(ok ? 200 : 500, "text/plain", ok ? "OK" : "FAIL");
}

static void handleCleanup() {
  if (!sdOk) { server.send(500, "text/plain", "SD not available"); return; }
  deleteEmptyCsvs();
  server.send(200, "text/plain", "OK");
}

static void handleReboot() {
  closeLogFile();                      // flush & close active CSV log cleanly
  server.send(200, "text/plain", "OK");
  server.client().stop();
  delay(200);
  ESP.restart();                       // never returns
}

static void handleWigleTest() {
  bool ok = wigleTestToken();

  JsonDocument doc(psramJsonAllocator());
  doc["ok"] = ok;
  doc["tokenStatus"] = wigleTokenStatus;
  doc["httpCode"] = wigleLastHttpCode;
  doc["message"] = uploadLastResult;

  String out;
  out.reserve(measureJson(doc) + 1);
  serializeJson(doc, out);
  server.send(ok ? 200 : 400, "application/json", out);
}

static void handleWdgwarsTest() {
  if (WiFi.status() != WL_CONNECTED) {
    server.send(400, "application/json",
                "{\"ok\":false,\"message\":\"STA WiFi not connected\"}");
    return;
  }
  if (cfg.wdgwarsApiKey.length() < 8) {
    server.send(400, "application/json",
                "{\"ok\":false,\"message\":\"No API key configured\"}");
    return;
  }

  bool ok = wdgwarsTestKey();

  JsonDocument doc(psramJsonAllocator());
  doc["ok"]      = ok;
  doc["message"] = uploadLastResult;

  String out;
  out.reserve(measureJson(doc) + 1);
  serializeJson(doc, out);
  server.send(ok ? 200 : 400, "application/json", out);
}

static void handleWdgwarsUploadAll() {
  if (!sdOk) { server.send(500, "text/plain", "SD not available"); return; }
  if (WiFi.status() != WL_CONNECTED) { server.send(400, "text/plain", "STA WiFi not connected"); return; }
  if (cfg.wdgwarsApiKey.length() < 8) { server.send(400, "text/plain", "No API key configured"); return; }

  // Pass -1 explicitly: web-triggered uploads bypass the maxBootUploads cap.
  uint32_t okCount = uploadAllCsvsToWdgwars(-1);

  JsonDocument doc(psramJsonAllocator());
  doc["ok"]       = (okCount > 0);
  doc["uploaded"] = okCount;
  doc["total"]    = uploadTotalFiles;
  doc["message"]  = uploadLastResult;

  String out;
  out.reserve(measureJson(doc) + 1);
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

static void handleWardriftTest() {
  if (WiFi.status() != WL_CONNECTED) {
    server.send(400, "application/json",
                "{\"ok\":false,\"message\":\"STA WiFi not connected\"}");
    return;
  }
  if (cfg.wardriftApiKey.length() < 8) {
    server.send(400, "application/json",
                "{\"ok\":false,\"message\":\"No API key configured\"}");
    return;
  }

  bool ok = wardriftTestKey();

  JsonDocument doc(psramJsonAllocator());
  doc["ok"]      = ok;
  doc["message"] = uploadLastResult;

  String out;
  out.reserve(measureJson(doc) + 1);
  serializeJson(doc, out);
  server.send(ok ? 200 : 400, "application/json", out);
}

static void handleWardriftUploadAll() {
  if (!sdOk) { server.send(500, "text/plain", "SD not available"); return; }
  if (WiFi.status() != WL_CONNECTED) { server.send(400, "text/plain", "STA WiFi not connected"); return; }
  if (cfg.wardriftApiKey.length() < 8) { server.send(400, "text/plain", "No API key configured"); return; }

  uint32_t okCount = uploadAllCsvsToWardrift(-1);

  JsonDocument doc(psramJsonAllocator());
  doc["ok"]       = (okCount > 0);
  doc["uploaded"] = okCount;
  doc["total"]    = uploadTotalFiles;
  doc["message"]  = uploadLastResult;

  String out;
  out.reserve(measureJson(doc) + 1);
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

static void handleWigleUploadAll() {
  if (!sdOk) { server.send(500, "text/plain", "SD not available"); return; }
  if (WiFi.status() != WL_CONNECTED) { server.send(400, "text/plain", "STA WiFi not connected"); return; }

  // Pass -1 explicitly: web-triggered uploads are always unlimited,
  // bypassing the maxBootUploads cap which applies only at boot.
  uint32_t okCount = uploadAllPending(-1);

  // History will auto-refresh when user next accesses /files.json
  
  JsonDocument doc(psramJsonAllocator());
  doc["ok"] = (okCount > 0);
  doc["uploaded"] = okCount;
  doc["total"] = uploadTotalFiles;
  doc["message"] = uploadLastResult;

  String out;
  out.reserve(measureJson(doc) + 1);
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

// Scratch path for a single on-demand per-file gzip upload below -- mirrors
// the combined orchestrator's gzScratchPathFor() in WigleUpload.cpp, kept
// local here since this is the only place in WebUI.cpp that needs it.
static String gzScratchPathForOne(const String& basename) {
  static const char* dir = "/logs/.gzscratch";
  if (!SD.exists(dir)) SD.mkdir(dir);
  return String(dir) + "/" + basename + ".gz";
}

static void handleWigleUploadOne() {
  if (!sdOk) { server.send(500, "text/plain", "SD not available"); return; }
  if (WiFi.status() != WL_CONNECTED) { server.send(400, "text/plain", "STA WiFi not connected"); return; }
  if (!server.hasArg("name")) { server.send(400, "text/plain", "Missing name"); return; }

  String path = server.arg("name");
  if (!SD.exists(path)) { server.send(404, "text/plain", "Not found"); return; }

  bool hasWigle    = cfg.wigleBasicToken.length() >= 8;
  bool hasWdg      = cfg.wdgwarsApiKey.length()   >= 8;
  bool hasWardrift = cfg.wardriftApiKey.length()  >= 8;
  if (!hasWigle && !hasWdg && !hasWardrift) {
    server.send(400, "text/plain", "No WiGLE token, WDGoWars API key, or Wardrift token configured");
    return;
  }

  String basename = pathBasename(path);
  FileUploadState st = loadUploadState(basename);

  uploading = true;
  uploadPausedScanWasEnabled = scanningEnabled;
  scanningEnabled = false;

  uploadTotalFiles = 1;
  uploadDoneFiles = 0;
  uploadCurrentFile = path;
  updateOLED(0);

  // Mirrors the combined orchestrator's per-file gzip path (WigleUpload.cpp's
  // uploadAllPending()): try whichever services are still pending for this
  // file -- WDGoWars, then Wardrift, then WiGLE -- skipping any already
  // marked done, and only move the file to /uploaded/ once every configured
  // service is done.
  wigleLastHttpCode = 0;  // avoid reporting a stale code if WiGLE isn't attempted below
  String wdgResult = "Already done", wardriftResult = "Already done", wigleResult = "Already done";

  if (hasWdg && !st.wdgwDone) {
    uploadTargetName = "WDGW UL";
    updateOLED(0);
    bool ok;
    if (cfg.gzipUploads) {
      String scratch = gzScratchPathForOne(basename);
      ok = gzipCompressFileToFile(path, scratch) && uploadFileToWdgwars(scratch);
      SD.remove(scratch);
    } else {
      ok = uploadFileToWdgwars(path);
    }
    if (ok) { markServiceDone(basename, UploadService::Wdgw); st.wdgwDone = true; }
    wdgResult = uploadLastResult;
    if (hasWardrift || hasWigle) delay(1500);  // brief settle between TLS sessions
  }

  if (hasWardrift && !st.wardriftDone) {
    uploadTargetName = "Wardrift UL";
    updateOLED(0);
    // Always sent uncompressed -- Wardrift's /v1/ingest/signals endpoint
    // does not honor Content-Encoding: gzip (confirmed in the field: a
    // gzip-compressed body comes back HTTP 400 "file did not contain
    // supported signal rows", even though the same file succeeds against
    // WDGoWars, which does support gzip). Ignore cfg.gzipUploads here.
    bool ok = uploadFileToWardrift(path);
    if (ok) { markServiceDone(basename, UploadService::Wardrift); st.wardriftDone = true; }
    wardriftResult = uploadLastResult;
    if (hasWigle) delay(1500);  // brief settle between TLS sessions
  }

  if (hasWigle && !st.wigleDone) {
    uploadTargetName = "WiGLE UL";
    updateOLED(0);
    bool ok;
    if (cfg.gzipUploads) {
      String scratch = gzScratchPathForOne(basename);
      ok = gzipCompressFileToFile(path, scratch) && uploadFileToWigle(scratch);
      SD.remove(scratch);
    } else {
      ok = uploadFileToWigle(path);
    }
    if (ok) { markServiceDone(basename, UploadService::Wigle); st.wigleDone = true; }
    wigleResult = uploadLastResult;
  }

  uploadDoneFiles = 1;
  updateOLED(0);

  uploading = false;
  scanningEnabled = uploadPausedScanWasEnabled;
  uploadCurrentFile = "";
  uploadTargetName = "";
  updateOLED(0);

  bool ok = allConfiguredServicesDone(st);
  if (ok) {
    // See the matching comment in WigleUpload.cpp's uploadAllPending()
    // finalize loop -- only clear the ledger once the move actually
    // succeeds, so a transient SD hiccup here can't make an already-fully-
    // uploaded file look unsent again on the next run.
    if (moveToUploaded(path)) {
      clearUploadState(basename);
      // History will auto-refresh when user next accesses /files.json
    } else {
      Serial.printf("[UPLOAD] WARNING: moveToUploaded failed for %s -- file stays in /logs, ledger kept\n", basename.c_str());
    }
  }

  // Combine all configured services' results into the message
  int attempted = (hasWdg ? 1 : 0) + (hasWardrift ? 1 : 0) + (hasWigle ? 1 : 0);
  String message;
  if (attempted > 1) {
    std::vector<String> parts;
    if (hasWdg)      parts.push_back("WDGW: " + wdgResult);
    if (hasWardrift) parts.push_back("Wardrift: " + wardriftResult);
    if (hasWigle)    parts.push_back("WiGLE: " + wigleResult);
    for (size_t i = 0; i < parts.size(); i++) {
      if (i) message += " | ";
      message += parts[i];
    }
  } else if (hasWdg) {
    message = wdgResult;
  } else if (hasWardrift) {
    message = wardriftResult;
  } else {
    message = wigleResult;
  }

  JsonDocument doc(psramJsonAllocator());
  doc["ok"] = ok;
  doc["httpCode"] = wigleLastHttpCode;
  doc["message"] = message;

  String out;
  out.reserve(measureJson(doc) + 1);
  serializeJson(doc, out);
  server.send(ok ? 200 : 500, "application/json", out);
}

// ---- Mesh Nodes (Core-pushed OTA to connected nodes) ----

static const char* meshBoardTypeLabel(uint8_t bt) {
  switch (bt) {
    case PIGLET_BOARD_NODE:    return "PigletNode";
    case PIGLET_BOARD_XIAO:    return "Piglet";
    case PIGLET_BOARD_TDONGLE: return "TDongleC5_Piglet";
    default:                   return "Unknown";
  }
}

static void handleMeshNodes() {
  JsonDocument doc(psramJsonAllocator());
  doc["coreActive"] = meshCoreActive;

  JsonArray arr = doc.createNestedArray("nodes");
  if (meshCoreActive) {
    uint32_t now = millis();
    for (uint8_t i = 0; i < CORE_MAX_NODES; i++) {
      if (!coreNodes[i].active) continue;
      JsonObject o = arr.createNestedObject();
      char macStr[18];
      snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
               coreNodes[i].mac[0], coreNodes[i].mac[1], coreNodes[i].mac[2],
               coreNodes[i].mac[3], coreNodes[i].mac[4], coreNodes[i].mac[5]);
      o["idx"]           = i;
      o["mac"]           = macStr;
      o["boardType"]     = coreNodes[i].boardType;
      o["board"]         = meshBoardTypeLabel(coreNodes[i].boardType);
      o["isPiglet"]      = coreNodes[i].isPiglet;
      o["isBiscuit"]     = coreNodes[i].isBiscuit;
      o["recordsRx"]     = coreNodes[i].recordsRx;
      o["lastSeenMsAgo"] = now - coreNodes[i].lastHbMs;
    }
  }

  CoreOtaStatus st = coreOtaStatus();
  JsonObject ota = doc.createNestedObject("ota");
  ota["phase"]      = (int)st.phase;
  ota["nodeIdx"]    = st.nodeIdx;
  ota["sentBytes"]  = st.sentBytes;
  ota["totalBytes"] = st.totalBytes;
  ota["msg"]        = st.msg;
  ota["loadMsg"]    = st.loadMsg;

  String out;
  out.reserve(measureJson(doc) + 1);
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

static void handleMeshOtaStart() {
  if (!meshCoreActive) { server.send(400, "text/plain", "Not in Core mode"); return; }
  if (!server.hasArg("idx")) { server.send(400, "text/plain", "Missing idx"); return; }

  uint8_t idx = (uint8_t)server.arg("idx").toInt();
  bool ok = coreOtaStart(idx);
  CoreOtaStatus st = coreOtaStatus();

  JsonDocument doc(psramJsonAllocator());
  doc["ok"]  = ok;
  doc["msg"] = st.msg;

  String out;
  out.reserve(measureJson(doc) + 1);
  serializeJson(doc, out);
  server.send(ok ? 200 : 400, "application/json", out);
}

static void handleMeshOtaCancel() {
  coreOtaAbort();
  server.send(200, "text/plain", "OK");
}

// ---- Manual per-node channel-range overrides (Web UI) ----

static bool webMacParse(const String& s, uint8_t* macOut) {
  unsigned int v[6];
  if (sscanf(s.c_str(), "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) return false;
  for (int i = 0; i < 6; i++) macOut[i] = (uint8_t)v[i];
  return true;
}

static void handleMeshChannelOverridesGet() {
  JsonDocument doc(psramJsonAllocator());
  JsonArray arr = doc.createNestedArray("overrides");
  uint8_t count = meshChOverrideGetCount();
  for (uint8_t i = 0; i < count; i++) {
    uint8_t mac[6]; uint16_t chStart, chEnd;
    if (!meshChOverrideGetAt(i, mac, chStart, chEnd)) continue;
    JsonObject o = arr.createNestedObject();
    char macStr[18];
    snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    o["mac"]     = macStr;
    o["chStart"] = chStart;
    o["chEnd"]   = chEnd;
  }

  String out;
  out.reserve(measureJson(doc) + 1);
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

static void handleMeshChannelOverridesSet() {
  if (!server.hasArg("mac") || !server.hasArg("chStart") || !server.hasArg("chEnd")) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"Missing mac/chStart/chEnd\"}");
    return;
  }
  uint8_t mac[6];
  if (!webMacParse(server.arg("mac"), mac)) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"Bad MAC format\"}");
    return;
  }
  int chStart = server.arg("chStart").toInt();
  int chEnd   = server.arg("chEnd").toInt();
  bool ok = meshChOverrideSet(mac, chStart, chEnd);

  JsonDocument doc(psramJsonAllocator());
  doc["ok"] = ok;
  if (!ok) doc["error"] = "Invalid channel -- must match a supported 2.4/5GHz channel number";
  String out;
  out.reserve(measureJson(doc) + 1);
  serializeJson(doc, out);
  server.send(ok ? 200 : 400, "application/json", out);
}

static void handleMeshChannelOverridesClear() {
  if (!server.hasArg("mac")) { server.send(400, "text/plain", "Missing mac"); return; }
  uint8_t mac[6];
  if (!webMacParse(server.arg("mac"), mac)) { server.send(400, "text/plain", "Bad MAC format"); return; }
  bool ok = meshChOverrideClear(mac);
  server.send(ok ? 200 : 404, "text/plain", ok ? "OK" : "Not found");
}

// ---- Firmware Update (self-update from GitHub Releases) ----

static void handleUpdateStatus() {
  // fwLog can hold up to FW_LOG_MAX (2048) bytes of text -- size the
  // document well above that plus the other fields and JSON overhead.
  JsonDocument doc(psramJsonAllocator());
  doc["checked"]         = fwUpdateChecked;
  doc["available"]       = fwUpdateAvailable;
  doc["currentVersion"]  = FIRMWARE_VERSION;
  doc["availableVersion"] = fwAvailableVersion;
  doc["notes"]           = fwReleaseNotes;
  doc["checkError"]      = fwCheckError;

  doc["downloading"]     = fwDownloading;
  doc["downloaded"]      = fwDownloaded;
  doc["downloadStage"]   = fwDownloadStage;
  doc["downloadedBytes"] = fwDownloadedBytes;
  doc["totalBytes"]      = fwDownloadTotalBytes;
  doc["downloadError"]   = fwDownloadError;

  doc["installing"]      = fwInstalling;
  doc["installError"]    = fwInstallError;

  doc["log"]             = fwLog;

  doc["nodeAssetKnown"]  = fwHasNodeAsset();
  doc["stagingNode"]     = fwStagingNode;
  doc["stageNodeError"]  = fwStageNodeError;

  String out;
  out.reserve(measureJson(doc) + 1);
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

static void handleUpdateCheck() {
  if (WiFi.status() != WL_CONNECTED) {
    server.send(400, "application/json",
                "{\"ok\":false,\"message\":\"STA WiFi not connected\"}");
    return;
  }

  fwCheckForUpdate();
  Serial.printf("[FWUPD] Post-check heap: free=%u largestBlock=%u\n",
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));

  bool ok = (fwCheckError.length() == 0);
  JsonDocument doc(psramJsonAllocator());
  doc["ok"]               = ok;
  doc["message"]          = fwCheckError;
  doc["available"]        = fwUpdateAvailable;
  doc["availableVersion"] = fwAvailableVersion;

  String out;
  out.reserve(measureJson(doc) + 1);
  serializeJson(doc, out);
  server.send(ok ? 200 : 502, "application/json", out);
}

static void handleUpdateDownloadStart() {
  fwDownloadStart();
  // fwDownloadStart() is synchronous but non-blocking -- it only validates
  // preconditions and arms the state machine fwDownloadTick() drives from
  // loop(). fwDownloading is true here if a transfer is now (still) in
  // progress; otherwise fwDownloadError holds the rejection reason.
  bool ok = fwDownloading;

  JsonDocument doc(psramJsonAllocator());
  doc["ok"]    = ok;
  doc["error"] = fwDownloadError;

  String out;
  out.reserve(measureJson(doc) + 1);
  serializeJson(doc, out);
  server.send(ok ? 200 : 400, "application/json", out);
}

static void handleUpdateDownloadCancel() {
  fwDownloadCancel();
  server.send(200, "text/plain", "OK");
}

// Deletes every file currently staged in /Firmware/ (this board's own
// downloaded-but-not-yet-installed image, plus anything staged for the mesh
// Core-push engine) -- lets the operator clear a stuck/partial download or
// just reclaim SD space without pulling the card.
static void handleUpdateClearFirmware() {
  bool ok = fwClearFirmwareFolder();

  JsonDocument doc(psramJsonAllocator());
  doc["ok"] = ok;
  if (!ok) {
    doc["error"] = (fwDownloading || fwInstalling)
      ? "Cannot clear while a download or install is in progress"
      : "SD not available";
  }

  String out;
  out.reserve(measureJson(doc) + 1);
  serializeJson(doc, out);
  server.send(ok ? 200 : 400, "application/json", out);
}

static void handleUpdateInstallFromSD() {
  // Blocking, but purely local (SD -> flash, no network) so it's quick. On
  // success this calls ESP.restart() internally and never returns -- a
  // response is only sent here if it failed before flashing began.
  bool ok = fwInstallFromSD();

  JsonDocument doc(psramJsonAllocator());
  doc["ok"]    = ok;
  doc["error"] = fwInstallError;

  String out;
  out.reserve(measureJson(doc) + 1);
  serializeJson(doc, out);
  server.send(ok ? 200 : 500, "application/json", out);
}

// "Store Piglet Node Firmware": stages /Firmware/PigletNode.ota.bin from the
// latest GitHub release so headless PigletNode nodes can be pushed updates
// over ESP-NOW, independent of whether this Core's own firmware is current.
static void handleUpdateStageNode() {
  if (WiFi.status() != WL_CONNECTED) { server.send(400, "text/plain", "STA WiFi not connected"); return; }

  bool ok = fwStageNodeFirmware();
  if (ok) coreOtaPreload();  // reload PSRAM images so the new file is usable without a reboot

  JsonDocument doc(psramJsonAllocator());
  doc["ok"]    = ok;
  doc["error"] = fwStageNodeError;

  String out;
  out.reserve(measureJson(doc) + 1);
  serializeJson(doc, out);
  server.send(ok ? 200 : 500, "application/json", out);
}

// Streams all CSVs from /logs and /uploaded merged into one RFC 1952 gzip file,
// using DEFLATE stored blocks (BTYPE=00 — no compression library required).
// Non-first files have their 2-line WiGLE header stripped so the merged result
// is one valid WiGLE CSV uploadable directly to WiGLE.net.
static void handleDownloadAll() {
  if (!sdOk) { server.send(500, "text/plain", "SD not available"); return; }

  std::vector<String> paths;
  const char* dirs[] = { "/logs", "/uploaded" };
  for (const char* d : dirs) {
    File root = SD.open(d); if (!root) continue;
    File f = root.openNextFile();
    while (f) {
      // Skip subdirectories and non-CSV files
      if (!f.isDirectory()) {
        String p = normalizeSdPath(d, f.name());
        if (p.endsWith(".csv") && !(currentCsvPath.length() && p == currentCsvPath))
          paths.push_back(p);
      }
      f.close(); f = root.openNextFile();
    }
    root.close();
  }

  Serial.printf("[DOWNLOAD] Collected %u CSV file(s) for gzip\n", (unsigned)paths.size());
  if (paths.empty()) { server.send(200, "text/plain", "No CSV files to download"); return; }

  // Chunked transfer — output size not known in advance
  server.sendHeader("Content-Disposition", "attachment; filename=\"piglet_logs.csv.gz\"");
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "application/gzip", "");
  WiFiClient cl = server.client();

  // Write one HTTP chunk (hex-size CRLF data CRLF)
  auto writeChunk = [&](const uint8_t* d, size_t n) {
    if (!n) return;
    char hdr[10]; snprintf(hdr, sizeof(hdr), "%X\r\n", (unsigned)n);
    cl.write((const uint8_t*)hdr, strlen(hdr));
    cl.write(d, n);
    cl.write((const uint8_t*)"\r\n", 2);
  };

  // CRC32 (ISO 3309 / gzip polynomial 0xEDB88320) -- shared implementation
  // in GzipDeflate.cpp, also used by the real-compression upload path.
  auto crc32Up = gzipCrc32Update;

  // RFC 1952 gzip header (10 bytes)
  const uint8_t gzHdr[10] = {
    0x1f, 0x8b, 0x08, 0x00,   // ID1 ID2 CM FLG
    0x00, 0x00, 0x00, 0x00,   // MTIME = 0
    0x00, 0xff                 // XFL OS=unknown
  };
  writeChunk(gzHdr, 10);

  uint32_t crc   = 0;
  uint32_t isize = 0;
  uint8_t  buf[512];

  bool first = true;
  for (const String& path : paths) {
    File f = SD.open(path, FILE_READ); if (!f) continue;
    if (!first) {
      // Strip 2-line WiGLE header from non-first files
      if (f.available()) f.readStringUntil('\n');
      if (f.available()) f.readStringUntil('\n');
    }
    first = false;
    for (;;) {
      int n = f.read(buf, sizeof(buf)); if (n <= 0) break;
      // DEFLATE stored block (BFINAL=0, BTYPE=00)
      uint16_t ln = (uint16_t)n;
      uint8_t  blk[5] = {
        0x00,                    // BFINAL=0, BTYPE=00
        (uint8_t)(ln & 0xFF),   (uint8_t)(ln >> 8),   // LEN
        (uint8_t)((~ln) & 0xFF),(uint8_t)((~ln) >> 8) // NLEN
      };
      writeChunk(blk, 5);
      writeChunk(buf, (size_t)n);
      crc    = crc32Up(crc, buf, (size_t)n);
      isize += (uint32_t)n;
      yield();
    }
    f.close();
  }

  // Final empty stored block closes the DEFLATE stream (BFINAL=1, LEN=0)
  const uint8_t finalBlk[5] = { 0x01, 0x00, 0x00, 0xFF, 0xFF };
  writeChunk(finalBlk, 5);

  // Gzip trailer: CRC32 + ISIZE (both little-endian 32-bit)
  uint8_t trailer[8] = {
    (uint8_t)crc,        (uint8_t)(crc >> 8),
    (uint8_t)(crc >> 16),(uint8_t)(crc >> 24),
    (uint8_t)isize,      (uint8_t)(isize >> 8),
    (uint8_t)(isize >> 16),(uint8_t)(isize >> 24)
  };
  writeChunk(trailer, 8);

  cl.write((const uint8_t*)"0\r\n\r\n", 5);
}

// ---------------- Server Init ----------------

void startWebServer() {
  Serial.println("[WEB] Starting web server routes...");

  server.on("/", handleRoot);
  server.on("/status.json", handleStatus);
  server.on("/files.json", handleFiles);
  server.on("/download",    handleDownload);
  server.on("/downloadAll", handleDownloadAll);
  server.on("/delete", HTTP_POST, handleDelete);
  server.on("/start",  HTTP_POST, handleStart);
  server.on("/stop",   HTTP_POST, handleStop);
  server.on("/extend", HTTP_POST, handleExtend);
  server.on("/saveConfig", HTTP_POST, handleSaveConfig);

  server.on("/ping",            handlePing);
  server.on("/uiLog",           HTTP_POST, handleUiLog);
  server.on("/reboot",          HTTP_POST, handleReboot);
  server.on("/cleanup",         HTTP_POST, handleCleanup);
  server.on("/deleteAll",       HTTP_POST, handleDeleteAll);
  server.on("/wigle/test",      HTTP_POST, handleWigleTest);
  server.on("/wigle/uploadAll", HTTP_POST, handleWigleUploadAll);
  server.on("/wigle/upload",    HTTP_POST, handleWigleUploadOne);
  server.on("/wdgwars/test",      HTTP_POST, handleWdgwarsTest);
  server.on("/wdgwars/uploadAll", HTTP_POST, handleWdgwarsUploadAll);
  server.on("/wardrift/test",      HTTP_POST, handleWardriftTest);
  server.on("/wardrift/uploadAll", HTTP_POST, handleWardriftUploadAll);

  server.on("/mesh/nodes.json",  handleMeshNodes);
  server.on("/mesh/ota/start",   HTTP_POST, handleMeshOtaStart);
  server.on("/mesh/ota/cancel",  HTTP_POST, handleMeshOtaCancel);

  server.on("/mesh/channelOverrides",       HTTP_GET,  handleMeshChannelOverridesGet);
  server.on("/mesh/channelOverrides",       HTTP_POST, handleMeshChannelOverridesSet);
  server.on("/mesh/channelOverrides/clear", HTTP_POST, handleMeshChannelOverridesClear);

  server.on("/update/status",         handleUpdateStatus);
  server.on("/update/check",          HTTP_POST, handleUpdateCheck);
  server.on("/update/downloadStart",  HTTP_POST, handleUpdateDownloadStart);
  server.on("/update/downloadCancel", HTTP_POST, handleUpdateDownloadCancel);
  server.on("/update/clearFirmware",  HTTP_POST, handleUpdateClearFirmware);
  server.on("/update/installFromSD",  HTTP_POST, handleUpdateInstallFromSD);
  server.on("/update/stageNode",      HTTP_POST, handleUpdateStageNode);

  server.begin();
  Serial.println("[WEB] Server started");
}
