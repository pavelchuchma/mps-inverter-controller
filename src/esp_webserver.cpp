#include <WebServer.h>
#include "esp_webserver.h"
#include <LittleFS.h>
#include <Arduino.h>
#include <ArduinoJson.h>
#include "inverter_comm.h"
#include "config.h"
#include "phone.h"
#include "relay.h"
#include "utils.h"
#include <math.h>

// `server` is defined in main.cpp; declare it here for use in this TU.
extern WebServer server;

static bool pendingRestart = false;

void initWebServer() {
  if (!LittleFS.begin()) {
    Serial.println("LittleFS mount failed");
  }
}

void handleRoot() {
  server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  server.sendHeader("Pragma", "no-cache");
  server.sendHeader("Expires", "-1");
  File f = LittleFS.open("/index.html", "r");
  if (!f) {
    server.send(500, "text/plain", "index.html not found");
    return;
  }
  server.streamFile(f, "text/html; charset=utf-8");
  f.close();
}

void handleNotFound() {
  String uri = server.uri();
  String path = uri;
  if (!path.startsWith("/")) path = "/" + path;

  if (LittleFS.exists(path)) {
    File f = LittleFS.open(path, "r");
    String contentType = "text/plain";
    if (path.endsWith(".html")) contentType = "text/html; charset=utf-8";
    else if (path.endsWith(".css")) contentType = "text/css";
    else if (path.endsWith(".js")) contentType = "application/javascript";
    else if (path.endsWith(".png")) contentType = "image/png";
    else if (path.endsWith(".svg")) contentType = "image/svg+xml";

    server.streamFile(f, contentType);
    f.close();
    return;
  }

  server.send(404, "text/plain", "Not found");
}

// ---- Internal state for reset info (provided by main.cpp during setup) ----
static int g_reset_reason_ws = 0;
static const char* g_reset_reason_str_ws = "";

void webserver_set_reset_info(int reason, const char* reason_str) {
  g_reset_reason_ws = reason;
  g_reset_reason_str_ws = reason_str ? reason_str : "";
}

// --------- JSON helpers (moved from main.cpp) ----------
// Short JSON keys to minimize GSM payload (~50% smaller); data/app.js reads matching short names.
static String makeStatusJson() {
  JsonDocument doc;
  InverterState s = {};
  inverter_get_status(&s);
  char mode_code = '\0';
  char mode_name[32] = "";
  inverter_get_mode(&mode_code, mode_name, sizeof(mode_name));
  doc["av"]  = s.ac_out_voltage;
  doc["aa"]  = s.ac_apparent_va;
  doc["aw"]  = s.ac_active_w;
  doc["lp"]  = s.load_percent;
  doc["bv"]  = s.batt_voltage;
  doc["bcc"] = s.batt_charge_current;
  doc["bs"]  = s.batt_soc;
  doc["ht"]  = s.heatsink_temp;
  doc["pi"]  = s.pv_input_current_batt;
  doc["piv"] = s.pv_input_voltage;
  doc["bvs"] = s.batt_voltage_from_scc;
  doc["bdc"] = s.batt_discharge_current;
  doc["pcp"] = s.pv_charging_power;
  doc["mc"]  = String(mode_code);
  doc["mn"]  = mode_name;
  doc["iv"]  = g_inverter_data_valid;
  doc["ts"]  = s.ts_ms;
  doc["th"]  = isnan(g_temp_h) ? JsonVariant() : g_temp_h;
  doc["tl"]  = isnan(g_temp_l) ? JsonVariant() : g_temp_l;

  doc["co"]  = isMobileChargerOn();
  doc["bp"]  = (int)getBoilerPower();
  doc["bo"]  = isBoilerOn();
  doc["bf"]  = isBoilerFault();
  doc["bfr"] = getBoilerFaultReason() ? getBoilerFaultReason() : "";

  // Phone snapshot. stale_secs reported by the phone is added to the on-ESP
  // snapshot age so the UI sees the true age of the underlying measurement,
  // not just how long ago we received the (already-stale) data.
  PhoneState ph = {};
  bool phoneValid = phone_get_status(&ph);
  doc["phv"] = phoneValid;
  if (phoneValid) {
    float snapshot_age_secs = (float)(millis() - ph.ts_ms) / 1000.0f;
    float batt_stale = isnan(ph.battery_stale_secs) ? 0.0f : ph.battery_stale_secs;
    float net_stale  = isnan(ph.network_stale_secs) ? 0.0f : ph.network_stale_secs;
    doc["phbp"]  = ph.battery_percentage;
    doc["phbs"]  = ph.battery_status;
    doc["phbc"]  = ph.battery_current_ua / 1000;
    doc["phbss"] = batt_stale + snapshot_age_secs;
    doc["phrx"]  = ph.net_rmnet0_rx_bytes;
    doc["phtx"]  = ph.net_rmnet0_tx_bytes;
    doc["phns"]  = net_stale + snapshot_age_secs;
  }

  doc["rr"]  = (int)g_reset_reason_ws;
  doc["rrs"] = g_reset_reason_str_ws;

  String out;
  serializeJson(doc, out);
  return out;
}

static String makeAckJson(const char* msg) {
  JsonDocument doc;
  doc["type"] = "ack";
  doc["ok"] = true;
  doc["msg"] = msg;
  String out;
  serializeJson(doc, out);
  return out;
}

static String makeErrJson(const char* code, const char* msg) {
  JsonDocument doc;
  doc["type"] = "err";
  doc["ok"] = false;
  doc["code"] = code;
  doc["msg"] = msg;
  String out;
  serializeJson(doc, out);
  return out;
}

// --------- Command handling ----------
static String handleCommand(JsonDocument& doc) {
  // Expected: { "type":"cmd", "name":"...", "value": ... }
  const char* name = doc["name"].as<const char*>();
  if (!name) {
    Serial.println("[CMD] missing name field");
    return makeErrJson("bad_request", "Missing 'name'");
  }

  if (strcmp(name, "set_boiler") == 0) {
    int val = doc["value"].as<int>();
    if (val < 0 || val > 3) {
      return makeErrJson("bad_value", "boiler power must be 0..3");
    }
    setBoilerPower((BoilerPower)val);
    const char* labels[] = {"OFF (0W)", "500W", "1000W", "2000W"};
    Serial.printf("[CMD] set_boiler: %s\n", labels[val]);
    char msg[32];
    snprintf(msg, sizeof(msg), "Boiler %s", labels[val]);
    return makeAckJson(msg);
  }

  if (strcmp(name, "clear_log") == 0) {
    File f = LittleFS.open("/app.log", "w");
    if (!f) {
      Serial.println("[CMD] clear_log: open failed");
      return makeErrJson("io_error", "Failed to open /app.log");
    }
    f.close();
    Serial.println("[CMD] clear_log: /app.log truncated");
    return makeAckJson("Log cleared");
  }

  if (strcmp(name, "restart") == 0) {
    printInfo("Restart requested from web UI (%s)",
              server.client().remoteIP().toString().c_str());
    pendingRestart = true;
    return makeAckJson("Restarting");
  }

  return makeErrJson("unknown_cmd", "Unknown command name");
}

// --------- HTTP API handlers (status + command via POST) ---------
static void handleStatus() {
  server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  server.sendHeader("Pragma", "no-cache");
  server.sendHeader("Expires", "-1");
  String s = makeStatusJson();
  server.send(200, "application/json", s);
}

// --------- Inverter configuration read-back (on-demand) ---------
// Queries QPIRI/QFLAG/QMOD over RS232 right now and returns the raw payloads.
// All parsing/mapping to the manual is done client-side (data/settings.js), so
// the mapping can be tweaked by re-uploading web files without reflashing.
// Each query blocks up to ~1s, so this handler can take a couple of seconds.
static void handleInvConfig() {
  server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  server.sendHeader("Pragma", "no-cache");
  server.sendHeader("Expires", "-1");

  JsonDocument doc;
  String qpiri, qflag, qmod, qmchgcr;
  bool ok_qpiri = inverter_query_raw("QPIRI", qpiri);
  bool ok_qflag = inverter_query_raw("QFLAG", qflag);
  bool ok_qmod  = inverter_query_raw("QMOD", qmod);
  // Selectable max-charging-current values; used by the UI to offer valid
  // choices for the editable "max charging current" field. Best-effort.
  bool ok_qmchgcr = inverter_query_raw("QMCHGCR", qmchgcr);

  doc["ok"] = ok_qpiri && ok_qflag;
  doc["qpiri"] = ok_qpiri ? qpiri : String();
  doc["qflag"] = ok_qflag ? qflag : String();
  doc["qmod"]  = ok_qmod ? qmod : String();
  doc["qmchgcr"] = ok_qmchgcr ? qmchgcr : String();
  doc["ts"] = millis();

  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

// Allowlisted write-command prefixes for /inv_set. Only configuration writes the
// settings page is allowed to issue — nothing else can be sent to the inverter.
static const char* const INV_SET_ALLOWED[] = {
  "MCHGC", "PBCV", "PBDV", "PCVV", "PBFT", "PSDV"
};

// Validate a write command: prefix must be allowlisted and the value part may
// contain only digits and '.', with the whole command kept short.
static bool invSetCmdAllowed(const String& cmd) {
  if (cmd.length() < 4 || cmd.length() > 12) return false;
  const char* prefix = nullptr;
  for (const char* p : INV_SET_ALLOWED) {
    if (cmd.startsWith(p)) { prefix = p; break; }
  }
  if (!prefix) return false;
  for (size_t i = strlen(prefix); i < cmd.length(); ++i) {
    char c = cmd[i];
    if (!((c >= '0' && c <= '9') || c == '.')) return false;
  }
  return cmd.length() > strlen(prefix); // must carry a value
}

// --------- Inverter configuration write (on-demand) ---------
// Body: { "cmds": ["PCVV56.4", "PBFT54.5", ...] }
// Each command is allowlist-validated, then sent over RS232; ACK means applied.
static void handleInvSet() {
  if (!server.hasArg("plain")) {
    server.send(400, "application/json", makeErrJson("bad_request", "Missing body"));
    return;
  }
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, server.arg("plain"));
  if (err) {
    server.send(400, "application/json", makeErrJson("json_parse", err.c_str()));
    return;
  }
  JsonArray cmds = doc["cmds"].as<JsonArray>();
  if (cmds.isNull() || cmds.size() == 0) {
    server.send(400, "application/json", makeErrJson("bad_request", "Missing 'cmds'"));
    return;
  }

  JsonDocument out;
  JsonArray results = out["results"].to<JsonArray>();
  for (JsonVariant v : cmds) {
    String cmd = v.as<String>();
    JsonObject r = results.add<JsonObject>();
    r["cmd"] = cmd;
    if (!invSetCmdAllowed(cmd)) {
      r["ok"] = false;
      r["resp"] = "REJECTED";
      Serial.printf("[INV_SET] rejected: %s\n", cmd.c_str());
      continue;
    }
    String resp;
    bool sent = inverter_query_raw(cmd.c_str(), resp);
    bool ack = sent && resp == "ACK";
    r["ok"] = ack;
    r["resp"] = sent ? resp : String("NO_RESPONSE");
    printInfo("Inverter write %s -> %s", cmd.c_str(), ack ? "ACK" : (sent ? resp.c_str() : "NO_RESPONSE"));
  }

  String body;
  serializeJson(out, body);
  server.send(200, "application/json", body);
}

static void handleCmdHttp() {
  if (!server.hasArg("plain")) {
    server.send(400, "application/json", makeErrJson("bad_request", "Missing body"));
    return;
  }
  String body = server.arg("plain");
  // Parse JSON body
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, body);
  if (err) {
    server.send(400, "application/json", makeErrJson("json_parse", err.c_str()));
    return;
  }
  String reply = handleCommand(doc);
  server.send(200, "application/json", reply);
  if (pendingRestart) {
    server.client().flush();
    delay(200);
    ESP.restart();
  }
}

// --------- Phone battery status endpoint ---------
static void handlePhoneBattery() {
  String body = server.hasArg("plain") ? server.arg("plain") : String();
  Serial.printf("[PHONE_BATTERY] %s %s from %s, body: %s\n",
                server.method() == HTTP_POST ? "POST" : "?",
                server.uri().c_str(),
                server.client().remoteIP().toString().c_str(),
                body.c_str());
  server.send(200, "text/plain", "OK");
}

// --------- LittleFS file upload via HTTP multipart ---------
static File uploadFile;

static void handleUploadPage() {
  server.send(200, "text/html; charset=utf-8",
    "<!doctype html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Upload</title></head><body style='font-family:system-ui;padding:18px'>"
    "<h2>Upload file to LittleFS</h2>"
    "<form method='POST' action='/upload' enctype='multipart/form-data'>"
    "<input type='file' name='file' multiple><br><br>"
    "<button type='submit' style='padding:8px 16px;font-size:14px'>Upload</button>"
    "</form><br><a href='/'>Back</a>"
    "</body></html>");
}

static void handleUploadComplete() {
  server.send(200, "text/plain", "OK — upload complete. Refresh the main page.");
}

static void handleUploadData() {
  HTTPUpload& upload = server.upload();
  if (upload.status == UPLOAD_FILE_START) {
    String filename = upload.filename;
    if (!filename.startsWith("/")) filename = "/" + filename;
    Serial.printf("[UPLOAD] start: %s\n", filename.c_str());
    uploadFile = LittleFS.open(filename, "w");
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (uploadFile) {
      uploadFile.write(upload.buf, upload.currentSize);
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (uploadFile) {
      uploadFile.close();
      Serial.printf("[UPLOAD] done: %s (%u bytes)\n", upload.filename.c_str(), upload.totalSize);
    }
  }
}

void webserver_setup_routes() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/inv_config", HTTP_GET, handleInvConfig);
  server.on("/inv_set", HTTP_POST, handleInvSet);
  server.on("/cmd", HTTP_POST, handleCmdHttp);
  server.on("/phone_battery", HTTP_POST, handlePhoneBattery);
  server.on("/upload", HTTP_GET, handleUploadPage);
  server.on("/upload", HTTP_POST, handleUploadComplete, handleUploadData);
  server.onNotFound(handleNotFound);
}
