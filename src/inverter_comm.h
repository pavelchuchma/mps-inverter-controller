#pragma once
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "config.h"

// Polling interval (ms) between QMOD+QPIGS cycles
#define INVERTER_POLL_INTERVAL_MS 3000

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
  float pv_input_current;       // EEEE   PV input current for battery [A]
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
void inverter_comm_init();

// Access functions that copy protected data (thread-safe)
bool inverter_get_status(InverterState* out);
bool inverter_get_mode(char* out_code, char* out_name, size_t name_cap);
