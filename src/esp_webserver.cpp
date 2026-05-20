#include <WebServer.h>
#include "esp_webserver.h"
#include <LittleFS.h>
#include <Arduino.h>
#include <ArduinoJson.h>
#include "inverter_comm.h"
#include "config.h"
#include "phone.h"
#include "relay.h"
#include <math.h>

// `server` is defined in main.cpp; declare it here for use in this TU.
extern WebServer server;

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
  doc["af"]  = s.ac_out_frequency;
  doc["aa"]  = s.ac_apparent_va;
  doc["aw"]  = s.ac_active_w;
  doc["lp"]  = s.load_percent;
  doc["bv"]  = s.batt_voltage;
  doc["bcc"] = s.batt_charge_current;
  doc["bs"]  = s.batt_soc;
  doc["ht"]  = s.heatsink_temp;
  doc["pi"]  = s.pv_input_current;
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
  server.on("/cmd", HTTP_POST, handleCmdHttp);
  server.on("/phone_battery", HTTP_POST, handlePhoneBattery);
  server.on("/upload", HTTP_GET, handleUploadPage);
  server.on("/upload", HTTP_POST, handleUploadComplete, handleUploadData);
  server.onNotFound(handleNotFound);
}
