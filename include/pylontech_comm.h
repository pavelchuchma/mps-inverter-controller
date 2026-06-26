#pragma once
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// Polling interval (ms) between 'pwr' command cycles
#define PYLONTECH_POLL_INTERVAL_MS 5000

// Console port serial settings (US5000 console port)
#define PYLONTECH_BAUD 115200
#define PYLONTECH_CMD "pwr 1\r"          // command terminated with CR

// Response collection timing
#define PYLONTECH_READ_WINDOW_MS 2000  // total time to collect a response
#define PYLONTECH_IDLE_GAP_MS 300      // stop after this gap once data started

// Consecutive failed poll cycles tolerated before battery data is marked
// invalid. Occasional single dropouts are acceptable and must not invalidate.
#define PYLONTECH_FAIL_INVALIDATE_THRESHOLD 3

// Parsed battery status (subset of 'pwr' console fields), values in natural units.
struct PylontechState {
  bool     cfet_on;            // CFetState ON/OFF
  bool     dfet_on;            // DFetState ON/OFF
  int      pack_index;         // "Power N" header
  float    voltage;            // V   (from mV)
  float    current;            // A   (from mA; + charge / - discharge)
  float    temperature;        // °C  (from mC)
  int      soc;                // Coulomb [%]
  int      total_capacity_mah; // Total Coulomb [mAH]
  float    max_voltage;        // V   (from mV)
  int      charge_times;       // cycle count
  char     basic_status[12];   // "Idle" / "Charge" / "Discharge"
  bool     heater_on;          // Heater Status
  uint32_t bat_events;         // Bat Events bitmask
  uint32_t power_events;       // Power Events bitmask
  uint32_t system_fault;       // System Fault bitmask
  uint32_t system_alarm;       // System Alarm bitmask
  uint32_t ts_ms;              // millis() when these values were last updated
};

// Global state (updated by background task)
extern PylontechState g_pylontech_status;
// Global validity flag for battery data.
extern bool g_pylontech_data_valid;

// Initialize battery console UART (Serial2) and start background polling task.
// Periodically sends 'pwr', parses the console response into g_pylontech_status,
// and maintains g_pylontech_data_valid.
void pylontech_comm_init(int rx_pin, int tx_pin);

// Thread-safe snapshot copy of the latest battery status.
bool pylontech_get_status(PylontechState* out);
// Thread-safe read of the battery data validity flag.
bool pylontech_data_valid();
