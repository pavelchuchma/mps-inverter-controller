#include "pylontech_can.h"
#include "utils.h"
#include <driver/twai.h>
#include <string.h>

// Frame identifiers broadcast by the BMS, see doc/battery_can_spec.md.
#define CAN_ID_LIMITS 0x351   // charge voltage, CCL, DCL
#define CAN_ID_SOC 0x355      // SoC, SoH
#define CAN_ID_MEASURED 0x356 // voltage, current, temperature
#define CAN_ID_ALARMS 0x359   // protection / alarm bitmaps, module count
#define CAN_ID_REQUESTS 0x35C // charge / discharge enable request
#define CAN_ID_VENDOR 0x35E   // manufacturer string "PYLON"

// Inverter reply, 8 zero bytes at 1 Hz. The vendor spec states it plainly, so
// it goes out unconditionally from task start.
#define CAN_ID_HEARTBEAT 0x305

// Bus health line interval.
#define CAN_STATUS_INTERVAL_MS 30000

// How often 0x305 is retried while the bus is silent, in case the reply is
// what wakes the pack. See the transmit site for why this is not 1 Hz.
#define CAN_WAKE_PROBE_INTERVAL_MS 30000

// --- transmit disabled, deliberately ---------------------------------------
// The bring-up sniffer transmitted nothing while frames were arriving, and the
// link worked. Sending 0x305 unconditionally (step 1.2.5 of the data spec) is
// the one thing that changed, and the pack has been silent since — and before
// single-shot landed, every unacknowledged reply auto-retried into bus-off,
// spraying error flags across the bus for some forty minutes, which is enough
// to push the pack's own error counters into error-passive or bus-off.
//
// So the controller now transmits nothing at all: the proven-working
// configuration, and the clean test of whether we caused the silence. This
// does NOT make it listen-only — TWAI_MODE_NORMAL still acknowledges every
// received frame, which is mandatory and is what keeps the pack transmitting.
// Re-enable only once the pack is healthy again, and then only into silence.
#define CAN_SEND_HEARTBEAT 0

// Bus-off recovery back-off. With no peer on the wire every frame we send goes
// unacknowledged, the error counter runs away and the controller drops back to
// bus-off within a second of each recovery. Retrying at that rate is a busy
// loop, so the interval doubles up to the maximum and is reset by the first
// received frame — the only thing that proves there is someone to talk to.
#define CAN_RECOVER_MIN_INTERVAL_MS 1000
#define CAN_RECOVER_MAX_INTERVAL_MS 60000
// app.log gets at most one bus-off line this often. The recovery path is
// per-second on a dead bus, and app.log is on flash.
#define CAN_BUSOFF_LOG_INTERVAL_MS 600000

// Frame groups touched by the current burst, for the range check below.
#define CAN_GROUP_LIMITS 0x01
#define CAN_GROUP_SOC 0x02
#define CAN_GROUP_MEASURED 0x04
#define CAN_GROUP_ALARMS 0x08
#define CAN_GROUP_REQUESTS 0x10

// --- bring-up scaffold, removed in commit 2 -------------------------------
// CAN's CRC already rejects corrupted frames, so this is not noise filtering:
// its only job is to catch a wrong layout assumption (byte order, scaling)
// before a mis-scaled ccl_a can reach anything. Once /can has confirmed the
// layout the encoding is fixed for the life of the pack, and a hardcoded
// window can then only misfire — see doc/battery_can_data_spec.md.
#define CAN_BRINGUP_RANGE_CHECK 1
#if CAN_BRINGUP_RANGE_CHECK
#define CAN_RANGE_CHARGE_V_MIN 40.0f
#define CAN_RANGE_CHARGE_V_MAX 60.0f
#define CAN_RANGE_LIMIT_A_MIN 0.0f
#define CAN_RANGE_LIMIT_A_MAX 500.0f
#define CAN_RANGE_VOLTAGE_V_MIN 30.0f
#define CAN_RANGE_VOLTAGE_V_MAX 60.0f
#define CAN_RANGE_CURRENT_A_MIN -500.0f
#define CAN_RANGE_CURRENT_A_MAX 500.0f
#define CAN_RANGE_TEMP_C_MIN -30.0f
#define CAN_RANGE_TEMP_C_MAX 80.0f
#endif
// -------------------------------------------------------------------------

static int g_tx_pin = -1;
static int g_rx_pin = -1;

static SemaphoreHandle_t g_can_mutex = NULL;
static PylontechCanState g_can_state = {};
static PylontechCanLink g_can_link = {};
static PylontechCanRaw g_can_raw[CAN_RAW_SLOTS] = {};

// Little-endian field accessors. The BMS uses 16-bit values throughout; which
// of them are signed is documented in battery_can_spec.md.
static uint16_t u16le(const uint8_t* d) {
  return (uint16_t)(d[0] | (d[1] << 8));
}
static int16_t i16le(const uint8_t* d) {
  return (int16_t)(d[0] | (d[1] << 8));
}

// Same freshness rule as pylontech_can_valid(), on a caller-held snapshot.
static bool state_is_fresh(const PylontechCanState& s, uint32_t now) {
  if (s.limits_ts_ms == 0 || s.soc_ts_ms == 0 || s.measured_ts_ms == 0) return false;
  return (now - s.limits_ts_ms) < CAN_STALE_MS
      && (now - s.soc_ts_ms) < CAN_STALE_MS
      && (now - s.measured_ts_ms) < CAN_STALE_MS;
}

// Record the raw payload for /can. Kept outside the range check so a rejected
// burst is still visible byte for byte, which is the whole point of the check.
static void record_raw(const twai_message_t& m, uint32_t now) {
  int free_slot = -1;
  for (int i = 0; i < CAN_RAW_SLOTS; ++i) {
    if (g_can_raw[i].id == m.identifier) {
      g_can_raw[i].dlc = m.data_length_code;
      memcpy(g_can_raw[i].data, m.data, 8);
      g_can_raw[i].ts_ms = now;
      g_can_raw[i].count++;
      return;
    }
    if (free_slot < 0 && g_can_raw[i].id == 0) free_slot = i;
  }
  if (free_slot < 0) return;  // more identifiers than expected: keep the first six
  g_can_raw[free_slot].id = m.identifier;
  g_can_raw[free_slot].dlc = m.data_length_code;
  memcpy(g_can_raw[free_slot].data, m.data, 8);
  g_can_raw[free_slot].ts_ms = now;
  g_can_raw[free_slot].count = 1;
}

// Decode one frame into the burst scratch. Scaling and byte order are the
// documented Pylontech layout (doc/battery_can_spec.md). Returns the group
// mask bit for the frame, or 0 if the identifier carries nothing decodable
// (0x35E is ASCII "PYLON" and is only kept as a raw payload).
static uint8_t decode_frame(const twai_message_t& m, PylontechCanState* s, uint32_t now) {
  const uint8_t* d = m.data;
  switch (m.identifier) {
    case CAN_ID_LIMITS:
      if (m.data_length_code < 6) break;
      s->charge_v = u16le(d + 0) / 10.0f;
      s->ccl_a = i16le(d + 2) / 10.0f;
      s->dcl_a = i16le(d + 4) / 10.0f;
      s->limits_ts_ms = now;
      return CAN_GROUP_LIMITS;
    case CAN_ID_SOC:
      if (m.data_length_code < 4) break;
      s->soc = u16le(d + 0);
      s->soh = u16le(d + 2);
      s->soc_ts_ms = now;
      return CAN_GROUP_SOC;
    case CAN_ID_MEASURED:
      if (m.data_length_code < 6) break;
      s->voltage_v = i16le(d + 0) / 100.0f;
      s->current_a = i16le(d + 2) / 10.0f;
      s->temp_c = i16le(d + 4) / 10.0f;
      s->measured_ts_ms = now;
      return CAN_GROUP_MEASURED;
    case CAN_ID_ALARMS:
      if (m.data_length_code < 5) break;
      s->protection = (uint32_t)d[0] | ((uint32_t)d[1] << 8);
      s->alarm = (uint32_t)d[2] | ((uint32_t)d[3] << 8);
      s->modules = d[4];
      s->alarms_ts_ms = now;
      return CAN_GROUP_ALARMS;
    case CAN_ID_REQUESTS:
      if (m.data_length_code < 1) break;
      s->request_flags = d[0];
      s->requests_ts_ms = now;
      return CAN_GROUP_REQUESTS;
    default:
      break;
  }
  return 0;
}

#if CAN_BRINGUP_RANGE_CHECK
static bool in_range(float v, float lo, float hi) {
  return v >= lo && v <= hi;
}

// Check only the groups this burst actually carried: a group that has not been
// seen yet still holds its zero default, which would fail every window.
static bool burst_in_range(const PylontechCanState& s, uint8_t groups) {
  if (groups & CAN_GROUP_LIMITS) {
    if (!in_range(s.charge_v, CAN_RANGE_CHARGE_V_MIN, CAN_RANGE_CHARGE_V_MAX)) return false;
    if (!in_range(s.ccl_a, CAN_RANGE_LIMIT_A_MIN, CAN_RANGE_LIMIT_A_MAX)) return false;
    if (!in_range(s.dcl_a, CAN_RANGE_LIMIT_A_MIN, CAN_RANGE_LIMIT_A_MAX)) return false;
  }
  if (groups & CAN_GROUP_SOC) {
    if (s.soc < 0 || s.soc > 100) return false;
    if (s.soh < 0 || s.soh > 100) return false;
  }
  if (groups & CAN_GROUP_MEASURED) {
    if (!in_range(s.voltage_v, CAN_RANGE_VOLTAGE_V_MIN, CAN_RANGE_VOLTAGE_V_MAX)) return false;
    if (!in_range(s.current_a, CAN_RANGE_CURRENT_A_MIN, CAN_RANGE_CURRENT_A_MAX)) return false;
    if (!in_range(s.temp_c, CAN_RANGE_TEMP_C_MIN, CAN_RANGE_TEMP_C_MAX)) return false;
  }
  return true;
}
#endif

static void print_bus_status(const char* prefix) {
  twai_status_info_t st;
  if (twai_get_status_info(&st) != ESP_OK) return;
  Serial.printf("[CAN] %s state=%d tx_err=%u rx_err=%u bus_err=%u tx_failed=%u missed=%u\n",
                prefix, (int)st.state, (unsigned)st.tx_error_counter,
                (unsigned)st.rx_error_counter, (unsigned)st.bus_error_count,
                (unsigned)st.tx_failed_count, (unsigned)st.rx_missed_count);
}

static bool driver_start() {
  twai_general_config_t g = TWAI_GENERAL_CONFIG_DEFAULT(
      (gpio_num_t)g_tx_pin, (gpio_num_t)g_rx_pin, TWAI_MODE_NORMAL);
  g.rx_queue_len = 32;
  twai_timing_config_t t = TWAI_TIMING_CONFIG_500KBITS();
  twai_filter_config_t f = TWAI_FILTER_CONFIG_ACCEPT_ALL();

  esp_err_t err = twai_driver_install(&g, &t, &f);
  if (err != ESP_OK) {
    Serial.printf("[CAN] twai_driver_install failed: %s\n", esp_err_to_name(err));
    return false;
  }
  err = twai_start();
  if (err != ESP_OK) {
    Serial.printf("[CAN] twai_start failed: %s\n", esp_err_to_name(err));
    twai_driver_uninstall();
    return false;
  }
  return true;
}

// Bus-off recovery is not automatic, so it has to be driven from here —
// otherwise powering the ESP32 up before the battery would leave the link dead
// until a reboot. Serial only: on a bus with no peer this runs every recovery
// interval, and app.log is on flash. The caller owns the back-off and the
// throttled app.log line.
static bool recover_bus() {
  twai_status_info_t st;
  Serial.println("[CAN] bus-off, recovering");
  if (twai_initiate_recovery() != ESP_OK) return false;
  // Recovery completes after 128 idle bus periods; poll for the stopped state.
  for (int i = 0; i < 50; ++i) {
    vTaskDelay(pdMS_TO_TICKS(100));
    if (twai_get_status_info(&st) == ESP_OK && st.state == TWAI_STATE_STOPPED) {
      if (twai_start() == ESP_OK) {
        Serial.println("[CAN] recovered from bus-off");
        return true;
      }
      return false;
    }
  }
  Serial.println("[CAN] bus-off recovery did not complete");
  return false;
}

static void pylontech_can_task(void* arg) {
  (void)arg;

  if (!driver_start()) {
    vTaskDelete(NULL);
    return;
  }
  Serial.println("[CAN] listening at 500 kbit/s (normal mode, acknowledging)");

  // Task-local working copies. `scratch` persists across bursts so a frame
  // group missing from one burst keeps its previous value and its own
  // timestamp; `burst` is the copy the current burst writes into, so a burst
  // that fails the range check can be dropped whole.
  PylontechCanState scratch = {};
  PylontechCanState burst = {};
  PylontechCanLink link = {};
  uint8_t burst_groups = 0;
  bool burst_active = false;
  unsigned long burst_start_ms = 0;
  unsigned long last_frame_ms = 0;

#if CAN_SEND_HEARTBEAT
  unsigned long last_heartbeat = 0;
#endif
  unsigned long last_status = millis();

  // Transition tracking for the Serial/app.log lines. Per-frame printing is
  // gone: GET /can carries the detail on demand.
  bool was_valid = false;
  bool have_prev_alarms = false;
  bool have_prev_requests = false;
  uint32_t prev_protection = 0;
  uint32_t prev_alarm = 0;
  uint8_t prev_request_flags = 0;

  // Enqueue failures, kept apart from the driver's own count of transmissions
  // that failed on the wire; link.tx_failed is the sum of the two.
  uint32_t tx_enqueue_failed = 0;

  unsigned long recover_interval_ms = CAN_RECOVER_MIN_INTERVAL_MS;
  unsigned long last_recover_ms = 0;
  unsigned long last_busoff_log_ms = 0;

  for (;;) {
    twai_message_t m;
    if (twai_receive(&m, pdMS_TO_TICKS(20)) == ESP_OK) {
      unsigned long now = millis();
      link.rx_frames++;
      link.last_rx_ms = now;
      last_frame_ms = now;
      // A frame proves there is a peer, so recovery can be prompt again.
      recover_interval_ms = CAN_RECOVER_MIN_INTERVAL_MS;
      if (!burst_active) {
        burst_active = true;
        burst_start_ms = now;
        burst = scratch;
        burst_groups = 0;
      }
      burst_groups |= decode_frame(m, &burst, now);
      if (g_can_mutex) xSemaphoreTake(g_can_mutex, portMAX_DELAY);
      record_raw(m, now);
      if (g_can_mutex) xSemaphoreGive(g_can_mutex);
    }

    // End of burst: no frame for CAN_BURST_GAP_MS, or CAN_BURST_MAX_MS since
    // the burst started. Committing per burst rather than per frame means a
    // reader never sees ccl_a from burst N next to current_a from burst N-1.
    if (burst_active
        && (millis() - last_frame_ms >= CAN_BURST_GAP_MS
            || millis() - burst_start_ms >= CAN_BURST_MAX_MS)) {
      burst_active = false;
      bool accept = true;
#if CAN_BRINGUP_RANGE_CHECK
      accept = burst_in_range(burst, burst_groups);
      if (!accept) link.rejected++;
#endif
      if (accept) {
        scratch = burst;
        if (g_can_mutex) xSemaphoreTake(g_can_mutex, portMAX_DELAY);
        g_can_state = scratch;
        if (g_can_mutex) xSemaphoreGive(g_can_mutex);

        if (burst_groups & CAN_GROUP_ALARMS) {
          if (have_prev_alarms
              && (scratch.protection != prev_protection || scratch.alarm != prev_alarm)) {
            printWarning("[CAN] protection 0x%04X -> 0x%04X, alarm 0x%04X -> 0x%04X",
                         (unsigned)prev_protection, (unsigned)scratch.protection,
                         (unsigned)prev_alarm, (unsigned)scratch.alarm);
          }
          prev_protection = scratch.protection;
          prev_alarm = scratch.alarm;
          have_prev_alarms = true;
        }
        if (burst_groups & CAN_GROUP_REQUESTS) {
          if (have_prev_requests && scratch.request_flags != prev_request_flags) {
            printInfo("[CAN] request flags 0x%02X -> 0x%02X (chg %d dchg %d force %d/%d full %d)",
                      prev_request_flags, scratch.request_flags,
                      can_charge_enabled(scratch.request_flags),
                      can_discharge_enabled(scratch.request_flags),
                      can_force_charge_1(scratch.request_flags),
                      can_force_charge_2(scratch.request_flags),
                      can_full_charge_requested(scratch.request_flags));
          }
          prev_request_flags = scratch.request_flags;
          have_prev_requests = true;
        }
      }
    }

    // Link up/down transition, evaluated off the same freshness rule the
    // readers use so /can and app.log cannot disagree.
    bool is_valid = state_is_fresh(scratch, millis());
    if (is_valid != was_valid) {
      was_valid = is_valid;
      if (is_valid) {
        printInfo("[CAN] link up: %.1f V %.1f A  SoC %d %%  CCL %.1f A  DCL %.1f A",
                  scratch.voltage_v, scratch.current_a, scratch.soc,
                  scratch.ccl_a, scratch.dcl_a);
      } else {
        printWarning("[CAN] link down: no complete burst for %lu ms",
                     (unsigned long)CAN_STALE_MS);
      }
    }

    twai_status_info_t st;
    bool have_status = (twai_get_status_info(&st) == ESP_OK);
    bool bus_off = have_status && st.state == TWAI_STATE_BUS_OFF;

#if CAN_SEND_HEARTBEAT
    // Is anyone on the wire? Any frame will do — this asks whether there is
    // someone to acknowledge, not whether the data set is complete.
    bool peer_present = link.last_rx_ms != 0
                     && millis() - link.last_rx_ms < CAN_STALE_MS;

    // The vendor spec asks the inverter to reply every second, and while the
    // pack is broadcasting that is free: it is there and it acknowledges. Into
    // a silent bus the same frame is destructive — the controller retries an
    // unacknowledged transmission at bus speed, so one queued frame walks the
    // error counter to bus-off within milliseconds. Hence: full rate while the
    // pack talks, a slow probe while it does not (in case 0x305 is what wakes
    // it), and single-shot either way so a missing acknowledge costs one
    // increment instead of a retry storm.
    unsigned long tx_interval = peer_present ? CAN_HEARTBEAT_INTERVAL_MS
                                             : CAN_WAKE_PROBE_INTERVAL_MS;
    if (have_status && st.state == TWAI_STATE_RUNNING
        && millis() - last_heartbeat >= tx_interval) {
      last_heartbeat = millis();
      twai_message_t hb;
      memset(&hb, 0, sizeof(hb));
      hb.identifier = CAN_ID_HEARTBEAT;
      hb.data_length_code = 8;
      hb.ss = 1;  // single shot: the controller must not retry this
      if (twai_transmit(&hb, pdMS_TO_TICKS(100)) != ESP_OK) tx_enqueue_failed++;
    }
#endif

    if (bus_off && millis() - last_recover_ms >= recover_interval_ms) {
      last_recover_ms = millis();
      if (last_busoff_log_ms == 0
          || millis() - last_busoff_log_ms >= CAN_BUSOFF_LOG_INTERVAL_MS) {
        last_busoff_log_ms = millis();
        printWarning("[CAN] bus-off (%u recoveries, %u frames received, retry every %lu s)",
                     (unsigned)link.recoveries, (unsigned)link.rx_frames,
                     recover_interval_ms / 1000);
      }
      if (recover_bus()) link.recoveries++;
      recover_interval_ms *= 2;
      if (recover_interval_ms > CAN_RECOVER_MAX_INTERVAL_MS) {
        recover_interval_ms = CAN_RECOVER_MAX_INTERVAL_MS;
      }
      have_status = (twai_get_status_info(&st) == ESP_OK);
    }

    if (millis() - last_status >= CAN_STATUS_INTERVAL_MS) {
      last_status = millis();
      print_bus_status("status:");
    }

    // Publish the counters. The driver keeps its own cumulative totals for the
    // things it sees; the rest are ours.
    if (have_status) {
      link.rx_missed = st.rx_missed_count;
      link.bus_errors = st.bus_error_count;
      link.tx_failed = st.tx_failed_count + tx_enqueue_failed;
      link.state = (uint8_t)st.state;
    }
    if (g_can_mutex) xSemaphoreTake(g_can_mutex, portMAX_DELAY);
    g_can_link = link;
    if (g_can_mutex) xSemaphoreGive(g_can_mutex);
  }
}

void pylontech_can_init(int tx_pin, int rx_pin) {
  if (!g_can_mutex) {
    g_can_mutex = xSemaphoreCreateMutex();
  }
  g_tx_pin = tx_pin;
  g_rx_pin = rx_pin;
  Serial.printf("[CAN] init, TX=GPIO%d RX=GPIO%d\n", tx_pin, rx_pin);
  // 6 KB, not 4: printInfo/printWarning format into a buffer and append to
  // LittleFS, which is heavier than the Serial.printf the sniffer used.
  xTaskCreatePinnedToCore(pylontech_can_task, "pylontech_can", 6144, NULL, 1, NULL, 1);
}

bool pylontech_can_get(PylontechCanState* out) {
  if (!out) return false;
  if (g_can_mutex) xSemaphoreTake(g_can_mutex, portMAX_DELAY);
  *out = g_can_state;
  if (g_can_mutex) xSemaphoreGive(g_can_mutex);
  return true;
}

bool pylontech_can_valid() {
  PylontechCanState s;
  if (g_can_mutex) xSemaphoreTake(g_can_mutex, portMAX_DELAY);
  s = g_can_state;
  if (g_can_mutex) xSemaphoreGive(g_can_mutex);
  return state_is_fresh(s, millis());
}

void pylontech_can_get_link(PylontechCanLink* out) {
  if (!out) return;
  if (g_can_mutex) xSemaphoreTake(g_can_mutex, portMAX_DELAY);
  *out = g_can_link;
  if (g_can_mutex) xSemaphoreGive(g_can_mutex);
}

void pylontech_can_get_raw(PylontechCanRaw* out) {
  if (!out) return;
  if (g_can_mutex) xSemaphoreTake(g_can_mutex, portMAX_DELAY);
  memcpy(out, g_can_raw, sizeof(g_can_raw));
  if (g_can_mutex) xSemaphoreGive(g_can_mutex);
}
