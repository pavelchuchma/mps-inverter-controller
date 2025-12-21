#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <WebSocketsServer.h>
#include <ArduinoJson.h>
#include <cmath>
#include "credentials.h"

IPAddress apIP(192, 168, 4, 1);
IPAddress netMsk(255, 255, 255, 0);

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
float outputDutyCycle = 0.0f; // 0.0 - 1.0 (represented as percent in UI)

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

  String out;
  serializeJson(doc, out);
  return out;
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
  wsBroadcastTxt(makeStatusJson());
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
    wsSend(clientId, makeStatusJson());
    break;
  }
  case WStype_DISCONNECTED:
    Serial.printf("[WS] Client %u disconnected\n", clientId);
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

  // Initialize webserver / LittleFS (web UI files in data/ will be uploaded to device)
  initWebServer();

  initializeWiFi();

  server.on("/", HTTP_GET, handleRoot);

  server.onNotFound(handleNotFound);
  server.begin();

  ws.begin();
  ws.onEvent(wsEvent);
  Serial.println("HTTP :80, WS :81");
}

uint32_t lastMock = 0;
uint32_t lastPush = 0;

void loop() {
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
