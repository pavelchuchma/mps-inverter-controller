#include "pylontech_can.h"
#include "influx.h"
#include "utils.h"
#include <driver/twai.h>
#include <math.h>
#include <string.h>

// Frame identifiers broadcast by the BMS, see doc/battery_can_spec.md.
#define CAN_ID_LIMITS 0x351   // charge voltage, CCL, DCL
#define CAN_ID_SOC 0x355      // SoC, SoH
#define CAN_ID_MEASURED 0x356 // voltage, current, temperature
#define CAN_ID_ALARMS 0x359   // protection / alarm bitmaps, module count
#define CAN_ID_REQUESTS 0x35C // charge / discharge enable request
#define CAN_ID_VENDOR 0x35E   // manufacturer string "PYLON"

// Inverter reply, 8 zero bytes at 1 Hz. Sent only as a silence probe — see
// the transmit policy below.
#define CAN_ID_HEARTBEAT 0x305

// Bus health line interval.
#define CAN_STATUS_INTERVAL_MS 30000

// Periodic health line into app.log, so a day of link behaviour can be read
// back after the fact. Deltas rather than cumulative totals: what matters when
// reading a day at once is the rate, not the running count. 72 lines a day at
// ~150 B is ~10 kB, comfortably inside the 100 kB the log rotates at, and far
// away from the per-frame writes constraint 4 of the data spec forbids.
// One health line every 20 minutes: 72 lines a day at ~150 B is ~10 kB, well
// inside the 100 kB log rotation (doc/battery_can_data_spec.md).
#define CAN_LOG_INTERVAL_MS (20UL * 60UL * 1000UL)

// Tier C event triggers (doc/battery_can_data_spec.md). A change worth seeing
// at its real time rather than at the next grid point calls influx_log_event(),
// which snapshots every measurement off-grid and forces a flush. The rate limit
// is absolute, not a multiple of METRICS_SAMPLE_INTERVAL_MS: relaxing the grid
// must not make a CCL step harder to see, and a dithering CCL must not be able
// to flood the batch either way.
#define CAN_EVENT_MIN_INTERVAL_MS 30000
#define CAN_EVENT_CCL_DELTA_A 5.0f

// Trace every 0x351 to the serial monitor, so the link can be watched live
// during bring-up. 0x351 arrives once per burst (every 2 s on this pack), so
// the cost is negligible - and it goes to Serial only, never through
// printInfo(): nothing per-frame may reach flash.
#define CAN_TRACE_LIMITS 1

// --- transmit: the 0x305 inverter reply ------------------------------------
// The sniffer's policy, adopted after it out-ran every variant of this module
// (todo 002): nothing is EVER transmitted while the pack broadcasts. The
// instrumented sniffer held the link for 13+ hours without putting a single
// frame on the wire, so the vendor's 1 Hz inverter reply is demonstrably not
// needed to keep this pack talking — and the only runs that never received a
// frame at all were the ones with the 1 Hz reply re-enabled.
//
// What remains is the sniffer's silence probe: after
// CAN_SILENCE_BEFORE_HEARTBEAT_MS without a frame, send 0x305 at 1 Hz until
// a frame arrives (in case 0x305 is what wakes a quiet BMS). Its tx counters
// double as the freeze diagnostic — after a silent period, tx_failed rising
// means the bus is genuinely dead, while successful probes mean the pack is
// acknowledging and only our receiver has gone deaf.
//
// One deliberate deviation from the sniffer: the probe is single-shot
// (TWAI_MSG_FLAG_SS). The sniffer's plain transmit auto-retried into the
// silent bus at bus speed and rode the error counter to bus-off within
// milliseconds of each probe; single-shot costs one increment (TEC +8)
// instead, so bus-off takes ~30 s of probing and the periodic recovery check
// keeps up with it — and only ever during silence.
#define CAN_SILENCE_BEFORE_HEARTBEAT_MS 10000

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

// Frame groups touched by the current burst: which of them arrived decides
// what the commit below folds into the accumulator and which event triggers
// are worth evaluating.
#define CAN_GROUP_LIMITS 0x01
#define CAN_GROUP_SOC 0x02
#define CAN_GROUP_MEASURED 0x04
#define CAN_GROUP_ALARMS 0x08
#define CAN_GROUP_REQUESTS 0x10

// The bring-up range check that used to sit here is gone (commit 2 of
// doc/battery_can_data_spec.md): /can confirmed the layout, the encoding is
// fixed for the life of the pack, and from here a hardcoded window could only
// misfire. The standing validation is now the console cross-check — the same
// voltage, current, temperature and SoC arrive over an independent link, and
// the two stored series have to agree.

static int g_tx_pin = -1;
static int g_rx_pin = -1;

static SemaphoreHandle_t g_can_mutex = NULL;
static PylontechCanState g_can_state = {};
static PylontechCanLink g_can_link = {};
static PylontechCanRaw g_can_raw[CAN_RAW_SLOTS] = {};

// current_a folded over the bursts since the metrics sampler last read it, see
// pylontech_can_take_current_range(). Guarded by g_can_mutex.
static float g_cur_min_a = 0.0f;
static float g_cur_max_a = 0.0f;
static bool g_cur_range_have = false;

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

// Record the raw payload for /can into the task-local table; the table is
// published to g_can_raw once per burst, not per frame — todo 002 put the
// per-frame mutex take on the suspect list, so the receive path stays free of
// any shared state.
static void record_raw(PylontechCanRaw* raw, const twai_message_t& m, uint32_t now) {
  int free_slot = -1;
  for (int i = 0; i < CAN_RAW_SLOTS; ++i) {
    if (raw[i].id == m.identifier) {
      raw[i].dlc = m.data_length_code;
      memcpy(raw[i].data, m.data, 8);
      raw[i].ts_ms = now;
      raw[i].count++;
      return;
    }
    if (free_slot < 0 && raw[i].id == 0) free_slot = i;
  }
  if (free_slot < 0) return;  // more identifiers than expected: keep the first six
  raw[free_slot].id = m.identifier;
  raw[free_slot].dlc = m.data_length_code;
  memcpy(raw[free_slot].data, m.data, 8);
  raw[free_slot].ts_ms = now;
  raw[free_slot].count = 1;
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
#if CAN_TRACE_LIMITS
      Serial.printf("[CAN] 0x351 dlc=%d raw", m.data_length_code);
      for (int i = 0; i < m.data_length_code; ++i) Serial.printf(" %02X", d[i]);
      Serial.printf("  CVL %.1f V  CCL %.1f A  DCL %.1f A\n",
                    s->charge_v, s->ccl_a, s->dcl_a);
#endif
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

// Queue an off-grid snapshot, rate-limited; returns false when the limit held it
// back, so the caller can keep the request pending instead of losing it. That
// matters because the rate limit is 30 s and the triggers are edges: a change
// that arrives too soon after the last point would otherwise never be stored.
// `last_ms` is the task-local time of the last snapshot, zero meaning none yet.
static bool log_can_event(unsigned long* last_ms) {
  unsigned long now = millis();
  if (*last_ms != 0 && now - *last_ms < CAN_EVENT_MIN_INTERVAL_MS) return false;
  *last_ms = now;
  influx_log_event();
  return true;
}

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
  // that fails the range check can be dropped whole. `raw` is the /can payload
  // table, task-local for the same reason as everything else here: the receive
  // path must not touch the mutex (todo 002).
  PylontechCanState scratch = {};
  PylontechCanState burst = {};
  PylontechCanLink link = {};
  PylontechCanRaw raw[CAN_RAW_SLOTS] = {};
  uint8_t burst_groups = 0;
  bool burst_active = false;
  unsigned long burst_start_ms = 0;
  // Initialized to now so a bus that is quiet from boot logs its silence once
  // after the same 10 s the sniffer used.
  unsigned long last_frame_ms = millis();

  // Zero-transmit build: this firmware never puts a frame on the wire, so the
  // flag only tracks whether the current silence has already been logged.
  bool was_silent = false;
  unsigned long last_status = millis();

  // Transition tracking for the Serial/app.log lines. Per-frame printing is
  // gone: GET /can carries the detail on demand.
  bool was_valid = false;
  bool have_prev_alarms = false;
  bool have_prev_requests = false;
  uint32_t prev_protection = 0;
  uint32_t prev_alarm = 0;
  uint8_t prev_request_flags = 0;
  // Last CCL that produced an event point, not the last one received: the
  // trigger is a step away from what is already on the timeline, so a slow
  // drift still fires once it has accumulated CAN_EVENT_CCL_DELTA_A.
  float prev_logged_ccl = 0.0f;
  bool have_prev_ccl = false;
  unsigned long last_event_ms = 0;
  // A trigger fired but the rate limit has not expired yet; emitted as soon as
  // it does, so a burst of changes costs one point but loses none of them.
  bool event_pending = false;

  // Enqueue failures, kept apart from the driver's own count of transmissions
  // that failed on the wire; link.tx_failed is the sum of the two.
  uint32_t tx_enqueue_failed = 0;

  unsigned long last_log_ms = millis();
  uint32_t last_log_rx = 0;
  uint32_t last_log_err = 0;

  unsigned long recover_interval_ms = CAN_RECOVER_MIN_INTERVAL_MS;
  unsigned long last_recover_ms = 0;
  unsigned long last_busoff_log_ms = 0;

  for (;;) {
    twai_message_t m;
    // 200 ms timeout, the sniffer's value. The 20 ms it briefly ran at meant
    // ~50 wakeups/s of status polling and mutex traffic on an idle bus; the
    // sniffer's cadence is the one proven to hold the link (todo 002). Burst
    // gap detection still works: frames within a burst arrive back to back,
    // and the 2 s between bursts dwarfs one blocked receive.
    if (twai_receive(&m, pdMS_TO_TICKS(200)) == ESP_OK) {
      unsigned long now = millis();
      link.rx_frames++;
      link.last_rx_ms = now;
      last_frame_ms = now;
      // A frame proves there is a peer, so recovery can be prompt again.
      recover_interval_ms = CAN_RECOVER_MIN_INTERVAL_MS;
      if (was_silent) {
        printInfo("[CAN] frames arriving again after silence");
        was_silent = false;
      }
      if (!burst_active) {
        burst_active = true;
        burst_start_ms = now;
        burst = scratch;
        burst_groups = 0;
      }
      burst_groups |= decode_frame(m, &burst, now);
      record_raw(raw, m, now);
    }

    // End of burst: no frame for CAN_BURST_GAP_MS, or CAN_BURST_MAX_MS since
    // the burst started. Committing per burst rather than per frame means a
    // reader never sees ccl_a from burst N next to current_a from burst N-1.
    if (burst_active
        && (millis() - last_frame_ms >= CAN_BURST_GAP_MS
            || millis() - burst_start_ms >= CAN_BURST_MAX_MS)) {
      burst_active = false;
      scratch = burst;
      // The one mutex take of the steady state, every ~2 s: raw payloads, the
      // decoded state and the link counters go out together. The driver-side
      // fields in the counters refresh in the minute branch, so /can sees those
      // at most a minute stale.
      if (g_can_mutex) xSemaphoreTake(g_can_mutex, portMAX_DELAY);
      memcpy(g_can_raw, raw, sizeof(g_can_raw));
      g_can_state = scratch;
      g_can_link = link;
      // Fold this burst's current into the range the metrics sampler will read.
      // Under the same mutex as the state it belongs to, so a sampler that
      // takes the range cannot get one from a burst the state does not show.
      if (burst_groups & CAN_GROUP_MEASURED) {
        if (!g_cur_range_have) {
          g_cur_min_a = scratch.current_a;
          g_cur_max_a = scratch.current_a;
          g_cur_range_have = true;
        } else {
          if (scratch.current_a < g_cur_min_a) g_cur_min_a = scratch.current_a;
          if (scratch.current_a > g_cur_max_a) g_cur_max_a = scratch.current_a;
        }
      }
      if (g_can_mutex) xSemaphoreGive(g_can_mutex);

      // Tier C triggers. Each one is a change that has to land in InfluxDB at
      // its real time rather than at the next grid point; the log lines below
      // are the same transitions, on the same conditions, into app.log.
      if (burst_groups & CAN_GROUP_LIMITS) {
        // A CCL step, or any crossing of zero. Zero means "do not charge" and
        // is worth a point whatever the step size, so it is tested separately
        // from the delta. The very first burst only seeds the baseline.
        if (!have_prev_ccl) {
          prev_logged_ccl = scratch.ccl_a;
          have_prev_ccl = true;
        } else if (fabsf(scratch.ccl_a - prev_logged_ccl) >= CAN_EVENT_CCL_DELTA_A
                   || (scratch.ccl_a == 0.0f) != (prev_logged_ccl == 0.0f)) {
          prev_logged_ccl = scratch.ccl_a;
          event_pending = true;
        }
      }
      if (burst_groups & CAN_GROUP_ALARMS) {
        if (have_prev_alarms
            && (scratch.protection != prev_protection || scratch.alarm != prev_alarm)) {
          printWarning("[CAN] protection 0x%04X -> 0x%04X, alarm 0x%04X -> 0x%04X",
                       (unsigned)prev_protection, (unsigned)scratch.protection,
                       (unsigned)prev_alarm, (unsigned)scratch.alarm);
          event_pending = true;
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
          event_pending = true;
        }
        prev_request_flags = scratch.request_flags;
        have_prev_requests = true;
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
      // Both directions are worth a point: the one before the gap shows what
      // the pack was doing when it stopped talking, the one after shows what
      // changed while it was quiet.
      event_pending = true;
    }

    // The single emit point for every tier C trigger above. Retried each
    // iteration (~200 ms) while the rate limit holds, which is what keeps a
    // change that arrived just after the last point from being dropped.
    if (event_pending && log_can_event(&last_event_ms)) event_pending = false;

    // Zero transmit: this firmware never sends 0x305 (nor anything else). The
    // 1 Hz probe it used to send into a silent bus was unacknowledged, drove
    // TEC to bus-off, and — captured on the short, terminated cable on
    // 2026-09-13 — held the link down in a self-inflicted bus-off/recovery
    // loop that only a driver restart or a monitor connect could break. The
    // pack broadcasts unprompted, so the hardware ACK of normal mode (emitted
    // only while the pack itself transmits) is all this side ever needs.
    // Silence is logged once per episode, no frame is put on the wire.
    if (!was_silent
        && millis() - last_frame_ms > CAN_SILENCE_BEFORE_HEARTBEAT_MS) {
      printWarning("[CAN] silent for %lu s (zero-transmit build: not probing)",
                   (unsigned long)(CAN_SILENCE_BEFORE_HEARTBEAT_MS / 1000));
      was_silent = true;
    }

    // The 30 s Serial line, and with it the only place bus-off is looked for.
    // Bus-off can only be reached by transmitting, transmitting only happens
    // during silence, and single-shot probes take ~30 s to get there — so this
    // cadence keeps up, and the steady state polls no status at all (todo 002
    // suspect 1).
    if (millis() - last_status >= CAN_STATUS_INTERVAL_MS) {
      last_status = millis();
      print_bus_status("status:");
      twai_status_info_t st;
      bool have_status = twai_get_status_info(&st) == ESP_OK;
      // Copy the driver-side counters out on this cadence, not on the health
      // line's: /can reads them live, and the health line only fires every
      // CAN_LOG_INTERVAL_MS, which would leave the endpoint up to 20 minutes
      // stale. The status is being queried here anyway, so this is free.
      if (have_status) {
        link.rx_missed = st.rx_missed_count;
        link.bus_errors = st.bus_error_count;
        link.rx_err = (uint8_t)st.rx_error_counter;
        link.tx_err = (uint8_t)st.tx_error_counter;
        link.tx_failed = st.tx_failed_count + tx_enqueue_failed;
        link.state = (uint8_t)st.state;
      }
      if (have_status
          && st.state == TWAI_STATE_BUS_OFF
          && millis() - last_recover_ms >= recover_interval_ms) {
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
      }
    }

    if (millis() - last_log_ms >= CAN_LOG_INTERVAL_MS) {
      unsigned long elapsed = millis() - last_log_ms;
      last_log_ms = millis();
      // The counters themselves are refreshed by the 30 s status branch above;
      // this only turns them into deltas. The driver keeps its own cumulative
      // totals for the things it sees; the rest are ours. REC and TEC are the
      // CAN error counters themselves, not running totals: each error adds to
      // them and each success takes away, so they say whether the controller is
      // currently reacting to something on the wire.
      uint32_t d_rx = link.rx_frames - last_log_rx;
      uint32_t d_err = link.bus_errors - last_log_err;
      last_log_rx = link.rx_frames;
      last_log_err = link.bus_errors;
      float per_min = elapsed ? (d_rx * 60000.0f / (float)elapsed) : 0.0f;
      if (is_valid) {
        printInfo("[CAN] rx +%u (%.0f/min) err +%u REC %u TEC %u missed %u recov %u tx_fail %u | "
                  "%.2f V %+.1f A %.1f C SoC %d %% SoH %d %% CCL %.1f A DCL %.1f A "
                  "prot 0x%04X alarm 0x%04X flags 0x%02X",
                  (unsigned)d_rx, per_min, (unsigned)d_err,
                  (unsigned)link.rx_err, (unsigned)link.tx_err,
                  (unsigned)link.rx_missed, (unsigned)link.recoveries,
                  (unsigned)link.tx_failed,
                  scratch.voltage_v, scratch.current_a, scratch.temp_c,
                  scratch.soc, scratch.soh, scratch.ccl_a, scratch.dcl_a,
                  (unsigned)scratch.protection, (unsigned)scratch.alarm,
                  scratch.request_flags);
      } else {
        printInfo("[CAN] rx +%u err +%u REC %u TEC %u missed %u recov %u tx_fail %u state %d | "
                  "link down, last frame %lu s ago",
                  (unsigned)d_rx, (unsigned)d_err,
                  (unsigned)link.rx_err, (unsigned)link.tx_err, (unsigned)link.rx_missed,
                  (unsigned)link.recoveries, (unsigned)link.tx_failed, (int)link.state,
                  link.last_rx_ms ? (unsigned long)((millis() - link.last_rx_ms) / 1000) : 0UL);
      }
      // Publish the refreshed counters. During normal traffic the burst
      // commit republishes them every ~2 s anyway; this covers a silent bus,
      // where /can would otherwise show counters frozen at the last burst.
      if (g_can_mutex) xSemaphoreTake(g_can_mutex, portMAX_DELAY);
      g_can_link = link;
      if (g_can_mutex) xSemaphoreGive(g_can_mutex);
    }
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

bool pylontech_can_take_current_range(float* lo, float* hi) {
  if (!lo || !hi) return false;
  bool have = false;
  if (g_can_mutex) xSemaphoreTake(g_can_mutex, portMAX_DELAY);
  have = g_cur_range_have;
  if (have) {
    *lo = g_cur_min_a;
    *hi = g_cur_max_a;
    g_cur_range_have = false;  // the next window starts from the next burst
  }
  if (g_can_mutex) xSemaphoreGive(g_can_mutex);
  return have;
}
