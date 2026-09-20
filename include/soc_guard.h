#pragma once
#include <Arduino.h>

// SoC guard: stop discharging the battery at a BMS SoC by moving the
// inverter's low DC cut-off (program 29, RS232 PSDV, QPIRI[9]).
//
// The installation is off-grid, so the cut-off is the only inverter setting
// that ever ends a discharge - and it is a voltage, which on LiFePO4 says
// nothing about SoC (the pack rests at 49.75 V from 44 % to 96 %). The guard
// therefore switches the cut-off between two values on SoC edges: 48.0 V when
// the BMS reports SOC_GUARD_ARM_PCT or less, which trips the inverter at once,
// and the normal 46.0 V again at SOC_GUARD_DISARM_PCT. Between the two the
// output runs only while PV holds the pack above 48 V.
//
// Writes happen on the edge only and are verified against the 5-minute QPIRI
// read in inverter_comm; a mismatch (NAK, lost frame, somebody editing program
// 29 on the LCD) is rewritten at most once per config interval. A stale SoC
// changes nothing. See doc/todo/007-soc-guard-cutoff.md.

// BMS SoC [%] at which the cut-off is raised (inclusive).
#define SOC_GUARD_ARM_PCT 15
// BMS SoC [%] at which the cut-off is restored (inclusive). The gap is the
// hysteresis that stops the output from flapping on the first morning sun.
#define SOC_GUARD_DISARM_PCT 25
// Cut-off while armed. 48.0 is the top of the PSDV range on a 48 V unit; the
// pack rests at ~47.8 V at 15 %, so the inverter trips without waiting for a
// load step.
#define SOC_GUARD_LVD_ARMED_V 48.0f
// Cut-off while disarmed - the value the inverter had before the guard existed.
#define SOC_GUARD_LVD_NORMAL_V 46.0f
// A BMS SoC older than this is not acted on.
#define SOC_GUARD_SOC_MAX_AGE_MS 300000
// Two LVD values closer than this are the same setting (QPIRI has 0.1 V
// resolution).
#define SOC_GUARD_LVD_EPS_V 0.05f

struct SocGuardState {
  bool enabled;          // switch, persisted in NVS
  bool armed;            // cut-off is (meant to be) at SOC_GUARD_LVD_ARMED_V
  bool decided;          // armed has been derived from a real QPIRI read
  float intended_lvd_v;  // what the guard wants program 29 to be
  uint32_t last_write_ms;
  bool last_write_ok;    // ACK on the last PSDV write
  int soc_at_last_change;
};

// Read the enabled flag from NVS. Call after inverter_comm_init() and
// pylontech_can_init(); decides nothing by itself.
void soc_guard_init();

// Evaluate once. Cheap while nothing changes; blocks up to ~1 s on the RS232
// when it writes, which happens on an edge only.
void soc_guard_tick();

// Turn the guard on or off. Off means hands off: an armed cut-off stays where
// it is until the settings page or a re-enable moves it.
void soc_guard_set_enabled(bool on);

void soc_guard_get(SocGuardState* out);
