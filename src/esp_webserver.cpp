#include <WebServer.h>
#include "esp_webserver.h"
#include <LittleFS.h>
#include <Arduino.h>
#include <ArduinoJson.h>
#include "inverter_comm.h"
#include "config.h"
#include "relay.h"

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
static String makeStatusJson() {
  JsonDocument doc;
  doc["type"] = "status";
  InverterState s = {};
  inverter_get_status(&s);
  // Also obtain current mode (thread-safe accessor)
  char mode_code = '\0';
  char mode_name[32] = "";
  inverter_get_mode(&mode_code, mode_name, sizeof(mode_name));
  // Insert attributes in the order requested by the UI
  doc["ac_out_voltage"] = s.ac_out_voltage;
  doc["ac_out_frequency"] = s.ac_out_frequency;
  doc["ac_apparent_va"] = s.ac_apparent_va;
  doc["ac_active_w"] = s.ac_active_w;
  doc["load_percent"] = s.load_percent;
  doc["batt_voltage"] = s.batt_voltage;
  doc["batt_charge_current"] = s.batt_charge_current;
  doc["batt_soc"] = s.batt_soc;
  doc["heatsink_temp"] = s.heatsink_temp;
  doc["pv_input_current"] = s.pv_input_current;
  doc["pv_input_voltage"] = s.pv_input_voltage;
  doc["batt_voltage_from_scc"] = s.batt_voltage_from_scc;
  doc["batt_discharge_current"] = s.batt_discharge_current;
  doc["pv_charging_power"] = s.pv_charging_power;
  doc["g_inverter_mode_code"] = String(mode_code);
  doc["g_inverter_mode_name"] = mode_name;
  // Map InverterState to UI schema
  doc["valid"] = g_inverter_data_valid;
  doc["ts_ms"] = s.ts_ms;
  doc["temp_h"] = isnan(g_temp_h) ? JsonVariant() : g_temp_h;
  doc["temp_l"] = isnan(g_temp_l) ? JsonVariant() : g_temp_l;

  doc["charger_on"] = isMobileChargerOn();

  // System diagnostics
  doc["reset_reason"] = (int)g_reset_reason_ws;
  doc["reset_reason_str"] = g_reset_reason_str_ws;

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

  if (strcmp(name, "set_charger") == 0) {
    bool on = doc["value"].as<bool>();
    setMobileCharger(on);
    Serial.printf("[CMD] set_charger: %s\n", on ? "ON" : "OFF");
    return makeAckJson(on ? "Charger ON" : "Charger OFF");
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
  server.on("/upload", HTTP_GET, handleUploadPage);
  server.on("/upload", HTTP_POST, handleUploadComplete, handleUploadData);
  server.onNotFound(handleNotFound);
}
