#include "influx.h"

#include <WiFi.h>
#include <HTTPClient.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <time.h>
#include <math.h>

#include "config.h"
#include "credentials.h"
#include "inverter_comm.h"
#include "phone.h"
#include "relay.h"
#include "utils.h"

// --- line-protocol field appenders ---
// Each writes "key=value" prefixed with a comma unless it is the first field on
// the line; `first` is cleared on the first successful append.
static void appendFloat(String& line, bool& first, const char* key, float v) {
  if (isnan(v)) return;
  char b[32];
  snprintf(b, sizeof(b), "%s%s=%.2f", first ? "" : ",", key, v);
  line += b;
  first = false;
}

static void appendInt(String& line, bool& first, const char* key, long v) {
  char b[32];
  snprintf(b, sizeof(b), "%s%s=%ldi", first ? "" : ",", key, v);
  line += b;
  first = false;
}

static void appendU64(String& line, bool& first, const char* key, uint64_t v) {
  char b[40];
  snprintf(b, sizeof(b), "%s%s=%llui", first ? "" : ",", key, (unsigned long long)v);
  line += b;
  first = false;
}

static void appendBool(String& line, bool& first, const char* key, bool v) {
  char b[24];
  snprintf(b, sizeof(b), "%s%s=%s", first ? "" : ",", key, v ? "true" : "false");
  line += b;
  first = false;
}

// String field value, wrapped in double quotes. Source values here are short
// enums (mode code, battery status) without quotes/backslashes, so no escaping.
static void appendStr(String& line, bool& first, const char* key, const char* v) {
  char b[48];
  snprintf(b, sizeof(b), "%s%s=\"%s\"", first ? "" : ",", key, v);
  line += b;
  first = false;
}

// Buffer for off-cadence event snapshots produced by other tasks (e.g. a boiler
// power change). influx_task drains it into its batch buffer under the mutex.
static String pendingEvents;
static SemaphoreHandle_t eventMutex = nullptr;

// Append one timestamped sample (all measurements) to the batch buffer.
static void append_sample(String& buf, time_t ts) {
  char tsbuf[16];
  snprintf(tsbuf, sizeof(tsbuf), " %ld\n", (long)ts);

  // chajda-inverter — only when inverter data is currently valid, so offline
  // periods leave gaps in Grafana instead of zeros.
  InverterState s = {};
  inverter_get_status(&s);
  if (g_inverter_data_valid) {
    String line = "chajda-inverter ";
    bool first = true;
    appendFloat(line, first, "ac_v", s.ac_out_voltage);
    appendInt(line, first, "ac_va", s.ac_apparent_va);
    appendInt(line, first, "ac_w", s.ac_active_w);
    appendInt(line, first, "load_pct", s.load_percent);
    appendFloat(line, first, "batt_v", s.batt_voltage);
    appendFloat(line, first, "batt_chg_a", s.batt_charge_current);
    appendInt(line, first, "soc", s.batt_soc);
    appendFloat(line, first, "heatsink_c", s.heatsink_temp);
    appendFloat(line, first, "pv_a", s.pv_input_current_batt);
    appendFloat(line, first, "pv_v", s.pv_input_voltage);
    appendFloat(line, first, "batt_v_scc", s.batt_voltage_from_scc);
    appendFloat(line, first, "batt_dischg_a", s.batt_discharge_current);
    appendInt(line, first, "pv_w", s.pv_charging_power);
    char mode_code = '\0';
    char mode_name[32] = "";
    if (inverter_get_mode(&mode_code, mode_name, sizeof(mode_name)) && mode_code) {
      char ms[2] = {mode_code, '\0'};
      appendStr(line, first, "mode", ms);
    }
    if (!first) {
      line += tsbuf;
      buf += line;
    }
  }

  // chajda-boiler — relay state + boiler water temperatures (g_temp_h/g_temp_l).
  {
    String line = "chajda-boiler ";
    bool first = true;
    appendInt(line, first, "power_w", boilerPowerToWatts(getBoilerPower()));
    appendBool(line, first, "on", isBoilerOn());
    appendBool(line, first, "fault", isBoilerFault());
    appendFloat(line, first, "temp_high", g_temp_h);
    appendFloat(line, first, "temp_low", g_temp_l);
    line += tsbuf;
    buf += line;
  }

  // chajda-phone — charger relay (always) + phone battery/traffic (when valid).
  {
    String line = "chajda-phone ";
    bool first = true;
    appendBool(line, first, "charger_on", isMobileChargerOn());
    PhoneState ph = {};
    if (phone_get_status(&ph)) {
      appendInt(line, first, "batt_pct", ph.battery_percentage);
      appendInt(line, first, "batt_ma", ph.battery_current_ua / 1000);
      appendU64(line, first, "rx", ph.net_rmnet0_rx_bytes);
      appendU64(line, first, "tx", ph.net_rmnet0_tx_bytes);
      appendStr(line, first, "batt_status", ph.battery_status);
    }
    line += tsbuf;
    buf += line;
  }
}

// POST the accumulated batch in a single request. Drops the batch on any
// failure — this is not time-critical and we never buffer to flash.
static void flush(const String& buf) {
  if (buf.length() == 0) return;
  if (!WiFi.isConnected()) {
    printWarning("[INFLUX] WiFi down, dropping %u bytes", (unsigned)buf.length());
    return;
  }

  String url = String("http://") + INFLUX_HOST + ":" + INFLUX_PORT +
               "/api/v2/write?org=" + INFLUX_ORG + "&bucket=" + INFLUX_BUCKET +
               "&precision=s";

  HTTPClient http;
  http.setTimeout(METRICS_HTTP_TIMEOUT_MS);
  if (!http.begin(url)) {
    printWarning("[INFLUX] http.begin() failed");
    return;
  }
  http.addHeader("Authorization", "Token " INFLUX_TOKEN);
  http.addHeader("Content-Type", "text/plain; charset=utf-8");

  uint32_t t0 = millis();
  int code = http.POST((uint8_t*)buf.c_str(), buf.length());
  uint32_t dt = millis() - t0;

  if (code == 204) {
    Serial.printf("[INFLUX] flushed %u bytes, HTTP 204 in %ums\n",
                  (unsigned)buf.length(), (unsigned)dt);
  } else {
    String resp = http.getString();
    printWarning("[INFLUX] POST HTTP %d in %ums, dropped %u bytes: %s",
                 code, (unsigned)dt, (unsigned)buf.length(), resp.c_str());
  }
  http.end();
}

// Capture a full snapshot now and queue it for the next flush. Safe to call from
// any task; the snapshot reflects live state at call time, so it records the
// state right before a boiler power change. The reason for the change is not
// stored here — look it up in the app log if needed.
void influx_log_event() {
  if (!eventMutex) return;  // called before influx_init
  time_t now = time(nullptr);
  if (now <= 24 * 3600) return;  // NTP not set yet, timestamp would be bogus
  xSemaphoreTake(eventMutex, portMAX_DELAY);
  append_sample(pendingEvents, now);
  xSemaphoreGive(eventMutex);
}

static void influx_task(void* arg) {
  (void)arg;
  String buf;
  buf.reserve(4096);
  int count = 0;
  for (;;) {
    // Drain any off-cadence events captured by other tasks and flush them
    // promptly (within one sample interval) rather than waiting for the full
    // minute batch, so a decision shows up quickly in Grafana.
    xSemaphoreTake(eventMutex, portMAX_DELAY);
    if (pendingEvents.length() > 0) {
      buf += pendingEvents;
      pendingEvents = "";
      count = METRICS_SAMPLES_PER_FLUSH;  // force flush this iteration
    }
    xSemaphoreGive(eventMutex);

    time_t now = time(nullptr);
    // Only sample once WiFi is up and NTP has set a real wall-clock time
    // (otherwise the per-sample timestamp would be bogus). Skipped samples
    // still advance the flush counter so the cadence stays one POST/minute.
    if (WiFi.isConnected() && now > 24 * 3600) {
      append_sample(buf, now);
    }
    if (++count >= METRICS_SAMPLES_PER_FLUSH) {
      flush(buf);
      buf = "";
      count = 0;
    }
    vTaskDelay(pdMS_TO_TICKS(METRICS_SAMPLE_INTERVAL_MS));
  }
}

void influx_init() {
  eventMutex = xSemaphoreCreateMutex();
  xTaskCreatePinnedToCore(
    influx_task,
    "influx_task",
    6144,
    NULL,
    1,
    NULL,
    1);
}
