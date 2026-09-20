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
#include "pylontech_can.h"
#include "pylontech_comm.h"
#include "phone.h"
#include "relay.h"
#include "soc_guard.h"
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
//
// `on_grid` is false for the off-grid event snapshots influx_log_event() takes
// from other tasks. Everything here is a snapshot of live state and is written
// either way; the one exception is the configuration block, which keeps
// bookkeeping of its own and must stay on a single thread. See there.
static void append_sample(String& buf, time_t ts, bool on_grid) {
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

  // chajda-battery — Pylontech pack status; only when valid, so offline periods
  // leave gaps in Grafana instead of zeros (same rationale as chajda-inverter).
  PylontechState bat = {};
  pylontech_get_status(&bat);
  if (g_pylontech_data_valid) {
    String line = "chajda-battery ";
    bool first = true;
    appendFloat(line, first, "voltage_v", bat.voltage);
    appendFloat(line, first, "current_a", bat.current);
    appendInt(line, first, "power_w", (long)(bat.voltage * bat.current));
    appendFloat(line, first, "temp_c", bat.temperature);
    appendInt(line, first, "soc", bat.soc);
    appendFloat(line, first, "max_voltage_v", bat.max_voltage);
    appendInt(line, first, "charge_times", bat.charge_times);
    appendBool(line, first, "cfet", bat.cfet_on);
    appendBool(line, first, "dfet", bat.dfet_on);
    appendBool(line, first, "heater", bat.heater_on);
    appendStr(line, first, "status", bat.basic_status);
    appendInt(line, first, "bat_events", (long)bat.bat_events);
    appendInt(line, first, "power_events", (long)bat.power_events);
    appendInt(line, first, "system_fault", (long)bat.system_fault);
    appendInt(line, first, "system_alarm", (long)bat.system_alarm);
    if (!first) {
      line += tsbuf;
      buf += line;
    }
  }

  // chajda-battery-can — the same pack over the CAN link, plus the fields only
  // CAN carries (CCL/DCL, SoH, the 0x35C request flags). Gated on
  // pylontech_can_valid() exactly as the block above is on
  // g_pylontech_data_valid, so an outage is a gap and not a run of zeros.
  //
  // The overlapping fields are deliberately stored twice: the two links are
  // independent, and the difference between them is the standing check that
  // both are still decoding correctly (doc/battery_can_data_spec.md).
  PylontechCanState can = {};
  pylontech_can_get(&can);
  if (pylontech_can_valid()) {
    String line = "chajda-battery-can ";
    bool first = true;
    appendFloat(line, first, "ccl_a", can.ccl_a);
    appendFloat(line, first, "dcl_a", can.dcl_a);
    appendFloat(line, first, "charge_v", can.charge_v);
    appendInt(line, first, "soc", can.soc);
    appendInt(line, first, "soh", can.soh);
    appendFloat(line, first, "voltage_v", can.voltage_v);
    appendFloat(line, first, "current_a", can.current_a);
    appendInt(line, first, "power_w", (long)(can.voltage_v * can.current_a));
    appendFloat(line, first, "temp_c", can.temp_c);
    // Range of current_a since the last point, which is what a point sample on
    // a 10 s (and later coarser) grid throws away — a boiler step or a short
    // discharge spike stays visible without storing every burst. Absent when no
    // burst landed in the window, so the fields are never a stale repeat.
    //
    // "Since the last point" is literal: an off-grid event snapshot takes the
    // range too, and the following grid point then covers only the time since
    // the event. That is the intended reading — each point's range covers the
    // gap back to the point before it, whether or not that one was on the grid.
    float cur_lo = 0.0f, cur_hi = 0.0f;
    if (pylontech_can_take_current_range(&cur_lo, &cur_hi)) {
      appendFloat(line, first, "current_min_a", cur_lo);
      appendFloat(line, first, "current_max_a", cur_hi);
    }
    appendInt(line, first, "protection", (long)can.protection);
    appendInt(line, first, "alarm", (long)can.alarm);
    appendInt(line, first, "modules", can.modules);
    appendBool(line, first, "chg_en", can_charge_enabled(can.request_flags));
    appendBool(line, first, "dchg_en", can_discharge_enabled(can.request_flags));
    appendBool(line, first, "force_chg_1", can_force_charge_1(can.request_flags));
    appendBool(line, first, "force_chg_2", can_force_charge_2(can.request_flags));
    appendBool(line, first, "full_chg_req", can_full_charge_requested(can.request_flags));
    appendInt(line, first, "req_raw", can.request_flags);  // undefined bits kept
    if (!first) {
      line += tsbuf;
      buf += line;
    }
  }

  // chajda-inverter-config — what the inverter is *allowed* to do, the
  // counterpart of the BMS limits in chajda-battery-can. Read every
  // INVERTER_CONFIG_INTERVAL_MS (5 min), so it gets its own measurement rather
  // than extra fields on chajda-inverter, where all but one point in thirty
  // would carry them.
  //
  // A point is written only when ts_ms has advanced, i.e. once per successful
  // QPIRI. That is the whole rule: a paused or dead link writes nothing and
  // leaves a gap for as long as the configuration went unconfirmed, instead of
  // restating values nobody read. A read returning unchanged values still
  // writes, which is what separates "unchanged" from "unknown" in Grafana.
  //
  // Grid path only: the static below is the one piece of state in this function,
  // and append_sample() is also called from other tasks through
  // influx_log_event(). Confining it to the influx task keeps it single-threaded
  // without a lock - and eventMutex could not be used for it anyway, since
  // influx_log_event() already holds that while calling in here. The cost is
  // that a config read is stored at the next grid tick rather than at an event
  // that happens to fall between, which is at most one sample interval.
  if (on_grid) {
    static uint32_t last_config_ts_ms = 0;
    InverterConfig cfg = {};
    if (inverter_get_config(&cfg) && cfg.ts_ms != last_config_ts_ms) {
      last_config_ts_ms = cfg.ts_ms;
      String line = "chajda-inverter-config ";
      bool first = true;
      appendFloat(line, first, "max_charge_a", cfg.max_charge_a);
      appendFloat(line, first, "bulk_v", cfg.bulk_v);
      appendFloat(line, first, "float_v", cfg.float_v);
      appendFloat(line, first, "lvd_v", cfg.lvd_v);
      appendFloat(line, first, "redischarge_v", cfg.redischarge_v);
      if (!first) {
        line += tsbuf;
        buf += line;
      }
    }
  }

  // chajda-boiler — relay state + boiler water temperatures (g_temp_h/g_temp_l).
  {
    String line = "chajda-boiler ";
    bool first = true;
    appendInt(line, first, "power_w", boilerPowerToWatts(getBoilerPower()));
    appendBool(line, first, "on", isBoilerOn());
    appendBool(line, first, "fault", isBoilerFault());
    appendBool(line, first, "manual", isBoilerManual());
    appendFloat(line, first, "temp_high", g_temp_h);
    appendFloat(line, first, "temp_low", g_temp_l);
    line += tsbuf;
    buf += line;
  }

  // chajda-soc-guard — switch, armed state and the cut-off the guard wants.
  // Its own measurement rather than fields on chajda-inverter-config: that one
  // is written only when a QPIRI read lands, so an arm/disarm would show up to
  // five minutes late and never in the event snapshots.
  {
    SocGuardState sg = {};
    soc_guard_get(&sg);
    String line = "chajda-soc-guard ";
    bool first = true;
    appendBool(line, first, "enabled", sg.enabled);
    appendBool(line, first, "armed", sg.enabled && sg.armed);
    appendFloat(line, first, "intended_lvd_v", sg.intended_lvd_v);
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

// Append the CAN link-health point, written once per flush and — unlike every
// measurement above — unconditionally. This series is what explains a gap in
// chajda-battery-can, so it is wanted precisely when there is no data to write.
// `elapsed_ms` is the time since the last one, for the rate.
static void append_can_link(String& buf, time_t ts, uint32_t elapsed_ms) {
  static uint32_t last_rx_frames = 0;
  static bool have_last_rx = false;

  PylontechCanLink lk = {};
  pylontech_can_get_link(&lk);

  // Derived from the frame counter and the real elapsed time rather than from
  // an assumed flush interval, so it stays correct if the interval changes.
  float rx_rate = 0.0f;
  if (have_last_rx && elapsed_ms > 0) {
    rx_rate = (lk.rx_frames - last_rx_frames) * 1000.0f / (float)elapsed_ms;
  }
  last_rx_frames = lk.rx_frames;
  have_last_rx = true;

  String line = "chajda-can-link ";
  bool first = true;
  appendInt(line, first, "rx_frames", (long)lk.rx_frames);
  appendFloat(line, first, "rx_rate", rx_rate);
  appendInt(line, first, "missed", (long)lk.rx_missed);
  appendInt(line, first, "bus_err", (long)lk.bus_errors);
  appendInt(line, first, "rec", lk.rx_err);
  appendInt(line, first, "tec", lk.tx_err);
  appendInt(line, first, "recoveries", (long)lk.recoveries);
  // Zero by construction in this build, which transmits nothing. Kept in the
  // series as the tripwire for 0x305 ever being re-enabled — it is the counter
  // that exposed the retry storm in the first place.
  appendInt(line, first, "tx_failed", (long)lk.tx_failed);
  appendInt(line, first, "state", lk.state);
  appendBool(line, first, "valid", pylontech_can_valid());
  char tsbuf[16];
  snprintf(tsbuf, sizeof(tsbuf), " %ld\n", (long)ts);
  line += tsbuf;
  buf += line;
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
  append_sample(pendingEvents, now, false);
  xSemaphoreGive(eventMutex);
}

static void influx_task(void* arg) {
  (void)arg;
  String buf;
  // ~1 kB per sample now that chajda-battery-can is in the batch, times
  // METRICS_SAMPLES_PER_FLUSH. Reserved up front so the batch does not grow by
  // reallocation on a heap this module shares with the TLS-free but still
  // allocation-heavy HTTP client.
  buf.reserve(8192);
  int count = 0;
  uint32_t last_link_ms = millis();
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
      append_sample(buf, now, true);
    }
    if (++count >= METRICS_SAMPLES_PER_FLUSH) {
      // One link-health point per flush, before the POST so it rides the same
      // request. Needs a wall-clock time like everything else, so a flush that
      // happens before NTP is set carries no link point either.
      if (now > 24 * 3600) {
        uint32_t elapsed = millis() - last_link_ms;
        last_link_ms = millis();
        append_can_link(buf, now, elapsed);
      }
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
