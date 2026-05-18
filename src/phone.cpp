#include "phone.h"

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

static SemaphoreHandle_t g_phone_mutex = NULL;

PhoneState g_phone_status = {};
bool       g_phone_data_valid = false;

const char* getPhoneIP() {
  static char ip_str[16] = {0};
  IPAddress gw = WiFi.gatewayIP();
  snprintf(ip_str, sizeof(ip_str), "%u.%u.%u.%u", gw[0], gw[1], gw[2], gw[3]);
  return ip_str;
}

static void copy_str(char* dst, size_t cap, const char* src) {
  if (!dst || cap == 0) return;
  if (!src) { dst[0] = '\0'; return; }
  strncpy(dst, src, cap - 1);
  dst[cap - 1] = '\0';
}

static void mark_invalid() {
  if (g_phone_mutex) xSemaphoreTake(g_phone_mutex, portMAX_DELAY);
  g_phone_data_valid = false;
  if (g_phone_mutex) xSemaphoreGive(g_phone_mutex);
}

// Quick TCP probe to the WiFi gateway on port 80. 1s timeout. Returns
// connect result and elapsed time. Fast fail (<100ms with ok=false) means
// gateway sent RST = WiFi link is healthy.
static void probe_gateway(bool* out_ok, uint32_t* out_ms) {
  IPAddress gw = WiFi.gatewayIP();
  if (gw == IPAddress(0, 0, 0, 0)) {
    *out_ok = false;
    *out_ms = 0;
    return;
  }
  WiFiClient c;
  uint32_t t0 = millis();
  bool ok = c.connect(gw, 80, 1000);
  *out_ms = millis() - t0;
  *out_ok = ok;
  c.stop();
}

static void log_link_state(const char* tag) {
  IPAddress gw = WiFi.gatewayIP();
  bool gw_ok = false;
  uint32_t gw_ms = 0;
  probe_gateway(&gw_ok, &gw_ms);
  Serial.printf("[PHONE] %s rssi=%d gw=%s gw_tcp=%s(%ums)\n",
                tag,
                (int)WiFi.RSSI(),
                gw.toString().c_str(),
                gw_ok ? "ok" : "fail",
                (unsigned)gw_ms);
}

static bool fetch_and_parse(const String& url) {
  HTTPClient http;
  http.setTimeout(PHONE_HTTP_TIMEOUT_MS);
  if (!http.begin(url)) {
    Serial.printf("[PHONE] http.begin() failed for %s\n", url.c_str());
    return false;
  }

  uint32_t t_get_start = millis();
  int code = http.GET();
  uint32_t t_get_ms = millis() - t_get_start;
  if (code != HTTP_CODE_OK) {
    Serial.printf("[PHONE] HTTP %d after %ums for %s\n", code, (unsigned)t_get_ms, url.c_str());
    http.end();
    return false;
  }

  uint32_t t_body_start = millis();
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, http.getStream());
  uint32_t t_body_ms = millis() - t_body_start;
  http.end();
  if (err) {
    Serial.printf("[PHONE] JSON parse error: %s (get=%ums body=%ums)\n",
                  err.c_str(), (unsigned)t_get_ms, (unsigned)t_body_ms);
    return false;
  }

  PhoneState s = {};

  JsonObjectConst bat = doc["battery"].as<JsonObjectConst>();
  s.battery_present              = bat["present"]            | false;
  copy_str(s.battery_technology, sizeof(s.battery_technology), bat["technology"] | "");
  copy_str(s.battery_health,     sizeof(s.battery_health),     bat["health"]     | "");
  copy_str(s.battery_plugged,    sizeof(s.battery_plugged),    bat["plugged"]    | "");
  copy_str(s.battery_status,     sizeof(s.battery_status),     bat["status"]     | "");
  s.battery_temperature_c        = bat["temperature"]       | NAN;
  s.battery_voltage_mv           = bat["voltage"]           | 0;
  s.battery_current_ua           = bat["current"]           | 0;
  s.battery_current_average_ua   = bat["current_average"]   | 0;
  s.battery_percentage           = bat["percentage"]        | 0;
  s.battery_level                = bat["level"]             | 0;
  s.battery_scale                = bat["scale"]             | 0;
  s.battery_charge_counter       = bat["charge_counter"]    | 0;
  s.battery_stale_secs           = bat["stale_secs"]        | NAN;

  JsonObjectConst net = doc["network"].as<JsonObjectConst>();
  copy_str(s.network_since, sizeof(s.network_since), net["since"] | "");
  JsonObjectConst ifs = net["interfaces"].as<JsonObjectConst>();
  s.net_wlan0_rx_bytes  = ifs["wlan0"]["rx_bytes"]  | (uint64_t)0;
  s.net_wlan0_tx_bytes  = ifs["wlan0"]["tx_bytes"]  | (uint64_t)0;
  s.net_rmnet0_rx_bytes = ifs["rmnet0"]["rx_bytes"] | (uint64_t)0;
  s.net_rmnet0_tx_bytes = ifs["rmnet0"]["tx_bytes"] | (uint64_t)0;
  s.network_stale_secs  = net["stale_secs"]         | NAN;

  s.ts_ms = millis();

  if (g_phone_mutex) xSemaphoreTake(g_phone_mutex, portMAX_DELAY);
  g_phone_status = s;
  g_phone_data_valid = true;
  if (g_phone_mutex) xSemaphoreGive(g_phone_mutex);

  Serial.printf("[PHONE] %d%% %s %s, %.1f C, %d mV, cur=%d uA (avg %d uA) (get=%ums body=%ums)\n",
                s.battery_percentage,
                s.battery_status,
                s.battery_plugged,
                s.battery_temperature_c,
                s.battery_voltage_mv,
                s.battery_current_ua,
                s.battery_current_average_ua,
                (unsigned)t_get_ms,
                (unsigned)t_body_ms);
  return true;
}

static void phone_task(void* arg) {
  (void)arg;
  for (;;) {
    if (!WiFi.isConnected()) {
      Serial.println("[PHONE] WiFi not connected, skipping poll");
      mark_invalid();
    } else {
      log_link_state("link before poll:");
      String url = String("http://") + getPhoneIP() + ":" + PHONE_STATUS_PORT + PHONE_STATUS_PATH;
      bool ok = false;
      for (int attempt = 1; attempt <= PHONE_HTTP_MAX_ATTEMPTS; ++attempt) {
        if (fetch_and_parse(url)) {
          if (attempt > 1) {
            Serial.printf("[PHONE] succeeded on attempt %d\n", attempt);
          }
          ok = true;
          break;
        }
        if (attempt < PHONE_HTTP_MAX_ATTEMPTS) {
          vTaskDelay(pdMS_TO_TICKS(PHONE_HTTP_RETRY_DELAY_MS));
        }
      }
      if (!ok) {
        mark_invalid();
        log_link_state("link after fail: ");
      }
    }
    vTaskDelay(pdMS_TO_TICKS(PHONE_POLL_INTERVAL_MS));
  }
}

void phone_comm_init() {
  if (!g_phone_mutex) {
    g_phone_mutex = xSemaphoreCreateMutex();
  }
  xTaskCreatePinnedToCore(
    phone_task,
    "phone_task",
    6144,
    NULL,
    1,
    NULL,
    1);
}

bool phone_get_status(PhoneState* out) {
  if (!out) return false;
  if (g_phone_mutex) xSemaphoreTake(g_phone_mutex, portMAX_DELAY);
  *out = g_phone_status;
  bool valid = g_phone_data_valid;
  if (g_phone_mutex) xSemaphoreGive(g_phone_mutex);
  return valid;
}
