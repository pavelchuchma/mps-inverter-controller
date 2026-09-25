#include <WebServer.h>
#include "esp_webserver.h"
#include <LittleFS.h>
#include <Arduino.h>
#include <ArduinoJson.h>
#include "inverter_comm.h"
#include "pylontech_comm.h"
#include "pylontech_can.h"
#include "config.h"
#include "phone.h"
#include "relay.h"
#include "soc_guard.h"
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
// The bare /status carries only what the main page shows. /status?full=1 adds
// everything the details and settings pages need (data/details.js, data/settings.js).
static String makeStatusJson(bool full) {
  JsonDocument doc;
  InverterState s = {};
  inverter_get_status(&s);
  // Battery values (SoC, voltage, current) come directly from the Pylontech
  // console, independent of the inverter link.
  PylontechState bat = {};
  pylontech_get_status(&bat);
  doc["iv"]  = g_inverter_data_valid;
  doc["bav"] = g_pylontech_data_valid;
  doc["bs"]  = bat.soc;
  doc["bv"]  = bat.voltage;
  doc["bc"]  = bat.current;   // signed: + charge / - discharge
  doc["pcp"] = s.pv_charging_power;
  doc["lp"]  = s.load_percent;
  doc["th"]  = isnan(g_temp_h) ? JsonVariant() : g_temp_h;
  doc["tl"]  = isnan(g_temp_l) ? JsonVariant() : g_temp_l;

  doc["bp"]  = (int)getBoilerPower();
  doc["bman"] = isBoilerManual();   // boiler mode: true = Manual, false = Auto
  doc["bo"]  = isBoilerOn();
  doc["tt"]  = getBoilerTargetTemp();   // virtual thermostat target [°C]
  doc["tr"]  = isBoilerTargetReached(); // true = tank at target, heating blocked
  doc["bf"]  = isBoilerFault();
  doc["bfr"] = getBoilerFaultReason() ? getBoilerFaultReason() : "";
  // Main page flow diagram: house consumption, inverter mode name and the SoC
  // guard marker on the battery bar.
  doc["aw"]  = s.ac_active_w;
  char mode_code = '\0';
  char mode_name[32] = "";
  if (inverter_get_mode(&mode_code, mode_name, sizeof(mode_name)) && mode_code) {
    char ms[2] = {mode_code, '\0'};
    doc["im"]  = ms;         // QMOD letter
    doc["imn"] = mode_name;  // its name
  }
  SocGuardState sg = {};
  soc_guard_get(&sg);
  doc["sg"]  = sg.enabled;
  doc["sga"] = sg.enabled && sg.armed;
  doc["sgarm"] = SOC_GUARD_ARM_PCT;

  if (!full) {
    String out;
    serializeJson(doc, out);
    return out;
  }

  // ---- Inverter (QPIGS) ----
  doc["av"]  = s.ac_out_voltage;
  doc["ava"] = s.ac_apparent_va;
  doc["ht"]  = s.heatsink_temp;
  doc["piv"] = s.pv_input_voltage;
  // The inverter's own view of the battery, next to the console's above.
  doc["ibv"] = s.batt_voltage;
  doc["bca"] = s.batt_charge_current;
  doc["bda"] = s.batt_discharge_current;
  // QPIGS status bits, raw and decoded. `lo` = b4 "load status": the inverter's own
  // view of whether its AC output is switched on, independent of the sensed
  // voltage it reports in `av`.
  doc["dsb"] = s.device_status_bits;
  doc["asb"] = s.additional_status_bits;
  doc["lo"]  = (s.device_status_bits & 0x10) != 0;
  doc["ts"]  = s.ts_ms;

  // ---- Battery console (RS485) ----
  doc["bm"]  = bat.basic_status;   // battery mode: Idle / Charge / Discharge
  doc["bt"]  = bat.temperature;
  doc["bal"] = bat.system_alarm;
  doc["slp"] = pylontech_comm_paused();  // console link paused (telnet client connected)

  // ---- Battery CAN link ----
  // Only the fields the UI actually shows plus the two the cross-check row
  // needs; the full decoded set stays on /can, which is the diagnostic
  // endpoint. Sent even when stale so the UI can grey the row rather than
  // blank it.
  PylontechCanState can = {};
  pylontech_can_get(&can);
  doc["cav"] = pylontech_can_valid();
  doc["ccl"] = can.ccl_a;
  doc["dcl"] = can.dcl_a;
  doc["chv"] = can.charge_v;
  doc["soh"] = can.soh;
  doc["cbv"] = can.voltage_v;
  doc["cbc"] = can.current_a;   // signed, same convention as bc

  // ---- Mobile charger / SoC guard ----
  doc["co"]  = isMobileChargerOn();
  // SoC guard (doc/todo/007-soc-guard-cutoff.md): switch, armed state, the
  // cut-off it wants and its two thresholds, so the settings page shows the
  // numbers the firmware actually uses.
  // `sg`, `sga` and `sgarm` are already in the bare payload above.
  doc["sgl"] = sg.intended_lvd_v;
  if (sg.last_write_ms != 0) doc["sgok"] = sg.last_write_ok;  // absent = never written
  doc["sgdis"] = SOC_GUARD_DISARM_PCT;

  // ---- Phone snapshot ----
  // stale_secs reported by the phone is added to the on-ESP snapshot age so
  // the UI sees the true age of the underlying measurement, not just how long
  // ago we received the (already-stale) data.
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

  // ---- System ----
  doc["up"]  = millis();
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

// --------- Audit trail ----------
// Every change made through the web UI (a setting, an inverter write, a reboot,
// a log clear, a file upload) lands in app.log with the address of the client
// that made it, so a surprising state read back later can be traced to who
// did it and when. One helper, so the line shape stays uniform: grep "[AUDIT]".
static void auditLog(const char* fmt, ...) {
  char what[200];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(what, sizeof(what), fmt, ap);
  va_end(ap);
  printInfo("[AUDIT] %s from %s", what, server.client().remoteIP().toString().c_str());
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
    auditLog("boiler power set to %s", labels[val]);
    char msg[32];
    snprintf(msg, sizeof(msg), "Boiler %s", labels[val]);
    return makeAckJson(msg);
  }

  if (strcmp(name, "set_boiler_manual") == 0) {
    bool on = doc["value"].as<int>() != 0;
    setBoilerManual(on);
    auditLog("boiler mode set to %s", on ? "Manual" : "Auto");
    return makeAckJson(on ? "Boiler mode Manual" : "Boiler mode Auto");
  }

  if (strcmp(name, "set_boiler_target_temp") == 0) {
    int val = doc["value"].as<int>();
    if (val < BOILER_TARGET_MIN_C || val > BOILER_TARGET_MAX_C ||
        !setBoilerTargetTemp((uint8_t)val)) {
      return makeErrJson("bad_value", "target temperature must be 10..60");
    }
    auditLog("boiler target temperature set to %d C", val);
    char msg[32];
    snprintf(msg, sizeof(msg), "Boiler target %d C", val);
    return makeAckJson(msg);
  }

  if (strcmp(name, "set_soc_guard") == 0) {
    bool on = doc["value"].as<int>() != 0;
    soc_guard_set_enabled(on);
    auditLog("SoC guard set to %s", on ? "on" : "off");
    return makeAckJson(on ? "SoC guard on" : "SoC guard off");
  }

  if (strcmp(name, "clear_log") == 0) {
    File f = LittleFS.open("/app.log", "w");
    if (!f) {
      Serial.println("[CMD] clear_log: open failed");
      return makeErrJson("io_error", "Failed to open /app.log");
    }
    f.close();
    // Written after the truncation, so it is the first line of the new log:
    // the history is gone, but who removed it is not.
    auditLog("app.log cleared");
    return makeAckJson("Log cleared");
  }

  if (strcmp(name, "restart") == 0) {
    auditLog("restart requested");
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
  bool full = server.hasArg("full") && server.arg("full") != "0";
  String s = makeStatusJson(full);
  server.send(200, "application/json", s);
}

// --------- Battery CAN link diagnostics ---------
// Emit a decoded field, or null when the frame group carrying it has never
// arrived. A zero there is indistinguishable from a real measurement of zero,
// which is exactly the confusion a diagnostic endpoint must not create.
static void canField(JsonObject o, const char* key, float v, uint32_t ts) {
  if (ts) o[key] = v; else o[key] = nullptr;
}
static void canFieldInt(JsonObject o, const char* key, long v, uint32_t ts) {
  if (ts) o[key] = v; else o[key] = nullptr;
}


// Returns the last raw payload per identifier alongside the decoded values, so
// byte order and scaling stay verifiable at any time — during bring-up, and
// after any firmware change that touches the parser. See
// doc/battery_can_data_spec.md.
static void handleCan() {
  server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  server.sendHeader("Pragma", "no-cache");
  server.sendHeader("Expires", "-1");

  PylontechCanState s = {};
  PylontechCanLink lk = {};
  PylontechCanRaw raw[CAN_RAW_SLOTS] = {};
  pylontech_can_get(&s);
  pylontech_can_get_link(&lk);
  pylontech_can_get_raw(raw);
  unsigned long now = millis();

  JsonDocument doc;
  doc["valid"] = pylontech_can_valid();

  JsonObject frames = doc["frames"].to<JsonObject>();
  for (int i = 0; i < CAN_RAW_SLOTS; ++i) {
    if (raw[i].id == 0) continue;
    char key[8];
    snprintf(key, sizeof(key), "%03X", (unsigned)raw[i].id);
    char hex[24];
    int n = 0;
    for (int b = 0; b < raw[i].dlc && b < 8; ++b) {
      n += snprintf(hex + n, sizeof(hex) - n, b ? " %02X" : "%02X", raw[i].data[b]);
    }
    hex[n] = '\0';
    JsonObject f = frames[key].to<JsonObject>();
    f["raw"] = hex;
    f["age_ms"] = now - raw[i].ts_ms;
    f["count"] = raw[i].count;
  }

  JsonObject dec = doc["decoded"].to<JsonObject>();
  canField(dec, "charge_v", s.charge_v, s.limits_ts_ms);
  canField(dec, "ccl_a", s.ccl_a, s.limits_ts_ms);
  canField(dec, "dcl_a", s.dcl_a, s.limits_ts_ms);
  canFieldInt(dec, "soc", s.soc, s.soc_ts_ms);
  canFieldInt(dec, "soh", s.soh, s.soc_ts_ms);
  canField(dec, "voltage_v", s.voltage_v, s.measured_ts_ms);
  canField(dec, "current_a", s.current_a, s.measured_ts_ms);
  canField(dec, "temp_c", s.temp_c, s.measured_ts_ms);
  canFieldInt(dec, "protection", s.protection, s.alarms_ts_ms);
  canFieldInt(dec, "alarm", s.alarm, s.alarms_ts_ms);
  canFieldInt(dec, "modules", s.modules, s.alarms_ts_ms);
  canFieldInt(dec, "request_flags", s.request_flags, s.requests_ts_ms);

  JsonObject link = doc["link"].to<JsonObject>();
  link["state"] = lk.state;
  link["rx"] = lk.rx_frames;
  link["missed"] = lk.rx_missed;
  link["bus_err"] = lk.bus_errors;
  link["rec"] = lk.rx_err;
  link["tec"] = lk.tx_err;
  link["recoveries"] = lk.recoveries;
  link["tx_failed"] = lk.tx_failed;
  link["last_rx_age_ms"] = lk.last_rx_ms ? (now - lk.last_rx_ms) : 0;

  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
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
  // Diagnostics: warning/fault bitmap and the parallel-info record (carries a
  // fault code and its own load-on bit). Best-effort, raw payloads.
  String qpiws, qpgs0, qpigs;
  bool ok_qpiws = inverter_query_raw("QPIWS", qpiws);
  bool ok_qpgs0 = inverter_query_raw("QPGS0", qpgs0);
  bool ok_qpigs = inverter_query_raw("QPIGS", qpigs);

  doc["ok"] = ok_qpiri && ok_qflag;
  doc["qpiri"] = ok_qpiri ? qpiri : String();
  doc["qflag"] = ok_qflag ? qflag : String();
  doc["qmod"]  = ok_qmod ? qmod : String();
  doc["qmchgcr"] = ok_qmchgcr ? qmchgcr : String();
  doc["qpiws"] = ok_qpiws ? qpiws : String();
  doc["qpgs0"] = ok_qpgs0 ? qpgs0 : String();
  doc["qpigs"] = ok_qpigs ? qpigs : String();
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
      // A rejected command never reaches the inverter, but somebody sent it
      // and that is worth the same trace as an accepted one.
      auditLog("inverter write %s rejected", cmd.c_str());
      continue;
    }
    String resp;
    bool sent = inverter_query_raw(cmd.c_str(), resp);
    bool ack = sent && resp == "ACK";
    r["ok"] = ack;
    r["resp"] = sent ? resp : String("NO_RESPONSE");
    auditLog("inverter write %s -> %s", cmd.c_str(), ack ? "ACK" : (sent ? resp.c_str() : "NO_RESPONSE"));
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
      auditLog("file %s uploaded (%u bytes)", upload.filename.c_str(), (unsigned)upload.totalSize);
    }
  }
}

void webserver_setup_routes() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/can", HTTP_GET, handleCan);
  server.on("/inv_config", HTTP_GET, handleInvConfig);
  server.on("/inv_set", HTTP_POST, handleInvSet);
  server.on("/cmd", HTTP_POST, handleCmdHttp);
  server.on("/phone_battery", HTTP_POST, handlePhoneBattery);
  server.on("/upload", HTTP_GET, handleUploadPage);
  server.on("/upload", HTTP_POST, handleUploadComplete, handleUploadData);
  server.onNotFound(handleNotFound);
}
