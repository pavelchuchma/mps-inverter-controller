#pragma once
#include <Arduino.h>
#include "config.h"

// --- Boiler element power control ---
// Two 1 kW resistive elements (R1, R2 @ 230 V AC) and three SPDT relays
// (A, B, C). Convention: state = 1 = relay energized = COM↔NO,
//                       state = 0 = relay de-energized = COM↔NC.
//
// Wiring (see doc/heater_relay_control_spec.md and doc/boiler_relay_schema.png):
//   L → A.COM,  N → B.COM,  C.COM → R1.top
//   R1.bottom = R2.top  (node Y);  B.NO → Y
//   C.NO = A.NO = R2.bottom (node X)
//   C.NC = B.NC (node Z);  A.NC unused
//
// Canonical states (A, B, C):
//   OFF      (0, 0, 0)  – 0 W
//   500 W    (1, 0, 0)  – R1 + R2 in series
//   1000 W   (1, 1, 0)  – only R2
//   2000 W   (1, 1, 1)  – R1 || R2
//
// Safety: toggling C while (B==0 && A==1) shorts L–N through C's
// transition arc. The state machine therefore moves only along the
// linear chain OFF ↔ 500 W ↔ 1000 W ↔ 2000 W, one relay per step,
// with a settling delay between steps.
//
// API is non-blocking: setBoilerPower() only stores the target, and
// tickBoiler() — called from the main loop — advances by exactly one
// rank per tick, no faster than RELAY_SETTLE_MS apart. getBoilerPower()
// returns the actually-achieved state (used by the inverter control).

enum BoilerPower : uint8_t {
  BOILER_OFF    = 0,
  BOILER_500W   = 1,
  BOILER_1000W  = 2,
  BOILER_2000W  = 3,
};

// Individual heating relay setters (on = relay energized = COM↔NO).
inline void setRelayBoilerA(bool on) {
  digitalWrite(RELAY_BOILER_A, on ? HIGH : LOW);
}
inline bool isRelayBoilerAOn() {
  return digitalRead(RELAY_BOILER_A) == HIGH;
}

inline void setRelayBoilerB(bool on) {
  digitalWrite(RELAY_BOILER_B, on ? HIGH : LOW);
}
inline bool isRelayBoilerBOn() {
  return digitalRead(RELAY_BOILER_B) == HIGH;
}

inline void setRelayBoilerC(bool on) {
  digitalWrite(RELAY_BOILER_C, on ? HIGH : LOW);
}
inline bool isRelayBoilerCOn() {
  return digitalRead(RELAY_BOILER_C) == HIGH;
}

void boilerRelayInit();
void setBoilerPower(BoilerPower power);
void tickBoiler();
BoilerPower getBoilerPower();

// Reads the BOILER_ON_PIN input — true when boiler is reported as on
inline bool isBoilerOn() {
  return digitalRead(BOILER_ON_PIN) == LOW;
}

// --- Mobile charger relay ---
// HIGH = on (charging enabled), LOW = off
inline void setMobileCharger(bool on) {
  digitalWrite(RELAY_MOBILE_CHARGER, on ? HIGH : LOW);
}

inline bool isMobileChargerOn() {
  return digitalRead(RELAY_MOBILE_CHARGER) == HIGH;
}
