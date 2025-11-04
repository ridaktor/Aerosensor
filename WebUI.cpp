// WebUI.cpp — ESP32 AeroSensor web UI (HTML+JS from PROGMEM)
// - Removed "Clear logs"
// - "Delete selected" is red (warn)
// - After Save, confirm log period + invert state

#include "WebUI.h"
#include "Shared.h"
#include "Logging.h"
#include "Config.h"

#include <WebServer.h>
#include <Arduino.h>
#include <FS.h>
#include <SPIFFS.h>
#include <math.h>
#include <ESP.h>
#include <stdlib.h>

#include "SensorMS5525.h"   // doZero(...)
#include "EnvSensor.h"      // env* globals

static void (*saveSettingsFn)() = nullptr;

// Provided in your .ino (for speed gating UI)
extern bool g_showSpeed;

// ------------------- UI: HTML + JS -------------------
static const char INDEX_HTML[] PROGMEM = R"HTML(<!doctype html><html><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>AeroSensor</title>
<link rel="preconnect" href="https://cdn.jsdelivr.net">
<style>
:root{--bg:#0f172a;--fg:#e5e7eb;--accent:#22c55e;--muted:#94a3b8}
*{box-sizing:border-box} body{margin:12px;background:var(--bg);color:var(--fg);font:16px/1.4 system-ui,Segoe UI,Roboto}
.card{max-width:980px;margin:0 auto 12px;background:#111827;border-radius:16px;padding:14px}
.row{display:flex;gap:8px;flex-wrap:wrap;align-items:center}
button{background:var(--accent);color:#062;border:0;padding:8px 12px;border-radius:10px;cursor:pointer;font-weight:600}
button.secondary{background:#2563eb;color:#fff} button.warn{background:#ef4444;color:#fff}
label{margin-right:12px;color:var(--muted)} small{color:var(--muted)}
#chart{position:relative;height:360px} #chart2{position:relative;height:220px;margin-top:12px}
input,select{background:#0b1020;color:#fff;border:1px solid #223;padding:6px 8px;border-radius:8px}
.badge{display:inline-block;padding:4px 8px;border-radius:999px;background:#0b1020;color:#9ca3af;border:1px solid #223}
</style>
<script src="https://cdn.jsdelivr.net/npm/chart.js@4.4.1/dist/chart.umd.min.js"></script>
<script src="https://cdn.jsdelivr.net/npm/chartjs-adapter-date-fns@3"></script>
</head><body>
<div class="card">
  <div class="row">
    <div class="badge">AeroSensor</div>
    <small id="state">—</small>
    <small id="env" style="margin-left:8px">ρ: —, P: —, T: —, RH: —</small>
  </div>
  <div class="row" style="margin-top:6px">
    <button id="btnZero">Zero</button>
    <button id="btnSelf" class="secondary">Self-test</button>
    <button id="btnLog"  class="secondary">Start logging</button>
    <button id="btnDl"   class="secondary" disabled>Download current CSV</button>
    <label><input id="smooth" type="checkbox"> Smooth (10-pt)</label>
  </div>
  <div id="chart"><canvas id="c1"></canvas></div>
  <div id="chart2"><canvas id="c2"></canvas></div>
</div>

<div class="card">
  <h2 style="margin:0 0 8px">Settings</h2>
  <div class="row">
    <label><input id="invert" type="checkbox"> Invert ΔP sign</label>
    <label>Log every <input id="logms" type="number" min="10" step="10" style="width:80px"> ms</label>
    <button id="btnSave" class="secondary">Save</button>
    <button id="btnDelOne" class="warn">Delete selected</button>
    <button id="btnFormat" class="warn">Format FS</button>
  </div>
</div>

<div class="card">
  <h2 style="margin:0 0 8px">Logs</h2>
  <div class="row">
    <select id="files" style="min-width:360px"></select>
    <button id="btnGet" class="secondary">Download</button>
  </div>
</div>

<script src="/app.js"></script>
</body></html>)HTML";

// JS from PROGMEM
// in WebUI.cpp, replace APP_JS string with this:
static const char APP_JS[] PROGMEM = R"JS(
const fmt = v=> (Math.round(v*100)/100).toFixed(2);
let logging=false, curFile="";
const live = {t:[], dp:[], va:[], tc:[]};

// Charts
const baseOpts = {animation:false, maintainAspectRatio:false,
  plugins:{legend:{display:true}}, scales:{x:{type:'time'}, y:{beginAtZero:false}}};
const ch1 = new Chart(document.getElementById('c1'), {type:'line',
  data:{labels:live.t, datasets:[
    {label:'ΔP (Pa)', data:live.dp, pointRadius:0},
    {label:'V (m/s)', data:live.va, pointRadius:0, yAxisID:'y1'}
  ]},
  options:{...baseOpts, scales:{x:{type:'time'}, y:{position:'left'}, y1:{position:'right'}}}});
const ch2 = new Chart(document.getElementById('c2'), {type:'line',
  data:{labels:live.t, datasets:[{label:'Temp (°C)', data:live.tc, pointRadius:0}]}, options:baseOpts});

function setState(s){ document.getElementById('state').textContent = s; }

async function syncTime(){
  try{
    const epoch_ms = Date.now();                 // browser wall time
    const tz_min   = new Date().getTimezoneOffset(); // minutes west of UTC (e.g. Türkiye = -180)
    await fetch('/api/time',{method:'POST', headers:{'Content-Type':'application/json'},
      body: JSON.stringify({epoch_ms, tz_min})});
  }catch(e){ console.warn('time sync failed', e); }
}

async function loadSettings(){
  const r = await fetch('/api/settings'); const s = await r.json();
  document.getElementById('invert').checked = !!s.invert;
  document.getElementById('logms').value   = s.logms;
}

async function refreshFiles(){
  const r = await fetch('/api/files'); const j = await r.json();
  const sel = document.getElementById('files'); sel.innerHTML='';
  (j.files||[]).forEach(f=>{
    const name = (typeof f === 'string') ? f : f.name;
    const size = (typeof f === 'object' && f.size!=null) ? ` (${f.size} B)` : '';
    const o=document.createElement('option'); o.value=name; o.textContent=name+size; sel.appendChild(o);
  });
}

async function poll(){
  try{
    const r = await fetch('/api/sample'); if(!r.ok) throw 0;
    const j = await r.json();

    logging = !!j.logging; curFile = j.curFile || "";
    document.getElementById('btnLog').textContent = logging? 'Stop logging' : 'Start logging';
    document.getElementById('btnDl').disabled = !curFile;

    setState(`ΔP=${fmt(j.dp)} Pa, V=${fmt(j.va)} m/s, T=${fmt(j.tc)} °C${logging? ' — logging → '+curFile:''}`);

    // env
    if (typeof j.rho === 'number' && typeof j.ap === 'number' && typeof j.at === 'number') {
      const env = document.getElementById('env');
      const hpa = (j.ap/100).toFixed(1);
      const rh  = (j.hasH && typeof j.ah === 'number') ? `, RH=${j.ah.toFixed(0)}%` : '';
      env.textContent = `ρ=${j.rho.toFixed(3)} kg/m³, P=${hpa} hPa, T=${j.at.toFixed(1)} °C${rh}`;
    }

    // Use real timestamp `ts` (epoch ms from device)
    const t = new Date(j.ts || j.t); // fallback to t if ts absent
    const sm = document.getElementById('smooth').checked;
    live.t.push(t); live.dp.push(sm? j.dp_s : j.dp); live.va.push(j.va); live.tc.push(sm? j.tc_s : j.tc);
    if (live.t.length>1200){ live.t.shift(); live.dp.shift(); live.va.shift(); live.tc.shift(); }
    ch1.update(); ch2.update();
  }catch(e){ setState('disconnected…'); }
  setTimeout(poll, 200);
}

// SAVE
document.getElementById('btnSave').onclick = async ()=>{
  try{
    const invert = document.getElementById('invert').checked;
    const logms  = +document.getElementById('logms').value;
    const r = await fetch('/api/settings',{
      method:'POST', headers:{'Content-Type':'application/json'},
      body: JSON.stringify({ invert, logms })
    });
    if(!r.ok) throw new Error('save failed');
    alert(`Saved ✓\nLog period: ${logms} ms\nInvert ΔP: ${invert ? 'on' : 'off'}`);
  }catch(e){ console.error(e); alert('Save failed'); }
};

document.getElementById('btnZero').onclick = async ()=>{
  if (!confirm('Zero differential pressure sensor?\n\nMake sure both ports are open to still air.')) return;
  const btn = document.getElementById('btnZero');
  btn.disabled = true;
  setState('zeroing…');
  try {
    const r = await fetch('/api/zero',{method:'POST'});
    const j = await r.json();
    if (!r.ok || !j.ok) throw new Error('zero failed');
    setState('zeroed');
    alert(`Zero completed ✓\nSamples: ${j.samples}\nNew dp_zero: ${j.dp_zero.toFixed(4)} Pa`);
  } catch(e) {
    console.error(e);
    setState('zero failed');
    alert('Zero failed ❌');
  } finally {
    btn.disabled = false;
  }
};

document.getElementById('btnLog').onclick  = async ()=>{
  try{
    const cmd = logging? 'stop' : 'start';
    const r = await fetch('/api/log?cmd='+cmd,{method:'POST'});
    const j = await r.json();
    if (j.error) { alert(`Logging error: ${j.error}${j.fs_total? `\nFS: ${j.fs_used}/${j.fs_total}`:''}`); }
    logging = !!j.logging; 
    curFile = j.curFile || "";
    document.getElementById('btnLog').textContent = logging? 'Stop logging' : 'Start logging';
    document.getElementById('btnDl').disabled = !curFile;
    setTimeout(refreshFiles, 200);
  }catch(e){ console.error(e); alert('Logging command failed'); }
};

document.getElementById('btnDl').onclick   = ()=>{ if (curFile) location.href='/download?file='+encodeURIComponent(curFile); };

document.getElementById('btnDelOne').onclick= async ()=>{
  const f=document.getElementById('files').value;
  if(!f){ alert('Select a file first'); return; }
  if(!confirm('Delete '+f+' ?')) return;
  try{
    const r = await fetch('/api/delete?file='+encodeURIComponent(f),{method:'POST'});
    const j = await r.json();
    if (!j.ok) alert('Delete failed');
    refreshFiles();
  }catch(e){ console.error(e); alert('Delete failed'); }
};

document.getElementById('btnFormat').onclick= async ()=>{
  if(!confirm('FORMAT the filesystem and reboot? All files will be erased.')) return;
  try{
    await fetch('/api/format',{method:'POST'});
    alert('Formatted; device will reboot…');
    setTimeout(()=>location.reload(), 4000);
  }catch(e){ console.error(e); alert('Format failed'); }
};

document.getElementById('btnGet').onclick  = ()=>{
  const f=document.getElementById('files').value;
  if(f) location.href='/download?file='+encodeURIComponent(f);
};

// Kickoff
(async()=>{ await syncTime(); await loadSettings(); await refreshFiles(); poll(); })();
)JS";

// ------------------- Endpoints -------------------
void setupHTTP(WebServer& server, void (*saveSettingsCb)()) {
  saveSettingsFn = saveSettingsCb;

  // UI + JS
  server.on("/", HTTP_GET, [&](){ server.send_P(200, "text/html", INDEX_HTML); });
  server.on("/app.js", HTTP_GET, [&](){ server.send_P(200, "application/javascript", APP_JS); });



  // Zero (works with doZero(ms) that returns void)
server.on("/api/zero", HTTP_POST, [&](){
  stopLogging();
  uint16_t n=0;
  bool ok = doZero(2000, &n);
  if (saveSettingsFn) saveSettingsFn();
  String j = "{";
  j += "\"ok\":" + String(ok ? "true" : "false") + ",";
  j += "\"dp_zero\":" + String(dp_zero, 4) + ",";
  j += "\"samples\":" + String((unsigned)n);
  j += "}";
  server.send(200, "application/json", j);
});

  // Logging control
  server.on("/api/log", HTTP_POST, [&](){
    String cmd = server.hasArg("cmd") ? server.arg("cmd") : "";
    if      (cmd=="start") startLogging();
    else if (cmd=="stop")  stopLogging();
    else { server.send(400, "application/json", "{\"error\":\"bad cmd\"}"); return; }

    String name = currentLogName();
    if (cmd=="start" && !loggingOn) {
      size_t total = SPIFFS.totalBytes();
      size_t used  = SPIFFS.usedBytes();
      String j = "{";
      j += "\"logging\":false,";
      j += "\"curFile\":\"\",";
      j += "\"error\":\"open failed (FS metadata likely full)\",";
      j += "\"fs_total\":" + String((unsigned long)total) + ",";
      j += "\"fs_used\":"  + String((unsigned long)used)  + "}";
      server.send(200, "application/json", j);
      return;
    }

    String j = String("{\"logging\":") + (loggingOn?"true":"false") +
               ",\"curFile\":\"" + name + "\"}";
    server.send(200, "application/json", j);
  });

  // Files & download
  server.on("/api/files", HTTP_GET, [&](){ listFilesJSON(server); });
  // Receive browser wall time and compute offset
server.on("/api/time", HTTP_POST, [&](){
  long long epoch_ms = 0;
  int tz_min = 0;
  if (server.hasArg("plain")) {
    String body = server.arg("plain");
    int i, c;

    if ((i = body.indexOf("\"epoch_ms\"")) != -1) {
      c = body.indexOf(':', i);
      // parse as 64-bit
      String v = body.substring(c+1);
      v.trim();
      epoch_ms = atoll(v.c_str());
    }
    if ((i = body.indexOf("\"tz_min\"")) != -1) {
      c = body.indexOf(':', i);
      tz_min = body.substring(c+1).toInt();
    }
  }
  if (epoch_ms > 0) {
    g_timeOffsetMs = epoch_ms - (long long)millis();
    g_tzOffsetMin  = tz_min;
    server.send(200, "application/json",
      String("{\"ok\":true,\"offset_ms\":") + g_timeOffsetMs + ",\"tz_min\":" + g_tzOffsetMin + "}");
  } else {
    server.send(400, "application/json", "{\"ok\":false}");
  }
});

// Live sample JSON — add ts (epoch ms)
server.on("/api/sample", HTTP_GET, [&](){
  Sample s; s.t_ms=lastS.t_ms; s.dp_Pa=lastS.dp_Pa; s.temp_C=lastS.temp_C; s.Va_mps=lastS.Va_mps;
  String cur = currentLogName();
  unsigned long t_ms = (unsigned long)s.t_ms;
  unsigned long long ts = (unsigned long long)g_timeOffsetMs + (unsigned long long)t_ms;

  String j = "{";
  j += "\"t\":"   + String(t_ms) + ",";                    // legacy ms
  j += "\"ts\":"  + String((unsigned long long)ts) + ",";
  j += "\"dp\":"  + String(s.dp_Pa, 4) + ",";
  j += "\"tc\":"  + String(s.temp_C, 3) + ",";
  j += "\"va\":"  + String(s.Va_mps, 4) + ",";
  j += "\"rho\":" + String(rho, 4) + ",";
  j += "\"ap\":"  + String(isnan(envP_Pa)?0:envP_Pa, 1) + ",";
  j += "\"at\":"  + String(isnan(envT_C)?0:envT_C, 2) + ",";
  j += "\"ah\":"  + String(isnan(envRH)?0:envRH, 1) + ",";
  j += "\"hasH\":"+ String(envHasHum ? "true" : "false") + ",";
  // smoothing
  static float adp=0, atc=0; static int n=0; n = min(10, n+1);
  adp += (s.dp_Pa - adp)/n; atc += (s.temp_C - atc)/n;
  j += "\"dp_s\":" + String(adp, 4) + ",";
  j += "\"tc_s\":" + String(atc, 4) + ",";
  j += "\"logging\":" + String(loggingOn ? "true" : "false") + ",";
  j += "\"curFile\":\"" + cur + "\"}";
  server.send(200, "application/json", j);
});

  // Files & download (no stopLogging)
// Files & download (no stopLogging)
server.on("/download", HTTP_GET, [&](){
  String fn = server.hasArg("file") ? server.arg("file") : String("");
  if (fn.length() && fn.charAt(0) != '/') fn = "/" + fn;
  if (fn.length()==0 || !SPIFFS.exists(fn)) { server.send(404, "text/plain", "no such file"); return; }

  // try direct read while logging (no stopLogging)
  File f = SPIFFS.open(fn, FILE_READ);
  if (!f) {
    // fallback: snapshot copy if concurrent open fails
    String snap = "/_snap.csv";
    if (SPIFFS.exists(snap)) SPIFFS.remove(snap);

    File src = SPIFFS.open(fn, FILE_READ);
    File dst = SPIFFS.open(snap, FILE_WRITE);
    if (src && dst) {
      uint8_t buf[1024];
      int n;
      while ((n = src.read(buf, sizeof(buf))) > 0) dst.write(buf, n);
      src.close(); dst.close();
      f = SPIFFS.open(snap, FILE_READ);
    }
    if (!f) { server.send(500, "text/plain", "open failed"); return; }
  }

  // Suggest a file name to the browser (strip leading '/')
  server.sendHeader("Content-Disposition",
                  "attachment; filename=\"" + fn.substring(1) + "\"");

  // streamFile will set Content-Type and Content-Length automatically
  server.streamFile(f, "text/csv");
  f.close();
});

  // Delete selected file (any file)
  server.on("/api/delete", HTTP_POST, [&](){
    String fn = server.hasArg("file") ? server.arg("file") : String("");
    if (fn.length() && fn.charAt(0) != '/') fn = "/" + fn;
    if (fn==currentLogName()) stopLogging();
    bool ok=false;
    if (fn.length() && SPIFFS.exists(fn)) ok = SPIFFS.remove(fn);
    server.send(200, "application/json", String("{\"ok\":") + (ok?"true":"false") + "}");
  });

  // FORMAT FS (danger) + reboot
  server.on("/api/format", HTTP_POST, [&](){
    stopLogging();
    bool ok = SPIFFS.format();
    server.send(200, "application/json", String("{\"formatted\":") + (ok?"true":"false") + "}");
    delay(250);
    ESP.restart();
  });

  // Settings (GET/POST)
  server.on("/api/settings", HTTP_GET, [&](){
    String j = "{";
    j += "\"invert\":" + String(invertDP ? "true" : "false") + ",";
    j += "\"logms\":"  + String(logEveryMs) + ",";
    j += "\"dp_zero\":" + String(dp_zero, 4);
    j += "}";
    server.send(200, "application/json", j);
  });

  server.on("/api/settings", HTTP_POST, [&](){
    if (server.hasArg("plain")) {
      String body = server.arg("plain");
      bool ninv = invertDP; 
      uint32_t nms = logEveryMs;
      int i;
      if ((i = body.indexOf("\"invert\""))!=-1){ int c = body.indexOf(':', i); ninv = body.substring(c+1, c+6).indexOf("true")!=-1; }
      if ((i = body.indexOf("\"logms\""))!=-1) { int c = body.indexOf(':', i); nms  = (uint32_t) body.substring(c+1).toInt(); }
      invertDP   = ninv;
      logEveryMs = max<uint32_t>(10, nms);
      if (saveSettingsFn) saveSettingsFn();
    }
    server.send(200, "text/plain", "OK");
  });

  server.onNotFound([&](){ server.send(404, "text/plain", "404"); });
  server.begin();
}