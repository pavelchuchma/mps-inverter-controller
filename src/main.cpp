#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <WebSocketsServer.h>
#include <ArduinoJson.h>
#include <cmath>
#include "credentials.h"
#include <esp_system.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

IPAddress apIP(192, 168, 4, 1);
IPAddress netMsk(255, 255, 255, 0);

WebServer server(80);
WebSocketsServer ws(81);

// Track connected WS clients to avoid broadcasting when nobody is listening
static int g_wsClientCount = 0;
static TaskHandle_t g_wsTask = nullptr; // handle WS broadcast task for diagnostics

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
  #pragma GCC diagnostic push
  #pragma GCC diagnostic ignored "-Wdeprecated-declarations"
  // Increased capacity to accommodate longer reset_reason_str descriptions
  StaticJsonDocument<768> doc;
  #pragma GCC diagnostic pop
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

// --------- WebSocket broadcast ----------
void wsBroadcastStatus() {
  if (g_wsClientCount <= 0) return; // nothing to send
  // Use fixed buffer to avoid dynamic allocation of String
  static char buf[512];
  size_t n = makeStatusJsonTo(buf, sizeof(buf));
  if (n > 0) {
    ws.broadcastTXT((uint8_t*)buf, n);
  }
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

// --------- WS events ----------
void wsEvent(uint8_t clientId, WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
  case WStype_CONNECTED: {
    IPAddress ip = ws.remoteIP(clientId);
    Serial.printf("[WS] Client %u connected from %s\n", clientId, ip.toString().c_str());
    ++g_wsClientCount;
    // Send initial status using fixed buffer to minimize heap churn
    static char buf[512];
    size_t n = makeStatusJsonTo(buf, sizeof(buf));
    if (n > 0) {
      ws.sendTXT(clientId, (uint8_t*)buf, n);
    }
    break;
  }
  case WStype_DISCONNECTED:
    Serial.printf("[WS] Client %u disconnected\n", clientId);
    if (g_wsClientCount > 0) --g_wsClientCount;
    break;

  case WStype_TEXT: {
    // Parse incoming JSON
    JsonDocument doc;
    Serial.print("[WS RX] ");
    Serial.write(payload, length);
    Serial.println();
    DeserializationError err = deserializeJson(doc, (const char*)payload, length);
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

  // Initialize webserver / LittleFS (web UI files in data/ will be uploaded to device)
  initWebServer();

  initializeWiFi();

  server.on("/", HTTP_GET, handleRoot);

  server.onNotFound(handleNotFound);
  server.begin();

  ws.begin();
  ws.onEvent(wsEvent);
  // Enable heartbeat to drop dead clients and free resources automatically
  // ping every 15s, expect pong within 3s, drop after 2 missed pongs
  ws.enableHeartbeat(15000, 3000, 2);
  // Setup PWM output pin
  pinMode(pwmPin, OUTPUT);
  digitalWrite(pwmPin, LOW);
  Serial.println("HTTP :80, WS :81");

  // Initialize inverter RS232 communication (background task)
//  inverter_comm_init();

  // Start a background task to broadcast WS status so slow clients won't block main loop
  xTaskCreatePinnedToCore(
    [](void*) { // task lambda
      for (;;) {
        if (g_wsClientCount > 0) {
          uint32_t t0 = millis();
          // Broadcast using fixed buffer to avoid heap fragmentation
          // Buffer for JSON broadcast; increased to fit extended reset reason strings
          static char buf[768];
          size_t n = makeStatusJsonTo(buf, sizeof(buf));
          if (n > 0) ws.broadcastTXT((uint8_t*)buf, n);
          uint32_t t1 = millis();
          if (t1 - t0 > 500) Serial.printf("[WARN] wsBroadcastTask iteration took %ums\n", (unsigned)(t1 - t0));
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
      }
    },
    "ws_bcast",
    4096,
    NULL,
    1,
    &g_wsTask,
    1);
}

uint32_t lastMock = 0;
uint32_t lastPush = 0;

void loop() {
  uint32_t t0 = millis();
  server.handleClient();
  uint32_t t1 = millis();
  ws.loop();
  uint32_t t2 = millis();

  // Log if these calls take unusually long (indicates blocking)
  uint32_t hdlDur = t1 - t0;
  uint32_t wsDur = t2 - t1;
  if (hdlDur > 50) Serial.printf("[WARN] server.handleClient() took %ums\n", hdlDur);
  if (wsDur > 50) Serial.printf("[WARN] ws.loop() took %ums\n", wsDur);

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
    Serial.printf("[WARN] heap free=%u, largest=%u\n", (unsigned)freeHeap, (unsigned)largest);
    if (g_wsTask) {
      UBaseType_t watermark = uxTaskGetStackHighWaterMark(g_wsTask);
      Serial.printf("[WARN] ws_bcast stack high watermark=%u words\n", (unsigned)watermark);
    }
  }
}