#include <WebServer.h>
#include "esp_webserver.h"
#include <LittleFS.h>
#include <Arduino.h>
#include <ArduinoJson.h>
#include "inverter_comm.h"

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

// ---- Externals from main.cpp (control state reflected in JSON/commands) ----
extern bool demoMode;
extern int outputLimitW;
extern float outputDutyCycle;

// --------- JSON helpers (moved from main.cpp) ----------
static String makeStatusJson() {
  JsonDocument doc;
  doc["type"] = "status";
  InverterState s = {};
  inverter_get_status(&s);
  // Map InverterState to UI schema
  doc["valid"] = g_inverter_data_valid;
  doc["pv_w"] = s.pv_charging_power;                  // PV charging power [W]
  doc["batt_soc"] = s.batt_soc;                        // [%]
  doc["batt_v"] = s.batt_voltage;                      // [V]
  doc["load_w"] = s.ac_active_w;                        // [W]
  doc["grid_ok"] = g_inverter_data_valid ? (s.grid_voltage > 10.0f) : false; // if not valid, show grid unknown/false
  doc["state"] = demoMode ? "Demo" : "Running";
  doc["ts_ms"] = s.ts_ms;
  doc["temp_h"] = isnan(g_temp_h) ? JsonVariant() : g_temp_h;
  doc["temp_l"] = isnan(g_temp_l) ? JsonVariant() : g_temp_l;

  // Include some “control state” so UI can reflect it
  doc["demo"] = demoMode;
  doc["output_limit_w"] = outputLimitW;
  doc["output_duty_cycle"] = outputDutyCycle;

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

  // Example command: set_demo (bool)
  if (strcmp(name, "set_demo") == 0) {
    if (doc["value"].isNull()) return makeErrJson("bad_request", "Missing 'value'");
    demoMode = doc["value"].as<bool>();
    return makeAckJson(demoMode ? "demo enabled" : "demo disabled");
  }

  // Example command: set_output_limit_w (int)
  if (strcmp(name, "set_output_limit_w") == 0) {
    if (doc["value"].isNull()) return makeErrJson("bad_request", "Missing 'value'");
    int v = doc["value"].as<int>();
    if (v < 0 || v > 10000) return makeErrJson("range", "output_limit_w out of range");
    outputLimitW = v;
    return makeAckJson("output limit updated");
  }

  // Example command: set_output_duty_cycle (float 0.0 - 1.0)
  if (strcmp(name, "set_output_duty_cycle") == 0) {
    if (doc["value"].isNull()) return makeErrJson("bad_request", "Missing 'value'");
    float v = doc["value"].as<float>();
    if (v < 0.0f || v > 1.0f) return makeErrJson("range", "output_duty_cycle out of range");
    outputDutyCycle = v;
    return makeAckJson("duty cycle updated");
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

void webserver_setup_routes() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/cmd", HTTP_POST, handleCmdHttp);
  server.onNotFound(handleNotFound);
}
