#pragma once
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "config.h"

// Polling interval (ms) between QMOD+QPIGS cycles
#define INVERTER_POLL_INTERVAL_MS 3000

// Consecutive failed poll cycles tolerated before inverter data is marked
// invalid. Occasional single dropouts are acceptable and must not invalidate.
#define INVERTER_FAIL_INVALIDATE_THRESHOLD 3

// QPIRI (configuration) read interval. These values change only when somebody
// writes them - from the settings page or the inverter's front panel - so this
// is about how fast a change should surface, not about resolution. Against that,
// every read is another RS232 exchange on the shared 15 m cable that
// doc/rj45_cable_wiring.md names first whenever CAN error counters climb: at
// 5 minutes it adds ~288 exchanges a day to the ~28 800 QMOD+QPIGS pairs the
// 3 s poll already sends, about 1 %.
#define INVERTER_CONFIG_INTERVAL_MS 300000

// Parsed status structure (subset of QPIGS fields)
struct InverterState {
  float grid_voltage;           // BBB.B  Grid voltage [V]
  float grid_frequency;         // CC.C   Grid frequency [Hz]
  float ac_out_voltage;         // DDD.D  AC output voltage [V]
  float ac_out_frequency;       // EE.E   AC output frequency [Hz]
  int   ac_apparent_va;         // FFFF   AC output apparent power [VA]
  int   ac_active_w;            // GGGG   AC output active power [W]
  int   load_percent;           // HHH    Output load percent [%] (max of W% or VA%)
  float bus_voltage;            // III    BUS voltage [V]
  float batt_voltage;           // JJ.JJ  Battery voltage [V]
  float batt_charge_current;    // KKK    Battery charging current [A]
  int   batt_soc;               // OOO    Battery capacity [%]
  float heatsink_temp;          // TTTT   Inverter heat sink temperature [°C] (or NTC A/D)
  float pv_input_current_batt;  // EEEE   PV input current for battery [A]
  float pv_input_voltage;       // UUU.U  PV input voltage [V]
  float batt_voltage_from_scc;  // WW.WW  Battery voltage from SCC [V]
  float batt_discharge_current; // PPPPP  Battery discharge current [A]
  uint8_t device_status_bits;   // b7..b0 Device status bits (b7 SBU, b6 config changed, b5 SCC fw, b4 load status, b3 reserved, b2 charging status, b1 SCC charging, b0 AC charging)
  int   batt_fan_offset_10mv;   // QQ     Battery voltage offset for fans on (10mV units)
  int   eeprom_version;         // VV     EEPROM version
  int   pv_charging_power;      // MMMMM  PV charging power [W]
  uint8_t additional_status_bits;// b10..b8 Additional status bits (b10 charging to float flag, b9 Switch On, b8 reserved)
  uint32_t ts_ms;               // timestamp (millis) when these values were last updated
};

// Global variables (updated by background task)
extern InverterState g_inverter_status;
// Global validity flag for inverter data (demo mode always true)
extern bool g_inverter_data_valid;
extern float g_temp_h;
extern float g_temp_l;
extern char g_inverter_mode_code; // single-letter mode code from QMOD
extern char g_inverter_mode_name[32];

// Initialize inverter communication and start background polling task
void inverter_comm_init(int rx_pin, int tx_pin);

// Access functions that copy protected data (thread-safe)
bool inverter_get_status(InverterState* out);
// Thread-safe read of the inverter data validity flag.
bool inverter_data_valid();

// Temporarily silence this link's RS232 traffic. Both serial links share the
// 15 m cable with the CAN pair, and the console link's 115200 baud RS232 swing
// is the first suspect for crosstalk into it (doc/rj45_cable_wiring.md), so
// muting them is how that gets tested without a trip to the site.
//
// Pausing clears the validity flag, so relay.cpp forces the boiler off rather
// than regulating on a frozen snapshot. `max_ms` is an auto-resume deadline:
// the link brings itself back even if the operator loses connectivity, which
// matters because nobody can reach the hardware.
void inverter_comm_set_paused(bool paused, uint32_t max_ms);
bool inverter_comm_paused();
// Thread-safe read of the latest battery discharge current [A].
float inverter_batt_discharge_current();
bool inverter_get_mode(char* out_code, char* out_name, size_t name_cap);

// Inverter configuration, read from QPIRI. Separate from InverterState on
// purpose: this is what the inverter is *allowed* to do (the counterpart of the
// BMS limits arriving over CAN), it changes only when somebody writes it, and it
// is sampled every INVERTER_CONFIG_INTERVAL_MS instead of every poll cycle.
//
// Only the five tokens worth storing are parsed here. The full 25-token mapping
// stays in data/settings.js, which owns it so the layout can be corrected by
// re-uploading web files without reflashing - the token order is model-dependent.
struct InverterConfig {
  float max_charge_a;    // QPIRI[14] total charge current limit (solar + grid)
  float bulk_v;          // QPIRI[10] bulk / absorb voltage
  float float_v;         // QPIRI[11] float voltage
  float lvd_v;           // QPIRI[9]  low DC cutoff
  float redischarge_v;   // QPIRI[22] SBU return: load goes back on the battery
                         //           above this voltage
  uint32_t ts_ms;        // millis() of the last successful read, 0 = never read
};

// Thread-safe snapshot of the last successfully read configuration. Returns
// false while nothing has been read yet (ts_ms == 0), in which case `out` holds
// zeros and must not be published anywhere.
//
// Deliberately NOT tied to g_inverter_data_valid: a paused or briefly dead link
// does not make the configuration untrue, so the last known values stay
// available to the UI. Storage has the stricter rule - influx.cpp writes a point
// only when ts_ms advances, so an outage leaves a gap rather than a run of
// values nobody actually read.
bool inverter_get_config(InverterConfig* out);

// Send an arbitrary command (e.g. "QPIRI", "QFLAG") synchronously and return the
// response payload (contents inside '(' .. , without frame/CRC). Blocks up to ~1s
// and is serialized against the background poll task. Returns false on
// timeout/CRC mismatch. Intended for on-demand reads, not periodic polling.
bool inverter_query_raw(const char* cmd, String& out_payload);
