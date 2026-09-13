#pragma once
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// ---------------------------------------------------------------------------
// Pylontech US5000 CAN link — parsed state (stage 2 of
// doc/battery_can_spec.md, commits 1-2 of doc/battery_can_data_spec.md).
//
// Receives the BMS broadcast, decodes it against the vendor specification and
// publishes it as a mutex-guarded snapshot. The snapshot is stored (influx.cpp
// writes chajda-battery-can on the metrics grid, plus an off-grid point on a
// significant change), but no control path consumes it yet — that is commit 3,
// and it waits on a full charge/discharge cycle of stored data. GET /can still
// shows the raw payload next to the decoded values, so byte order and scaling
// stay verifiable after any change to the parser.
//
// The driver runs in TWAI_MODE_NORMAL because the ESP32 is the only other node
// on the battery's CAN port. CAN needs an acknowledge bit from a second node;
// in listen-only mode the battery would never see one, would retransmit every
// frame and drop into error-passive. Normal mode acknowledges, which is the
// only thing this module puts on the wire while the pack broadcasts — the
// 0x305 inverter reply is sent only as a probe into a silent bus, see the
// transmit policy in pylontech_can.cpp.
// ---------------------------------------------------------------------------

// This pack repeats the whole set every 2 s, not the 1 s the vendor documents
// (measured: 180 frames/min over six identifiers). Five missed bursts is
// therefore 10 s, not 5 — the intent is to tolerate a few dropped bursts, and
// at 5 s a single one plus jitter would already flap the validity flag.
// Replaces the console link's failed-poll counter; CAN has no poll cycles to
// count.
#define CAN_STALE_MS 10000

// Burst detection. All six frames arrive within ~10 ms, then the bus is quiet
// until the next repeat. A burst ends after this much silence, or after
// CAN_BURST_MAX_MS from its first frame if the gap never appears.
#define CAN_BURST_GAP_MS 50
#define CAN_BURST_MAX_MS 200

// 0x305 silence-probe rate, active only after 10 s without a frame.
#define CAN_HEARTBEAT_INTERVAL_MS 1000

// Decoded BMS state, committed once per burst so a reader never sees fields
// from two different bursts side by side. Values are in natural units.
struct PylontechCanState {
  // 0x351 — limits (bytes 6-7 are blank in the vendor spec: there is no DVL)
  float charge_v;          // recommended charge voltage
  float ccl_a;             // charge current limit — the reason for this link
  float dcl_a;             // discharge current limit
  // 0x355
  int soc;                 // %
  int soh;                 // %
  // 0x356
  float voltage_v;
  float current_a;         // + charge / - discharge
  float temp_c;            // average cell temperature
  // 0x359 — kept as raw words: only seven bits are defined across the four
  // bytes of a 2018 specification, so named booleans would silently discard
  // whatever a newer firmware puts in the rest.
  uint32_t protection;     // bytes 0-1 (tables 1, 2), zero-extended
  uint32_t alarm;          // bytes 2-3 (tables 3, 4), zero-extended
  uint8_t modules;         // byte 4
  // 0x35C
  uint8_t request_flags;   // raw d[0], see the accessors below
  // Freshness per frame group: millis() of the burst that carried it, 0 until
  // the group has been seen at least once.
  uint32_t limits_ts_ms;
  uint32_t soc_ts_ms;
  uint32_t measured_ts_ms;
  uint32_t alarms_ts_ms;
  uint32_t requests_ts_ms;
};

// Link health, deliberately separate from the data: it is wanted precisely
// when pylontech_can_valid() is false and the data series has a gap.
struct PylontechCanLink {
  uint32_t rx_frames;      // total received since boot
  uint32_t rx_missed;      // driver rx-queue overflow
  uint32_t bus_errors;     // count of (bit, stuff, CRC, form, ACK) errors; the
                           // driver does not say which, so read it next to the
                           // two counters below
  uint8_t rx_err;          // CAN receive error counter (REC), live not cumulative
  uint8_t tx_err;          // CAN transmit error counter (TEC), live not cumulative
  uint32_t recoveries;     // bus-off recoveries performed
  uint32_t tx_failed;      // 0x305 heartbeats that did not go out
  uint8_t state;           // twai_state_t
  uint32_t last_rx_ms;
};

// Last raw payload per identifier, for GET /can. Diagnostic data, kept out of
// the state struct on purpose.
struct PylontechCanRaw {
  uint32_t id;
  uint8_t dlc;
  uint8_t data[8];
  uint32_t ts_ms;          // millis() of the last frame with this identifier
  uint32_t count;          // frames received with this identifier
};
#define CAN_RAW_SLOTS 6

// 0x35C table 5. The two force-charge bits stay separate: bit 5 is for an
// inverter that lets the pack shut down, bit 4 for one that does not.
inline bool can_charge_enabled(uint8_t request_flags) { return request_flags & 0x80; }
inline bool can_discharge_enabled(uint8_t request_flags) { return request_flags & 0x40; }
inline bool can_force_charge_1(uint8_t request_flags) { return request_flags & 0x20; }
inline bool can_force_charge_2(uint8_t request_flags) { return request_flags & 0x10; }
inline bool can_full_charge_requested(uint8_t request_flags) { return request_flags & 0x08; }

// Install the TWAI driver on the given pins and start the receive task.
void pylontech_can_init(int tx_pin, int rx_pin);

// Thread-safe snapshot copy of the latest decoded BMS state.
bool pylontech_can_get(PylontechCanState* out);

// True while 0x351, 0x355 and 0x356 have all been seen within CAN_STALE_MS.
bool pylontech_can_valid();

// Thread-safe copy of the link counters.
void pylontech_can_get_link(PylontechCanLink* out);

// Thread-safe copy of the raw payload table; `out` holds CAN_RAW_SLOTS entries.
// Unused slots have id == 0.
void pylontech_can_get_raw(PylontechCanRaw* out);

// Min/max of current_a folded over every burst since the last call, and reset
// by it. The CAN task sees the current at ~0.5 Hz while the metrics sampler
// reads once per grid interval, so a point sample throws the rest away; this
// turns each stored point into a range instead. Returns false when no burst has
// carried 0x356 since the last call, in which case *lo and *hi are untouched.
//
// The reset is inside the accessor on purpose: it makes read-and-clear atomic,
// which only works while there is exactly one consumer (influx.cpp).
bool pylontech_can_take_current_range(float* lo, float* hi);

// Hold the bus dominant for two seconds, then restart the driver. Forces every
// other node into bus-off and then hands it a clean idle bus - the only thing
// that has so far restarted a pack that has stopped broadcasting. Blocks for
// about two seconds. Only for a link that is already dead.
bool pylontech_can_force_bus_reset();
