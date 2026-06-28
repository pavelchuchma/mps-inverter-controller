#include "pylontech_comm.h"
#include "utils.h"
#include <HardwareSerial.h>
#include <ctype.h>

static SemaphoreHandle_t g_pylon_mutex = NULL;

PylontechState g_pylontech_status = {};
bool g_pylontech_data_valid = false;

// Send the 'pwr' command and collect the full console response into `resp`.
// Returns true ONLY if the end-of-response sentinel arrived, which guarantees a
// complete frame. We deliberately do NOT stop on an idle gap: the console can
// pause mid-frame, and an early stop would truncate the response — the parser
// would then publish zero-defaulted fields. Returning false on a truncated or
// missing response lets the caller keep the last good values instead.
static bool pylontech_collect_response(String& resp) {
  // Flush TX and discard any stale RX bytes before issuing the command.
  Serial2.flush();
  while (Serial2.available()) Serial2.read();

  Serial2.print(PYLONTECH_CMD);

  resp = "";
  unsigned long start = millis();

  while (millis() - start < PYLONTECH_READ_WINDOW_MS) {
    if (Serial2.available()) {
      int b = Serial2.read();
      if (b < 0) continue;
      resp += (char)b;
      // End-of-response sentinels: completion line or console prompt.
      if (resp.indexOf("Command completed successfully") >= 0 || resp.endsWith("$$")) {
        return true;
      }
    } else {
      vTaskDelay(pdMS_TO_TICKS(2));
    }
  }

  return false;  // window expired without the sentinel: incomplete response
}

// Trim leading/trailing ASCII whitespace from a String (in place copy).
static String trim_copy(const String& s) {
  String t = s;
  t.trim();
  return t;
}

// True only for a clean integer token: optional leading '-' then one or more
// ASCII digits. A serial glitch that drops digits leaves a non-numeric token
// (e.g. "mV"), which String::toInt() would silently turn into 0.
static bool is_int_token(const String& v) {
  if (v.length() == 0) return false;
  int i = 0;
  if (v[0] == '-') {
    if (v.length() == 1) return false;
    i = 1;
  }
  for (; i < (int)v.length(); ++i) {
    if (!isdigit((unsigned char)v[i])) return false;
  }
  return true;
}

// Parse the 'pwr' console response into `out`. The caller only passes complete
// frames (pylontech_collect_response returns true only on the end sentinel).
// Each core numeric field is validated: its token must be a clean integer and,
// where a zero/garbage value is physically impossible, within a plausible
// range. A single corrupted field rejects the whole frame so the caller keeps
// the last good values instead of publishing a bogus zero to InfluxDB.
static bool parse_pwr_payload(const String& resp, PylontechState& out) {
  PylontechState s = {};
  bool saw_voltage = false;
  bool saw_soc = false;

  // First corrupted field in this frame (for the rejection log), if any.
  bool frame_bad = false;
  String bad_label;
  String bad_value;
  auto reject = [&](const String& lbl, const String& val) {
    if (!frame_bad) {
      frame_bad = true;
      bad_label = lbl;
      bad_value = val;
    }
  };
  // Validate a clean integer in [lo, hi]; on failure mark the frame bad.
  auto num_ok = [&](const String& lbl, const String& val, long lo, long hi, long& outv) -> bool {
    if (!is_int_token(val)) { reject(lbl, val); return false; }
    long n = val.toInt();
    if (n < lo || n > hi) { reject(lbl, val); return false; }
    outv = n;
    return true;
  };
  // Validate a clean integer with no range constraint (signed/wide values).
  auto tok_ok = [&](const String& lbl, const String& val) -> bool {
    if (!is_int_token(val)) { reject(lbl, val); return false; }
    return true;
  };

  int pos = 0;
  int len = resp.length();
  while (pos < len) {
    int nl = resp.indexOf('\n', pos);
    if (nl < 0) nl = len;
    String line = resp.substring(pos, nl);
    line.replace("\r", "");
    pos = nl + 1;

    String t = trim_copy(line);
    if (t.length() == 0) continue;

    // The "Power N" pack header has no colon — special-case it.
    if (t.startsWith("Power") && t.indexOf(':') < 0) {
      String rest = trim_copy(t.substring(5));
      if (rest.length() && isdigit((unsigned char)rest.charAt(0))) {
        s.pack_index = rest.toInt();
      }
      continue;
    }

    int colon = t.indexOf(':');
    if (colon < 0) continue;
    String label = trim_copy(t.substring(0, colon));
    String rhs = trim_copy(t.substring(colon + 1));
    // value = first whitespace-separated token of the right-hand side
    int sp = rhs.indexOf(' ');
    String value = (sp < 0) ? rhs : rhs.substring(0, sp);

    long n;
    if (label == "CFetState") {
      s.cfet_on = (rhs == "ON");
    } else if (label == "DFetState") {
      s.dfet_on = (rhs == "ON");
    } else if (label == "Voltage") {
      if (num_ok(label, value, 40000, 60000, n)) {
        s.voltage = n / 1000.0f;
        saw_voltage = true;
      }
    } else if (label == "Current") {
      if (tok_ok(label, value)) s.current = value.toInt() / 1000.0f;
    } else if (label == "Temperature") {
      if (num_ok(label, value, -30000, 80000, n)) s.temperature = n / 1000.0f;
    } else if (label == "Coulomb") {
      if (num_ok(label, value, 0, 100, n)) {
        s.soc = n;
        saw_soc = true;
      }
    } else if (label == "Total Coulomb") {
      if (tok_ok(label, value)) s.total_capacity_mah = value.toInt();
    } else if (label == "Max Voltage") {
      if (num_ok(label, value, 45000, 60000, n)) s.max_voltage = n / 1000.0f;
    } else if (label == "Charge Times") {
      if (tok_ok(label, value)) s.charge_times = value.toInt();
    } else if (label == "Basic Status") {
      strncpy(s.basic_status, value.c_str(), sizeof(s.basic_status) - 1);
      s.basic_status[sizeof(s.basic_status) - 1] = '\0';
    } else if (label == "Heater Status") {
      s.heater_on = (value == "ON");
    } else if (label == "Bat Events") {
      s.bat_events = strtoul(value.c_str(), NULL, 16);
    } else if (label == "Power Events") {
      s.power_events = strtoul(value.c_str(), NULL, 16);
    } else if (label == "System Fault") {
      s.system_fault = strtoul(value.c_str(), NULL, 16);
    } else if (label == "System Alarm") {
      s.system_alarm = strtoul(value.c_str(), NULL, 16);
    }
    // Protect ENA and the "*. Status: Normal" lines are intentionally ignored.
  }

  // A corrupted core field: log the offending field plus the whole raw frame to
  // the persistent app log (for diagnostics) and reject. This only runs on a
  // sentinel-terminated frame, so a downed line (no sentinel, parser not called)
  // cannot flood the log.
  if (frame_bad) {
    char hdr[160];
    snprintf(hdr, sizeof(hdr),
             "[BAT] rejected frame: bad field \"%s\" value \"%s\"",
             bad_label.c_str(), bad_value.c_str());
    printWarningBlock(hdr, resp);
    return false;
  }

  if (!saw_voltage || !saw_soc) return false;  // keep last good values
  s.ts_ms = millis();
  out = s;
  return true;
}

// Print a battery status snapshot to Serial (thread-safe).
static void print_status_snapshot() {
  PylontechState s;
  bool valid;
  if (g_pylon_mutex) xSemaphoreTake(g_pylon_mutex, portMAX_DELAY);
  s = g_pylontech_status;
  valid = g_pylontech_data_valid;
  if (g_pylon_mutex) xSemaphoreGive(g_pylon_mutex);

  Serial.println("--- Battery Status Snapshot ---");
  if (!valid) {
    Serial.println("Read failed, no data available");
  } else {
    Serial.printf("Pack %d  Status: %s  CFet:%s DFet:%s Heater:%s\n",
                  s.pack_index, s.basic_status,
                  s.cfet_on ? "ON" : "OFF", s.dfet_on ? "ON" : "OFF",
                  s.heater_on ? "ON" : "OFF");
    Serial.printf("Voltage: %.2f V (max %.2f V), Current: %.2f A, Temp: %.2f C, SoC: %d %%\n",
                  s.voltage, s.max_voltage, s.current, s.temperature, s.soc);
    Serial.printf("Total capacity: %d mAH, Charge times: %d\n",
                  s.total_capacity_mah, s.charge_times);
    Serial.printf("Events bat:0x%X pwr:0x%X  Fault:0x%X  Alarm:0x%X\n",
                  s.bat_events, s.power_events, s.system_fault, s.system_alarm);
    Serial.printf("Timestamp: %u ms\n", (unsigned)s.ts_ms);
  }
  Serial.println("-------------------------------");
}

static void pylontech_task(void* arg) {
  (void)arg;
  uint8_t consec_fails = 0;
  for (;;) {
    String resp;
    bool ok = false;
    if (pylontech_collect_response(resp)) {
      PylontechState s;
      if (parse_pwr_payload(resp, s)) {
        if (g_pylon_mutex) xSemaphoreTake(g_pylon_mutex, portMAX_DELAY);
        g_pylontech_status = s;
        if (g_pylon_mutex) xSemaphoreGive(g_pylon_mutex);
        ok = true;
      } else {
        Serial.println("[BAT] failed to parse pwr response");
      }
    } else {
      Serial.println("[BAT] no response from battery console");
    }

    if (ok) {
      consec_fails = 0;
      if (g_pylon_mutex) xSemaphoreTake(g_pylon_mutex, portMAX_DELAY);
      g_pylontech_data_valid = true;
      if (g_pylon_mutex) xSemaphoreGive(g_pylon_mutex);
    } else {
      // Tolerate occasional dropouts: invalidate only after N consecutive fails.
      if (consec_fails < PYLONTECH_FAIL_INVALIDATE_THRESHOLD) consec_fails++;
      if (consec_fails >= PYLONTECH_FAIL_INVALIDATE_THRESHOLD) {
        if (g_pylon_mutex) xSemaphoreTake(g_pylon_mutex, portMAX_DELAY);
        g_pylontech_data_valid = false;
        if (g_pylon_mutex) xSemaphoreGive(g_pylon_mutex);
      }
    }

    print_status_snapshot();

    vTaskDelay(pdMS_TO_TICKS(PYLONTECH_POLL_INTERVAL_MS));
  }
}

void pylontech_comm_init(int rx_pin, int tx_pin) {
  if (!g_pylon_mutex) {
    g_pylon_mutex = xSemaphoreCreateMutex();
  }
  // Initialize Serial2 for the battery console port.
  Serial2.begin(PYLONTECH_BAUD, SERIAL_8N1, rx_pin, tx_pin);

  // Create background task
  xTaskCreatePinnedToCore(
    pylontech_task,
    "pylontech_task",
    4096,
    NULL,
    1,
    NULL,
    1);
}

bool pylontech_get_status(PylontechState* out) {
  if (!out) return false;
  if (g_pylon_mutex) xSemaphoreTake(g_pylon_mutex, portMAX_DELAY);
  *out = g_pylontech_status;
  if (g_pylon_mutex) xSemaphoreGive(g_pylon_mutex);
  return true;
}

bool pylontech_data_valid() {
  bool v;
  if (g_pylon_mutex) xSemaphoreTake(g_pylon_mutex, portMAX_DELAY);
  v = g_pylontech_data_valid;
  if (g_pylon_mutex) xSemaphoreGive(g_pylon_mutex);
  return v;
}
