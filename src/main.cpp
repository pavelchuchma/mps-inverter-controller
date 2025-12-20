#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <WebSocketsServer.h>
#include <ArduinoJson.h>
#include <cmath>

const char* AP_SSID = "FV-Dashboard";
const char* AP_PASS = "12345678";

IPAddress apIP(192,168,4,1);
IPAddress netMsk(255,255,255,0);

DNSServer dnsServer;
WebServer server(80);
WebSocketsServer ws(81);

// WS helpers: accept by-value so callers can pass rvalues (makeStatusJson())
static inline void wsSend(uint8_t clientId, String s) {
  ws.sendTXT(clientId, s);
}

static inline void wsBroadcastTxt(String s) {
  ws.broadcastTXT(s);
}

// --------- App state ----------
struct Status {
  float pv_w;
  float batt_soc;
  float batt_v;
  float load_w;
  bool  grid_ok;
  const char* state;
  uint32_t ts_ms;
};

Status g;

bool demoMode = true;
int outputLimitW = 2000; // example “control” value (mock)

// --------- Mock status generation ----------
void updateMock() {
  static float t = 0;
  t += 0.12f;

  // Demo data
  g.pv_w = 1200 + 350 * sin(t);
  g.batt_soc = 60 + 12 * sin(t * 0.25f);
  g.batt_v = 52.1 + 0.45 * sin(t * 0.7f);
  g.load_w = 700 + 180 * sin(t * 0.5f);

  g.grid_ok = true;
  g.state = demoMode ? "Demo" : "Running";
  g.ts_ms = millis();
}

// --------- JSON helpers ----------
String makeStatusJson() {
  StaticJsonDocument<256> doc;
  doc["type"] = "status";
  doc["pv_w"] = (int)round(g.pv_w);
  doc["batt_soc"] = g.batt_soc;
  doc["batt_v"] = g.batt_v;
  doc["load_w"] = (int)round(g.load_w);
  doc["grid_ok"] = g.grid_ok;
  doc["state"] = g.state;
  doc["ts_ms"] = g.ts_ms;

  // Include some “control state” so UI can reflect it
  doc["demo"] = demoMode;
  doc["output_limit_w"] = outputLimitW;

  String out;
  serializeJson(doc, out);
  return out;
}

String makeAckJson(const char* msg) {
  StaticJsonDocument<160> doc;
  doc["type"] = "ack";
  doc["ok"] = true;
  doc["msg"] = msg;
  String out;
  serializeJson(doc, out);
  return out;
}

String makeErrJson(const char* code, const char* msg) {
  StaticJsonDocument<200> doc;
  doc["type"] = "err";
  doc["ok"] = false;
  doc["code"] = code;
  doc["msg"] = msg;
  String out;
  serializeJson(doc, out);
  return out;
}

// --------- WebSocket broadcast ----------
void wsBroadcastStatus() {
  wsBroadcastTxt(makeStatusJson());
}

// --------- Command handling ----------
String handleCommand(JsonDocument& doc) {
  // Expected: { "type":"cmd", "name":"...", "value": ... }
  const char* name = doc["name"] | nullptr;
  if (!name) return makeErrJson("bad_request", "Missing 'name'");

  // Example command: set_demo (bool)
  if (strcmp(name, "set_demo") == 0) {
    if (!doc.containsKey("value")) return makeErrJson("bad_request", "Missing 'value'");
    demoMode = doc["value"].as<bool>();
    return makeAckJson(demoMode ? "demo enabled" : "demo disabled");
  }

  // Example command: set_output_limit_w (int)
  if (strcmp(name, "set_output_limit_w") == 0) {
    if (!doc.containsKey("value")) return makeErrJson("bad_request", "Missing 'value'");
    int v = doc["value"].as<int>();
    if (v < 0 || v > 10000) return makeErrJson("range", "output_limit_w out of range");
    outputLimitW = v;
    return makeAckJson("output limit updated");
  }

  return makeErrJson("unknown_cmd", "Unknown command name");
}

// --------- WS events ----------
void wsEvent(uint8_t clientId, WStype_t type, uint8_t * payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED: {
      IPAddress ip = ws.remoteIP(clientId);
      Serial.printf("[WS] Client %u connected from %s\n", clientId, ip.toString().c_str());
      wsSend(clientId, makeStatusJson());
      break;
    }
    case WStype_DISCONNECTED:
      Serial.printf("[WS] Client %u disconnected\n", clientId);
      break;

    case WStype_TEXT: {
      // Parse incoming JSON
      StaticJsonDocument<512> doc;
      DeserializationError err = deserializeJson(doc, payload, length);
      if (err) {
        wsSend(clientId, makeErrJson("json_parse", err.c_str()));
        return;
      }

      const char* msgType = doc["type"] | "";
      if (strcmp(msgType, "hello") == 0) {
        wsSend(clientId, makeAckJson("hello"));
        wsSend(clientId, makeStatusJson());
        return;
      }

      if (strcmp(msgType, "cmd") == 0) {
        String reply = handleCommand(doc);
        wsSend(clientId, reply);

        // Optionally push fresh status right after a command
        wsBroadcastStatus();
        return;
      }

      wsSend(clientId, makeErrJson("bad_request", "Unknown message type"));
      break;
    }

    default:
      break;
  }
}

// --------- Captive portal HTML (same idea as before, with WS + a slider) ----------
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

// --------- HTTP handlers ----------
void handleRoot() {
  server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  server.sendHeader("Pragma", "no-cache");
  server.sendHeader("Expires", "-1");
  server.send(200, "text/html; charset=utf-8", INDEX_HTML);
}

void handleNotFound() {
  server.sendHeader("Location", String("http://") + apIP.toString() + "/", true);
  server.send(302, "text/plain", "");
}

void setup() {
  Serial.begin(921600);

  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(apIP, apIP, netMsk);
  WiFi.softAP(AP_SSID, AP_PASS);

  dnsServer.start(53, "*", apIP);

  server.on("/", HTTP_GET, handleRoot);

  // Captive portal helper endpoints (improves auto-open on OSes)
  server.on("/generate_204", HTTP_GET, handleRoot);
  server.on("/gen_204", HTTP_GET, handleRoot);
  server.on("/hotspot-detect.html", HTTP_GET, handleRoot);
  server.on("/success.txt", HTTP_GET, handleRoot);
  server.on("/ncsi.txt", HTTP_GET, handleRoot);
  server.on("/connecttest.txt", HTTP_GET, handleRoot);

  server.onNotFound(handleNotFound);
  server.begin();

  ws.begin();
  ws.onEvent(wsEvent);

  Serial.print("AP IP: "); Serial.println(WiFi.softAPIP());
  Serial.println("HTTP :80, WS :81");
}

uint32_t lastMock = 0;
uint32_t lastPush = 0;

void loop() {
  dnsServer.processNextRequest();
  server.handleClient();
  ws.loop();

  if (millis() - lastMock >= 250) {
    lastMock = millis();
    updateMock();
  }

  if (millis() - lastPush >= 1000) {
    lastPush = millis();
    wsBroadcastStatus();
  }
}
