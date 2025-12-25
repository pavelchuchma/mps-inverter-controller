#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <ArduinoJson.h>
#include <cmath>
#include "credentials.h"
#include <esp_system.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <stdarg.h>
#include <LittleFS.h>

IPAddress apIP(192, 168, 4, 1);
IPAddress netMsk(255, 255, 255, 0);

WebServer server(80);

// Format boot-relative milliseconds to HH:MM:SS.sss into provided buffer.
static inline void formatBootTimeMs(char* buf, size_t cap, uint32_t ms) {
  uint32_t total_ms = ms;
  uint32_t total_sec = total_ms / 1000;
  unsigned h = total_sec / 3600;
  unsigned m = (total_sec % 3600) / 60;
  unsigned s = total_sec % 60;
  unsigned ms_part = total_ms % 1000;
  if (cap > 0) {
    snprintf(buf, cap, "%02u:%02u:%02u.%03u", h, m, s, ms_part);
  }
}

// Print a WARN log with boot-relative timestamp. Accepts printf-style args.
static inline void printWarning(const char* fmt, ...) {
  char tbuf[16];
  formatBootTimeMs(tbuf, sizeof(tbuf), millis());
  char msg[384];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);
  char out[420];
  snprintf(out, sizeof(out), "[%s] [WARN] %s\n", tbuf, msg);
  Serial.print(out);
  // Append warning into LittleFS logfile
  if (LittleFS.begin()) {
    File f = LittleFS.open("/app.log", "a");
    if (f) {
      f.print(out);
      f.close();
    }
  }
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

// ---- Reset reason (persisted from setup) ----
static esp_reset_reason_t g_reset_reason = ESP_RST_UNKNOWN;
static const char* g_reset_reason_str = "UNKNOWN";

static const char* resetReasonToStr(esp_reset_reason_t r){
  switch(r){
    case ESP_RST_UNKNOWN:      return "ESP_RST_UNKNOWN: Reset reason can not be determined";
    case ESP_RST_POWERON:      return "ESP_RST_POWERON: Reset due to power-on event";
    case ESP_RST_EXT:          return "ESP_RST_EXT: Reset by external pin (not applicable for ESP32)";
    case ESP_RST_SW:           return "ESP_RST_SW: Software reset via esp_restart";
    case ESP_RST_PANIC:        return "ESP_RST_PANIC: Software reset due to exception/panic";
    case ESP_RST_INT_WDT:      return "ESP_RST_INT_WDT: Reset due to interrupt watchdog";
    case ESP_RST_TASK_WDT:     return "ESP_RST_TASK_WDT: Reset due to task watchdog";
    case ESP_RST_WDT:          return "ESP_RST_WDT: Reset due to other watchdogs";
    case ESP_RST_DEEPSLEEP:    return "ESP_RST_DEEPSLEEP: Reset after exiting deep sleep mode";
    case ESP_RST_BROWNOUT:     return "ESP_RST_BROWNOUT: Brownout reset (software or hardware)";
    case ESP_RST_SDIO:         return "ESP_RST_SDIO: Reset over SDIO";
    default:                   return "OTHER";
  }
}

bool demoMode = true;
int outputLimitW = 2000; // example “control” value (mock)
float outputDutyCycle = 0.0f; // 0.0 - 1.0 (represented as percent in UI)
// PWM-capable GPIO25
const int pwmPin = 25; // change if you want a different output-capable pin

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
  JsonDocument doc;
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
  doc["output_duty_cycle"] = outputDutyCycle;

  // System diagnostics
  doc["reset_reason"] = (int)g_reset_reason;
  doc["reset_reason_str"] = g_reset_reason_str;

  String out;
  serializeJson(doc, out);
  return out;
}

// Safer, no-String variant to reduce heap fragmentation on long runs
// Returns number of bytes written (excluding null-terminator); 0 on failure
size_t makeStatusJsonTo(char* buf, size_t cap) {
  // Reserve a static doc to avoid heap churn. Size estimate ~300B; use headroom.
  // Potlačení deprecation warningu pro StaticJsonDocument pouze zde
  // Increased capacity to accommodate longer reset_reason_str descriptions
  JsonDocument doc;
  doc["type"] = "status";
  doc["pv_w"] = (int)round(g.pv_w);
  doc["batt_soc"] = g.batt_soc;
  doc["batt_v"] = g.batt_v;
  doc["load_w"] = (int)round(g.load_w);
  doc["grid_ok"] = g.grid_ok;
  doc["state"] = g.state;
  doc["ts_ms"] = g.ts_ms;
  doc["demo"] = demoMode;
  doc["output_limit_w"] = outputLimitW;
  doc["output_duty_cycle"] = outputDutyCycle;
  doc["reset_reason"] = (int)g_reset_reason;
  doc["reset_reason_str"] = g_reset_reason_str;
  return serializeJson(doc, buf, cap);
}

String makeAckJson(const char* msg) {
  JsonDocument doc;
  doc["type"] = "ack";
  doc["ok"] = true;
  doc["msg"] = msg;
  String out;
  serializeJson(doc, out);
  return out;
}

String makeErrJson(const char* code, const char* msg) {
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
String handleCommand(JsonDocument& doc) {
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
void handleStatus() {
  server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  server.sendHeader("Pragma", "no-cache");
  server.sendHeader("Expires", "-1");
  String s = makeStatusJson();
  server.send(200, "application/json", s);
}

void handleCmdHttp() {
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

// --------- Embedded web UI helpers (LittleFS) ----------
#include "esp_webserver.h"
#include "inverter_comm.h"

void connectToWiFi() {
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  uint16_t connectWait = 240;
  while (WiFi.status() != WL_CONNECTED) {
    delay(250);
    Serial.printf(".");
    if (!connectWait--) {
      Serial.printf("Failed to connect, restarting!\n");
      ESP.restart();
    }
  }
}

void createWiFiAP() {
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(apIP, apIP, netMsk);
  WiFi.softAP(AP_SSID, AP_PASS);

  Serial.print("AP IP: "); Serial.println(WiFi.softAPIP());
}

void initializeWiFi() {
  connectToWiFi();
  // createWiFiAP();

  Serial.printf("\nWiFi connected, IP address: ");
  Serial.println(WiFi.localIP());
  if (!MDNS.begin("inverter")) {
    Serial.println("ERROR: setting up MDNS responder!");
  } else {
    Serial.println("mDNS responder started");
  }
}


void setup() {
  Serial.begin(921600);
  // Log reset reason to help diagnose unexpected restarts
  g_reset_reason = esp_reset_reason();
  g_reset_reason_str = resetReasonToStr(g_reset_reason);
  Serial.printf("[BOOT] reset reason=%d (%s)\n", (int)g_reset_reason, g_reset_reason_str);
  // Also log reboot reason as a WARN (will append to LittleFS via printWarning)
  printWarning("[BOOT] reset reason=%d (%s)", (int)g_reset_reason, g_reset_reason_str);

  // Initialize webserver / LittleFS (web UI files in data/ will be uploaded to device)
  initWebServer();

  initializeWiFi();

  server.on("/", HTTP_GET, handleRoot);
  // HTTP API for polling UI
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/cmd", HTTP_POST, handleCmdHttp);

  server.onNotFound(handleNotFound);
  server.begin();

  // Setup PWM output pin
  pinMode(pwmPin, OUTPUT);
  digitalWrite(pwmPin, LOW);
  Serial.println("HTTP :80");

  // Initialize inverter RS232 communication (background task)
//  inverter_comm_init();
}

uint32_t lastMock = 0;
uint32_t lastPush = 0;

void loop() {
  uint32_t t0 = millis();
  server.handleClient();
  uint32_t t1 = millis();

  // Log if handleClient takes unusually long (indicates blocking)
  uint32_t hdlDur = t1 - t0;
  if (hdlDur > 100) {
    printWarning("server.handleClient() took %ums", hdlDur);
  }

  if (millis() - lastMock >= 250) {
    lastMock = millis();
    updateMock();
  }

  // Software PWM: period 2000 ms (2s). Drive `pwmPin` HIGH for
  // outputDutyCycle * period, otherwise LOW. `outputDutyCycle` is 0.0-1.0.
  const uint32_t pwmPeriodMs = 2000;
  uint32_t phase = millis() % pwmPeriodMs;
  uint32_t onTime = (uint32_t)round(outputDutyCycle * (float)pwmPeriodMs);
  digitalWrite(pwmPin, (phase < onTime) ? HIGH : LOW);
  // Small yield to allow WiFi/RTOS background tasks to run and avoid starvation
  delay(5);

  // Periodic diagnostics to catch memory/stack issues causing resets after hours
  static uint32_t lastDiag = 0;
  if (millis() - lastDiag > 600000) { // every 600s
    lastDiag = millis();
    size_t freeHeap = ESP.getFreeHeap();
    size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
    printWarning("heap free=%u, largest=%u", (unsigned)freeHeap, (unsigned)largest);
  }
}