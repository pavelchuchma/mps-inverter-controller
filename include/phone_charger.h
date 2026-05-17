#pragma once
#include <Arduino.h>

// Hysteresis thresholds for the phone-battery regulator (percent).
// Charging is enabled when SoC falls to LOW and disabled when it reaches
// HIGH; in between, the current relay state is kept (avoids relay chatter).
#define PHONE_BATT_LOW   25
#define PHONE_BATT_HIGH  80

// If the latest successful phone status snapshot is older than this, the
// regulator forces the charger ON as a safe default. 5 minutes covers
// ~10 lost polls at the 30 s phone-task cadence.
#define PHONE_STATUS_STALE_MS  (5UL * 60UL * 1000UL)

// Periodic regulator tick (call from the main task scheduler, ~10 s period).
// Reads the latest phone snapshot and drives RELAY_MOBILE_CHARGER:
//   * !valid OR snapshot older than PHONE_STATUS_STALE_MS  -> ON  (safe default)
//   * battery_status == "FULL"                             -> OFF
//   * battery_percentage <= PHONE_BATT_LOW                 -> ON
//   * battery_percentage >= PHONE_BATT_HIGH                -> OFF
//   * otherwise                                             -> keep current
// Logs every ON<->OFF transition via printWarning().
void tickPhoneCharger();
