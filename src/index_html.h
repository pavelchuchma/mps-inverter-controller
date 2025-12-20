// Generated file: extracted HTML for the captive portal
// Keep as a single PROGMEM string to reduce RAM usage.
const char INDEX_HTML[] PROGMEM = R"HTML(
<!doctype html>
<html>
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>FV Dashboard</title>
  <style>
    body{font-family:system-ui,sans-serif;margin:0;padding:18px;background:#f6f7f9;}
    .grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(220px,1fr));gap:12px;}
    .card{background:#fff;border:1px solid #e6e8ee;border-radius:14px;padding:14px;box-shadow:0 1px 2px rgba(0,0,0,.04);}
    .k{font-size:12px;opacity:.65;}
    .v{font-size:28px;font-weight:650;margin-top:6px;}
    .row{display:flex;justify-content:space-between;gap:10px;align-items:center;}
    .pill{font-size:12px;padding:4px 8px;border-radius:999px;background:#eef2ff;}
    .err{background:#ffecec;}
    .ok{background:#eaffea;}
    button{padding:10px 14px;font-size:14px;border-radius:12px;border:1px solid #ddd;background:#fff;}
    input[type="range"]{width:100%;}
    pre{background:#111827;color:#e5e7eb;padding:10px;border-radius:12px;overflow:auto;margin:8px 0 0;}
    .muted{opacity:.65;font-size:12px;}
  </style>
</head>
<body>
  <div class="row">
    <h2 style="margin:0;">FV Dashboard</h2>
    <span id="conn" class="pill">connecting…</span>
  </div>
  <p class="muted">WebSocket push (port 81)</p>

  <div class="grid">
    <div class="card"><div class="k">FV výkon</div><div class="v"><span id="pv">—</span> W</div></div>
    <div class="card"><div class="k">Baterie (SoC)</div><div class="v"><span id="soc">—</span> %</div></div>
    <div class="card"><div class="k">Napětí baterie</div><div class="v"><span id="bv">—</span> V</div></div>
    <div class="card"><div class="k">Zátěž</div><div class="v"><span id="load">—</span> W</div></div>
    <div class="card"><div class="k">Síť</div><div class="v" id="grid">—</div></div>
    <div class="card"><div class="k">Stav</div><div class="v" style="font-size:22px;" id="state">—</div>
      <div class="muted">ts: <span id="ts">—</span></div></div>
  </div>

  <div class="card" style="margin-top:12px;">
    <div class="k">Ovládání (demo)</div>
    <div class="row" style="margin-top:10px;">
      <button id="btnDemo">Toggle demo</button>
      <span class="muted">demo: <span id="demoVal">—</span></span>
    </div>

    <div style="margin-top:14px;">
      <div class="row">
        <div class="muted">Output limit (W)</div>
        <div><b id="limVal">—</b></div>
      </div>
      <input id="lim" type="range" min="0" max="5000" step="50" value="2000">
      <div class="row">
        <button id="btnApply">Apply limit</button>
        <span class="muted">mock command</span>
      </div>
    </div>

    <div class="muted" style="margin-top:10px;">Log:</div>
    <pre id="log">—</pre>
  </div>

<script>
const $ = (id) => document.getElementById(id);
function logln(s){
  const el = $("log");
  el.textContent = (el.textContent === "—" ? "" : el.textContent + "\n") + s;
  el.scrollTop = el.scrollHeight;
}
function setConn(ok, msg){
  const el = $("conn");
  el.textContent = msg;
  el.className = "pill " + (ok ? "ok" : "err");
}

let ws;
let demoMode = false;

function send(obj){
  const s = JSON.stringify(obj);
  logln("SEND: " + s);
  if(ws && ws.readyState === 1) ws.send(s);
}

function connect(){
  const url = `ws://${location.hostname}:81/`;
  logln("Connecting: " + url);
  ws = new WebSocket(url);

  ws.onopen = () => {
    setConn(true, "WS connected");
    send({type:"hello", role:"ui"});
  };

  ws.onclose = () => {
    setConn(false, "WS disconnected");
    logln("WS closed, reconnecting…");
    setTimeout(connect, 1000);
  };

  ws.onmessage = (ev) => {
    try{
      const j = JSON.parse(ev.data);

      if(j.type === "status"){
        $("pv").textContent = Math.round(j.pv_w);
        $("soc").textContent = Number(j.batt_soc).toFixed(1);
        $("bv").textContent = Number(j.batt_v).toFixed(2);
        $("load").textContent = Math.round(j.load_w);
        $("grid").textContent = j.grid_ok ? "OK" : "FAIL";
        $("state").textContent = j.state || "—";
        $("ts").textContent = j.ts_ms;

        demoMode = !!j.demo;
        $("demoVal").textContent = demoMode ? "true" : "false";

        const lim = Math.round(j.output_limit_w ?? 2000);
        $("limVal").textContent = lim;
        $("lim").value = lim;
        return;
      }

      if(j.type === "ack"){
        logln("ACK: " + (j.msg || "ok"));
        return;
      }

      if(j.type === "err"){
        logln("ERR: " + (j.code || "err") + " / " + (j.msg || ""));
        return;
      }

      logln("MSG: " + ev.data);
    }catch(e){
      logln("RAW: " + ev.data);
    }
  };
}

$("btnDemo").addEventListener("click", () => {
  demoMode = !demoMode;
  send({type:"cmd", name:"set_demo", value: demoMode});
});

$("lim").addEventListener("input", () => {
  $("limVal").textContent = $("lim").value;
});

$("btnApply").addEventListener("click", () => {
  const v = Number($("lim").value);
  send({type:"cmd", name:"set_output_limit_w", value: v});
});

connect();
</script>
</body>
</html>
)HTML";
