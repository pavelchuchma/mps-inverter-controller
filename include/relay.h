#pragma once
#include <Arduino.h>
#include "config.h"

// --- Boiler element power control ---
// Two 1kW elements (E1, E2) in series, controlled by 3 relays:
//   RELAY_BOILER_L1       — end of E1:  OFF=N, ON=L
//   RELAY_BOILER_L2       — end of E2:  OFF=N, ON=L
//   RELAY_BOILER_COMMON_N — junction:   OFF=disconnected, ON=N
//
// Power levels:
//   0W   — all off
//   500W — both elements in series (half voltage each)
//   1000W — one element at full voltage
//   2000W — both elements at full voltage (parallel via common N)
//
// For 500W and 1000W, two equivalent relay configurations exist.
// Round-robin alternates between them to even out element and relay wear.

enum BoilerPower : uint8_t {
  BOILER_OFF    = 0,
  BOILER_500W   = 1,
  BOILER_1000W  = 2,
  BOILER_2000W  = 3,
};

// Individual heating relay setters (on = relay energized)
// Element relays: HIGH = energized (on), LOW = not energized (off)
inline void setRelayBoilerL1(bool on) {
  digitalWrite(RELAY_BOILER_L1, on ? HIGH : LOW);
}
inline bool isRelayBoilerL1On() {
  return digitalRead(RELAY_BOILER_L1) == HIGH;
}

inline void setRelayBoilerL2(bool on) {
  digitalWrite(RELAY_BOILER_L2, on ? HIGH : LOW);
}
inline bool isRelayBoilerL2On() {
  return digitalRead(RELAY_BOILER_L2) == HIGH;
}

inline void setRelayBoilerCommonN(bool on) {
  digitalWrite(RELAY_BOILER_COMMON_N, on ? HIGH : LOW);
}
inline bool isRelayBoilerCommonNOn() {
  return digitalRead(RELAY_BOILER_COMMON_N) == HIGH;
}

void boilerRelayInit();
void setBoilerPower(BoilerPower power);
BoilerPower getBoilerPower();

// --- Mobile charger relay ---
// HIGH = on (charging enabled), LOW = off
inline void setMobileCharger(bool on) {
  digitalWrite(RELAY_MOBILE_CHARGER, on ? HIGH : LOW);
}

inline bool isMobileChargerOn() {
  return digitalRead(RELAY_MOBILE_CHARGER) == HIGH;
}
