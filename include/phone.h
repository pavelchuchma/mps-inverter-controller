#pragma once
#include <Arduino.h>

// Polling interval (ms) between GETs to the phone status endpoint.
#define PHONE_POLL_INTERVAL_MS 30000
// HTTP timeout per single request attempt (ms). Kept short so a transient
// RF outage doesn't burn the whole poll window on one stuck TCP handshake.
#define PHONE_HTTP_TIMEOUT_MS  5000
// Max attempts per poll. A 5-minute interference burst typically loses only
// a handful of SYNs; 3 attempts recover most polls without long stalls.
#define PHONE_HTTP_MAX_ATTEMPTS 3
// Backoff between attempts (ms).
#define PHONE_HTTP_RETRY_DELAY_MS 1000
// TCP port and path served by the Termux status_server on the phone.
#define PHONE_STATUS_PORT      8080
#define PHONE_STATUS_PATH      "/status"

// Returns the current phone IP as a dotted-quad string. Stub: hardcoded
// LAN address. Will later be replaced by dynamic discovery.
const char* getPhoneIP();

// Parsed status snapshot from http://<phone-ip>:8080/status
struct PhoneState {
  // Battery
  bool   battery_present;
  char   battery_technology[16];     // e.g. "Li-ion"
  char   battery_health[16];         // e.g. "GOOD"
  char   battery_plugged[16];        // "UNPLUGGED" / "AC" / "USB" / ...
  char   battery_status[16];         // "DISCHARGING" / "CHARGING" / ...
  float  battery_temperature_c;
  int    battery_voltage_mv;
  int    battery_current_ua;         // raw value from Android (µA, signed)
  int    battery_current_average_ua;
  int    battery_percentage;         // 0..100
  int    battery_level;
  int    battery_scale;
  int    battery_charge_counter;     // µAh
  float  battery_stale_secs;

  // Network
  char     network_since[32];        // ISO timestamp string from endpoint
  uint64_t net_wlan0_rx_bytes;
  uint64_t net_wlan0_tx_bytes;
  uint64_t net_rmnet0_rx_bytes;
  uint64_t net_rmnet0_tx_bytes;
  float    network_stale_secs;

  uint32_t ts_ms;                    // millis() when snapshot was updated
};

// Global state updated by the background poll task.
extern PhoneState g_phone_status;
extern bool       g_phone_data_valid;

// Start the background polling task. Call once after WiFi is up.
void phone_comm_init();

// Thread-safe accessor: copies current snapshot into *out.
bool phone_get_status(PhoneState* out);
